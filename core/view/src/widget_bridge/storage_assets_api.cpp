// widget_bridge/storage_assets_api.cpp - storage and asset-loading registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/asset_manager.hpp>
#include <pulp/runtime/base64.hpp>
#include "api_registry.hpp"

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
    if (rel.empty() || rel.is_absolute()) return false;
    for (const auto& part : rel) {
        const auto s = part.string();
        if (s.empty() || s == "." || s == "..") return false;
    }
    return true;
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

BridgeAssetLoad load_bridge_asset(const std::string& url,
                                  const std::vector<std::filesystem::path>& asset_roots) {
    BridgeAssetLoad result;
    if (url.empty()) {
        result.status = 400;
        return result;
    }

    auto& assets = AssetManager::instance();
    const bool reviewed_roots_only = !asset_roots.empty();

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
        if (reviewed_roots_only) {
            result.status = 403;
            return result;
        }
        result.blob = load_from_file(url.substr(7));
    } else {
        result.blob = load_from_embedded(url);
        if (!result.blob.valid()) {
            result.blob = load_from_asset_roots(url);
        }
        if (!result.blob.valid() && !reviewed_roots_only) {
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

void WidgetBridge::register_storage_key_value_api() {
    BridgeApiContext api{engine_};

    // localStorage equivalent - file-based key-value in plugin data dir.
    register_bridge_function(api, "storageGetItem", [](choc::javascript::ArgumentList args) {
        auto key = args.get<std::string>(0, "");
        if (key.empty()) return choc::value::createString("");
        auto dir = std::filesystem::temp_directory_path() / "pulp-storage";
        auto path = dir / (key + ".dat");
        if (!std::filesystem::exists(path)) return choc::value::createString("");
        std::ifstream f(path);
        std::string val((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return choc::value::createString(val);
    });

    register_bridge_function(api, "storageSetItem", [](choc::javascript::ArgumentList args) {
        auto key = args.get<std::string>(0, "");
        auto val = args.get<std::string>(1, "");
        if (key.empty()) return choc::value::Value();
        auto dir = std::filesystem::temp_directory_path() / "pulp-storage";
        std::filesystem::create_directories(dir);
        std::ofstream f(dir / (key + ".dat"));
        f << val;
        return choc::value::Value();
    });

    register_bridge_function(api, "storageRemoveItem", [](choc::javascript::ArgumentList args) {
        auto key = args.get<std::string>(0, "");
        if (key.empty()) return choc::value::Value();
        auto dir = std::filesystem::temp_directory_path() / "pulp-storage";
        std::filesystem::remove(dir / (key + ".dat"));
        return choc::value::Value();
    });
}

void WidgetBridge::register_asset_loading_api() {
    BridgeApiContext api{engine_};

    register_bridge_function(api, "__loadAssetSync__", [this](choc::javascript::ArgumentList args) {
        auto url = args.get<std::string>(0, "");
        auto asset = load_bridge_asset(url, asset_roots_);

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

void WidgetBridge::register_font_assets_api() {
    BridgeApiContext api{engine_};

    // Font loading: loadFont(path) -> success boolean.
    // This is an existence probe for web-compat callers; actual renderer font
    // registration is handled by registerFont(family, path) below.
    register_bridge_function(api, "loadFont", [](choc::javascript::ArgumentList args) {
        auto path = args.get<std::string>(0, "");
        bool exists = !path.empty() && std::filesystem::exists(path);
        return choc::value::createBool(exists);
    });

    // registerFont(family, path) - register a bundled .ttf/.otf with the text
    // renderer under `family`, so set_font() / Label font_family resolve to the
    // shipped face instead of a same-named system font (or a generic fallback).
    // Codegen emits these from the envelope's font_family_assets before any
    // setFontFamily.
    register_bridge_function(api, "registerFont", [](choc::javascript::ArgumentList args) {
        auto family = args.get<std::string>(0, "");
        auto path = args.get<std::string>(1, "");
        if (family.empty() || path.empty()) return choc::value::createBool(false);
        if (path.rfind("file://", 0) == 0) path = path.substr(7);
        if (!std::filesystem::exists(path)) return choc::value::createBool(false);
        // register_font_family decodes the file + registers the typeface with
        // the canvas font registry (Skia on GPU builds; no-op stub otherwise).
        AssetManager::instance().register_font_family(family, path);
        return choc::value::createBool(true);
    });
}

} // namespace pulp::view
