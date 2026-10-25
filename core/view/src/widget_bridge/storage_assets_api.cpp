// widget_bridge/storage_assets_api.cpp - storage and asset-loading registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/asset_manager.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/log.hpp>
#include "api_registry.hpp"
#include "private_storage.hpp"

#include <pulp/runtime/safe_relative_path.hpp>

#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace pulp::view {

namespace {

std::string strip_leading_slashes(std::string path) {
    while (!path.empty() && (path.front() == '/' || path.front() == '\\')) {
        path.erase(path.begin());
    }
    return path;
}

std::string canonical_bridge_asset_mime_type(std::string mime_type) {
    if (mime_type == "application/json" || mime_type == "text/json") {
        return "application/json;charset=utf-8";
    }
    return mime_type;
}

std::string guess_bridge_asset_mime_type(const std::string& path) {
    auto dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? std::string{} : path.substr(dot);
    for (auto& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".js" || ext == ".mjs") return "text/javascript";
    if (ext == ".css") return "text/css";
    if (ext == ".json") return "application/json";
    if (ext == ".txt" || ext == ".wgsl" || ext == ".sksl") return "text/plain";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".webp") return "image/webp";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".woff") return "font/woff";
    if (ext == ".ttf") return "font/ttf";
    return "application/octet-stream";
}

bool bridge_asset_is_text_like(const std::string& mime_type) {
    return mime_type.rfind("text/", 0) == 0
        || mime_type.find("json") != std::string::npos
        || mime_type.find("javascript") != std::string::npos
        || mime_type.find("xml") != std::string::npos
        || mime_type.find("svg") != std::string::npos;
}

bool safe_relative_asset_path(const std::filesystem::path& rel) {
    return pulp::runtime::is_safe_relative_path(rel);
}

bool path_within(const std::filesystem::path& path, const std::filesystem::path& root) {
    auto p = path.lexically_normal();
    auto r = root.lexically_normal();
    auto pit = p.begin();
    auto rit = r.begin();
    for (; rit != r.end(); ++rit, ++pit) {
        if (pit == p.end() || *pit != *rit) return false;
    }
    return true;
}

std::filesystem::path best_effort_canonical(std::filesystem::path path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) return canonical.lexically_normal();
    ec.clear();
    canonical = std::filesystem::absolute(path, ec);
    if (!ec) return canonical.lexically_normal();
    return path.lexically_normal();
}

struct BridgeAssetLoad {
    bool ok = false;
    int status = 404;
    std::string resolved_path;
    std::string mime_type;
    BlobData blob;
};

bool looks_like_absolute_file_path(const std::string& path) {
    if (path.empty())
        return false;
    if (path.front() == '/' || path.front() == '\\')
        return true;
    return path.size() > 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':' &&
           (path[2] == '/' || path[2] == '\\');
}

// The filesystem path a `file://` URL names. `file:///C:/x` names `C:/x`.
std::string strip_file_scheme(const std::string& url) {
    std::string path = url.substr(7);
#if defined(_WIN32)
    if (path.size() > 2 && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1])) &&
        path[2] == ':')
        path.erase(path.begin());
#endif
    return path;
}

// Whether `path` is a file larger than the asset manager's per-file ceiling.
// Every bridge API that hands a resolved path to a loader checks this, so the
// ceiling holds for loaders that read the file themselves.
bool exceeds_read_ceiling(const std::filesystem::path& path) {
    const auto ceiling = AssetManager::instance().max_file_bytes();
    if (ceiling == 0)
        return false;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return !ec && size > ceiling;
}

// Report a path the bridge declined to open. Bounded so a script that retries
// in a loop cannot flood the log.
void log_refused_file_path(const std::string& path) {
    static std::atomic<int> reported{0};
    constexpr int kMaxReports = 32;
    const int n = reported.fetch_add(1, std::memory_order_relaxed);
    if (n < kMaxReports) {
        runtime::log_warn("WidgetBridge: '{}' is not a file under the script directory or a "
                          "reviewed asset root; not opened",
                          path);
    } else if (n == kMaxReports) {
        runtime::log_warn("WidgetBridge: further refused file paths are not reported");
    }
}

// A `file://` URL or an absolute path is judged by
// WidgetBridge::resolve_script_relative(), the same rule every other
// path-taking bridge API applies: it loads when it resolves inside the script
// directory or a reviewed asset root (or the host opted into unreviewed
// paths), and is refused with 403 otherwise. A relative name that matches no
// embedded asset and no root falls back to the filesystem only under that
// opt-in.
BridgeAssetLoad load_bridge_asset(const std::string& url,
                                  const std::vector<std::filesystem::path>& asset_roots,
                                  const WidgetBridge& bridge) {
    BridgeAssetLoad result;
    if (url.empty()) {
        result.status = 400;
        return result;
    }

    auto& assets = AssetManager::instance();
    const bool reviewed_roots_only = !bridge.allow_unreviewed_file_paths();

    auto load_from_file = [&](std::string path) {
        if (path.empty()) {
            return BlobData{};
        }
#if defined(_WIN32)
        if (path.size() > 2 && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == ':') {
            path.erase(path.begin());
        }
#endif
        result.resolved_path = path;
        return assets.load_blob(path);
    };

    auto load_from_embedded = [&](std::string name) {
        name = strip_leading_slashes(std::move(name));
        if (name.empty()) {
            return BlobData{};
        }
        result.resolved_path = name;
        if (assets.has_embedded(name)) {
            return assets.load_blob_embedded(name);
        }
        return BlobData{};
    };

    auto load_from_asset_roots = [&](std::string rel) {
        rel = strip_leading_slashes(std::move(rel));
        const std::filesystem::path rel_path(rel);
        if (!safe_relative_asset_path(rel_path)) {
            return BlobData{};
        }
        for (const auto& root : asset_roots) {
            auto root_norm = best_effort_canonical(root);
            auto candidate = best_effort_canonical(root / rel_path);
            if (!path_within(candidate, root_norm)) continue;
            auto blob = load_from_file(candidate.string());
            if (blob.valid()) return blob;
        }
        return BlobData{};
    };

    if (url.rfind("pulp://", 0) == 0) {
        auto ref = url.substr(7);
        result.blob = load_from_embedded(ref);
        if (!result.blob.valid()) {
            result.blob = load_from_asset_roots(ref);
        }
        if (!result.blob.valid() && !reviewed_roots_only) {
            result.blob = load_from_file(strip_leading_slashes(ref));
        }
    } else if (url.rfind("file://", 0) == 0) {
        const auto path = bridge.resolve_script_relative(url);
        if (path.empty()) {
            result.status = 403;
            return result;
        }
        result.blob = load_from_file(path);
    } else {
        result.blob = load_from_embedded(url);
        if (!result.blob.valid()) {
            result.blob = load_from_asset_roots(url);
        }
        if (!result.blob.valid() && looks_like_absolute_file_path(url)) {
            const auto path = bridge.resolve_script_relative(url);
            if (path.empty()) {
                result.status = 403;
                return result;
            }
            result.blob = load_from_file(path);
        } else if (!result.blob.valid() && !reviewed_roots_only) {
            result.blob = load_from_file(url);
        }
    }

    if (!result.blob.valid()) {
        result.status = 404;
        return result;
    }

    result.ok = true;
    result.status = 200;
    result.mime_type = canonical_bridge_asset_mime_type(
        guess_bridge_asset_mime_type(result.resolved_path.empty() ? url : result.resolved_path));
    return result;
}

std::string base64_encode_blob(const std::vector<uint8_t>& data) {
    return runtime::base64_encode(data.data(), data.size());
}

} // namespace

bool WidgetBridge::permits_file_path(const std::filesystem::path& path) const {
    if (path.empty())
        return false;
    if (!granted_capabilities_.has(ReloadCapability::Filesystem))
        return false;
    if (allow_unreviewed_file_paths_)
        return true;
    const auto candidate = best_effort_canonical(path);
    if (!script_base_dir_.empty() &&
        path_within(candidate, best_effort_canonical(script_base_dir_)))
        return true;
    for (const auto& root : asset_roots_) {
        if (path_within(candidate, best_effort_canonical(root)))
            return true;
    }
    return false;
}

std::string WidgetBridge::resolve_script_relative(const std::string& path) const {
    if (path.empty())
        return path;
    const auto refuse = [&path] {
        log_refused_file_path(path);
        return std::string{};
    };
    if (path.rfind("file://", 0) == 0)
        return resolve_script_relative(strip_file_scheme(path));
    // Callers open the result as a file, so any other scheme would be read as
    // a path relative to the process working directory.
    if (path.find("://") != std::string::npos)
        return allow_unreviewed_file_paths_ ? path : refuse();

    const std::filesystem::path requested(path);
    if (requested.is_absolute() || looks_like_absolute_file_path(path)) {
        if (!permits_file_path(requested) || exceeds_read_ceiling(requested))
            return refuse();
        return path;
    }
    // A relative path that climbs out of its directory is refused outright.
    if (!safe_relative_asset_path(requested))
        return refuse();

    // Containment is judged on canonical forms, so a symlink cannot escape.
    // The script-directory result keeps the caller's spelling; a root result
    // is canonical.
    if (!script_base_dir_.empty()) {
        const auto candidate = (script_base_dir_ / requested).lexically_normal();
        std::error_code ec;
        if (path_within(best_effort_canonical(candidate),
                        best_effort_canonical(script_base_dir_)) &&
            std::filesystem::exists(candidate, ec) && !ec)
            return exceeds_read_ceiling(candidate) ? refuse() : candidate.generic_string();
    }

    for (const auto& root : asset_roots_) {
        const auto canonical_root = best_effort_canonical(root);
        const auto candidate = best_effort_canonical(root / requested);
        if (!path_within(candidate, canonical_root)) continue;
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec)
            return exceeds_read_ceiling(candidate) ? refuse() : candidate.generic_string();
    }

    // Not found under any root. Only a host that opted into unreviewed paths
    // gets the name back unchanged, to be opened relative to its working
    // directory.
    return allow_unreviewed_file_paths_ ? path : refuse();
}

void BridgeRegistrars::register_storage_key_value_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // localStorage equivalent - one hashed file per key under the per-user,
    // per-bridge store (see private_storage.hpp). An empty dir means no
    // per-user root is known and every call is a safe no-op.
    const auto entry_path = [&self](const std::string& key) -> std::filesystem::path {
        const auto& dir = detail::cached_private_dir(self.private_store_dirs_,
                                                     detail::PrivateStoreKind::WidgetStorage,
                                                     self.script_base_dir_);
        if (dir.empty())
            return {};
        return dir / detail::storage_key_file_name(key);
    };

    register_bridge_function(
        api, "storageGetItem", [entry_path](choc::javascript::ArgumentList args) {
            auto key = args.get<std::string>(0, "");
            if (key.empty())
                return choc::value::createString("");
            const auto path = entry_path(key);
            if (path.empty())
                return choc::value::createString("");
            return choc::value::createString(detail::read_private_file(path).value_or(""));
        });

    register_bridge_function(api, "storageSetItem",
                             [entry_path](choc::javascript::ArgumentList args) {
                                 auto key = args.get<std::string>(0, "");
                                 auto val = args.get<std::string>(1, "");
                                 if (key.empty())
                                     return choc::value::Value();
                                 const auto path = entry_path(key);
                                 if (!path.empty())
                                     detail::write_private_file(path, val);
                                 return choc::value::Value();
                             });

    register_bridge_function(api, "storageRemoveItem",
                             [entry_path](choc::javascript::ArgumentList args) {
                                 auto key = args.get<std::string>(0, "");
                                 if (key.empty())
                                     return choc::value::Value();
                                 const auto path = entry_path(key);
                                 if (path.empty())
                                     return choc::value::Value();
                                 std::error_code ec;
                                 std::filesystem::remove(path, ec);
                                 return choc::value::Value();
                             });
}

void BridgeRegistrars::register_asset_loading_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    register_bridge_function(api, "__loadAssetSync__", [&self](choc::javascript::ArgumentList args) {
        auto url = args.get<std::string>(0, "");
        // Generated runtime packages place reviewed sidecars beside the entry
        // script. Treat that directory as the first bounded asset root, which
        // matches resolve_script_relative() and avoids requiring every host to
        // redundantly repeat script_path.parent_path() in asset_roots.
        auto roots = self.asset_roots_;
        if (!self.script_base_dir_.empty())
            roots.insert(roots.begin(), self.script_base_dir_);
        auto asset = load_bridge_asset(url, roots, self);

        auto result = choc::value::createObject("");
        result.addMember("ok", choc::value::createBool(asset.ok));
        result.addMember("status", choc::value::createInt32(asset.status));
        result.addMember("url", choc::value::createString(url));
        result.addMember("resolvedPath", choc::value::createString(asset.resolved_path));
        result.addMember("contentType", choc::value::createString(asset.mime_type));
        result.addMember("base64", choc::value::createString(asset.ok ? base64_encode_blob(asset.blob.data) : ""));
        if (asset.ok && bridge_asset_is_text_like(asset.mime_type)) {
            result.addMember("text", choc::value::createString(
                std::string(asset.blob.data.begin(), asset.blob.data.end())));
        } else {
            result.addMember("text", choc::value::createString(""));
        }
        return result;
    });
}

void BridgeRegistrars::register_font_assets_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // Font loading: loadFont(path) -> success boolean.
    // This is an existence probe for web-compat callers; actual renderer font
    // registration is handled by registerFont(family, path) below.
    register_bridge_function(api, "loadFont", [&self](choc::javascript::ArgumentList args) {
        auto path = self.resolve_script_relative(args.get<std::string>(0, ""));
        bool exists = !path.empty() && std::filesystem::exists(path);
        return choc::value::createBool(exists);
    });

    // registerFont(family, path) - register a bundled .ttf/.otf with the text
    // renderer under `family`, so set_font() / Label font_family resolve to the
    // shipped face instead of a same-named system font (or a generic fallback).
    // Codegen emits these from the envelope's font_family_assets before any
    // setFontFamily.
    register_bridge_function(api, "registerFont", [&self](choc::javascript::ArgumentList args) {
        auto family = args.get<std::string>(0, "");
        auto path = args.get<std::string>(1, "");
        if (family.empty() || path.empty()) return choc::value::createBool(false);
        path = self.resolve_script_relative(path);
        if (path.empty() || !std::filesystem::exists(path))
            return choc::value::createBool(false);
        // register_font_family decodes the file + registers the typeface with
        // the canvas font registry (Skia on GPU builds; no-op stub otherwise).
        AssetManager::instance().register_font_family(family, path);
        return choc::value::createBool(true);
    });
}

} // namespace pulp::view
