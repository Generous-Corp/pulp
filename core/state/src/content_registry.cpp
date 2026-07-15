#include <pulp/state/content_registry.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/system.hpp>

#include <choc/text/choc_JSON.h>

#include <miniz.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>

namespace fs = std::filesystem;

namespace pulp::state {

namespace {

std::string read_text(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool write_text(const fs::path& path, const std::string& body) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // Atomic write: stage to a sibling temp file, fully flush it, then rename it
    // onto the destination so a crash / power loss / full disk mid-write never
    // truncates an existing file (e.g. the content index). Mirrors
    // PropertiesFile::save_to and the skp_capture / waveform_overview_cache
    // temp-then-rename convention.
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp);
        if (!f.is_open()) return false;
        f << body;
        f.flush();
        if (!f.good()) {
            f.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        std::error_code rm_ec;
        fs::remove(tmp, rm_ec);
        return false;
    }
    return true;
}

std::string string_member(const choc::value::ValueView& object, std::string_view name) {
    auto value = object[name];
    return value.isString() ? std::string(value.getString()) : std::string{};
}

std::vector<std::string> string_array_member(const choc::value::ValueView& object,
                                             std::string_view name) {
    std::vector<std::string> out;
    auto value = object[name];
    if (!value.isArray()) return out;
    for (uint32_t i = 0; i < value.size(); ++i) {
        auto item = value[i];
        if (item.isString()) out.emplace_back(item.getString());
    }
    return out;
}

bool contains(const std::vector<std::string>& values, std::string_view needle) {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

bool intersects(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    for (const auto& value : a) {
        if (contains(b, value)) return true;
    }
    return false;
}

bool safe_rel_path(const fs::path& rel) {
    if (rel.empty() || rel.is_absolute()) return false;
    for (const auto& part : rel) {
        const auto s = part.string();
        if (s.empty() || s == "." || s == "..") return false;
    }
    return true;
}

bool safe_id_component(std::string_view value) {
    if (value.empty() || value == "." || value == "..") return false;
    for (const unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            continue;
        return false;
    }
    return true;
}

bool path_within(const fs::path& path, const fs::path& root) {
    auto p = path.lexically_normal();
    auto r = root.lexically_normal();
    auto pit = p.begin();
    auto rit = r.begin();
    for (; rit != r.end(); ++rit, ++pit) {
        if (pit == p.end() || *pit != *rit) return false;
    }
    return true;
}

bool all_in(const std::vector<std::string>& values,
            const std::vector<std::string>& allowed,
            std::string* offending = nullptr) {
    for (const auto& value : values) {
        if (!contains(allowed, value)) {
            if (offending) *offending = value;
            return false;
        }
    }
    return true;
}

std::string json_escape(std::string_view s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string json_string(std::string_view s) {
    return "\"" + json_escape(s) + "\"";
}

std::string strings_json(const std::vector<std::string>& values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out += ",";
        out += json_string(values[i]);
    }
    out += "]";
    return out;
}

std::string now_string() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

void append_files_under(const fs::path& pack_root,
                        const std::vector<std::string>& declared_paths,
                        std::vector<fs::path>& out) {
    for (const auto& declared : declared_paths) {
        const auto path = pack_root / fs::path(declared);
        std::error_code ec;
        if (fs::is_regular_file(path, ec)) {
            out.push_back(path);
            continue;
        }
        if (!fs::is_directory(path, ec)) continue;

        for (fs::recursive_directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec)) out.push_back(it->path());
        }
    }

    std::sort(out.begin(), out.end());
}

struct LocalContentManifest {
    fs::path source;
    fs::path root;
    bool archive = false;
    bool manifest_file = false;
    std::string json;
    std::string manifest_sha256;
    ContentPackInfo pack;
    std::vector<std::string> exported_kind_names;
    std::vector<std::string> exported_paths;
};

std::vector<fs::path> preset_files(const std::vector<fs::path>& paths) {
    std::vector<fs::path> out;
    std::copy_if(paths.begin(), paths.end(), std::back_inserter(out), [](const fs::path& path) {
        return path.extension() == ".json";
    });
    return out;
}

void validate_export_paths(const std::vector<std::string>& paths,
                           std::vector<std::string>& issues) {
    for (const auto& rel : paths) {
        if (!safe_rel_path(fs::path(rel)))
            issues.push_back("unsafe-export: " + rel);
    }
}

void validate_export_paths_exist(const fs::path& root,
                                 const std::vector<std::string>& paths,
                                 std::vector<std::string>& issues) {
    std::error_code ec;
    auto root_norm = fs::weakly_canonical(root, ec);
    if (ec) root_norm = root.lexically_normal();
    for (const auto& declared : paths) {
        const fs::path rel(declared);
        ec.clear();
        auto path = fs::weakly_canonical(root / rel, ec);
        if (ec) path = (root / rel).lexically_normal();
        if (!safe_rel_path(rel) || !path_within(path, root_norm) || !fs::exists(path)) {
            issues.push_back("missing-path: `" + rel.generic_string()
                             + "` references missing or unsafe path `" + rel.generic_string() + "`");
        }
    }
}

bool validate_directory_entries(const fs::path& root, std::vector<std::string>& issues) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const auto rel = fs::relative(it->path(), root, ec);
        if (ec || !safe_rel_path(rel)) {
            issues.push_back("source-entry: unsafe path under content pack root");
            return false;
        }
        if (fs::is_symlink(it->symlink_status(ec))) {
            issues.push_back("source-entry: symlinks are not allowed in content packs");
            return false;
        }
    }
    if (ec) {
        issues.push_back("source-entry: failed to scan content pack root: " + ec.message());
        return false;
    }
    return true;
}

bool zip_read_file(mz_zip_archive& zip, const char* name, std::string& out) {
    size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&zip, name, &size, 0);
    if (!data) return false;
    out.assign(static_cast<const char*>(data), size);
    mz_free(data);
    return true;
}

bool archive_contains_path(const std::vector<std::string>& archived_payloads,
                           const fs::path& declared_path) {
    const auto normalized = declared_path.lexically_normal().generic_string();
    if (normalized.empty() || normalized == ".") return true;
    const auto prefix = normalized + "/";
    return std::any_of(archived_payloads.begin(), archived_payloads.end(), [&](const auto& file) {
        return file == normalized || file.rfind(prefix, 0) == 0;
    });
}

bool validate_archive_entries(const fs::path& archive,
                              std::vector<std::string>& issues,
                              const std::vector<std::string>* required_paths = nullptr) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, archive.string().c_str(), 0)) {
        issues.push_back("archive-open: failed to open " + archive.string());
        return false;
    }

    bool ok = true;
    std::vector<std::string> archived_payloads;
    const auto count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            issues.push_back("archive-entry: failed to read entry metadata");
            ok = false;
            continue;
        }
        const fs::path rel(stat.m_filename);
        if (!safe_rel_path(rel)) {
            issues.push_back("archive-entry: unsafe path `" + rel.generic_string() + "`");
            ok = false;
        }
        if (!stat.m_is_directory && rel.generic_string() != "files.sha256.json")
            archived_payloads.push_back(rel.generic_string());
    }

    if (required_paths) {
        for (const auto& required : *required_paths) {
            const fs::path rel(required);
            if (!safe_rel_path(rel) || !archive_contains_path(archived_payloads, rel)) {
                issues.push_back("missing-path: `" + rel.generic_string()
                                 + "` references missing or unsafe path `" + rel.generic_string() + "`");
                ok = false;
            }
        }
    }

    std::string sha_json;
    if (!zip_read_file(zip, "files.sha256.json", sha_json)) {
        issues.push_back("sha256: missing files.sha256.json");
        ok = false;
    } else {
        try {
            auto sha_root = choc::json::parse(sha_json);
            auto files = sha_root["files"];
            if (files.isObject()) {
                std::vector<std::string> declared_files;
                for (uint32_t i = 0; i < files.size(); ++i) {
                    const auto name = std::string(files.getObjectMemberAt(i).name);
                    auto value = files.getObjectMemberAt(i).value;
                    if (!value.isString() || !safe_rel_path(fs::path(name))) {
                        issues.push_back("sha256: invalid entry `" + name + "`");
                        ok = false;
                        continue;
                    }
                    declared_files.push_back(name);
                    size_t size = 0;
                    void* data = mz_zip_reader_extract_file_to_heap(&zip, name.c_str(), &size, 0);
                    if (!data) {
                        issues.push_back("sha256: missing archived file `" + name + "`");
                        ok = false;
                        continue;
                    }
                    const auto hash = pulp::runtime::sha256_hex(
                        static_cast<const unsigned char*>(data), size);
                    mz_free(data);
                    if (std::string(value.getString()) != "sha256-" + hash) {
                        issues.push_back("sha256: digest mismatch for `" + name + "`");
                        ok = false;
                    }
                }
                for (const auto& name : archived_payloads) {
                    if (std::find(declared_files.begin(), declared_files.end(), name) == declared_files.end()) {
                        issues.push_back("sha256: unlisted archived file `" + name + "`");
                        ok = false;
                    }
                }
            } else {
                issues.push_back("sha256: invalid files.sha256.json");
                ok = false;
            }
        } catch (...) {
            issues.push_back("sha256: invalid files.sha256.json");
            ok = false;
        }
    }
    mz_zip_reader_end(&zip);
    return ok;
}

bool extract_archive_content(const fs::path& archive,
                             const fs::path& dest_root,
                             std::string& error) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, archive.string().c_str(), 0)) {
        error = "Failed to open archive " + archive.string();
        return false;
    }

    std::error_code ec;
    fs::create_directories(dest_root, ec);
    if (ec) {
        mz_zip_reader_end(&zip);
        error = "Failed to create install root: " + ec.message();
        return false;
    }

    auto root_norm = fs::absolute(dest_root, ec).lexically_normal();
    if (ec) root_norm = dest_root.lexically_normal();
    const auto count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        const fs::path rel(stat.m_filename);
        if (!safe_rel_path(rel)) {
            mz_zip_reader_end(&zip);
            error = "Refusing unsafe archive path `" + rel.generic_string() + "`";
            return false;
        }
        auto dest = fs::absolute(dest_root / rel, ec).lexically_normal();
        if (ec) dest = (dest_root / rel).lexically_normal();
        if (!path_within(dest, root_norm)) {
            mz_zip_reader_end(&zip);
            error = "Refusing archive path outside install root `" + rel.generic_string() + "`";
            return false;
        }
        fs::create_directories(dest.parent_path(), ec);
        if (ec || !mz_zip_reader_extract_to_file(&zip, i, dest.string().c_str(), 0)) {
            mz_zip_reader_end(&zip);
            error = "Failed to extract `" + rel.generic_string() + "`";
            return false;
        }
    }
    mz_zip_reader_end(&zip);
    return true;
}

bool copy_declared_content(const fs::path& source_root,
                           const fs::path& dest_root,
                           const std::vector<std::string>& declared_paths,
                           std::string& error) {
    std::vector<std::pair<fs::path, fs::path>> files;
    auto add_file = [&](const fs::path& source, const fs::path& rel) -> bool {
        if (!safe_rel_path(rel)) {
            error = "Unsafe source path while installing content";
            return false;
        }
        if (std::find_if(files.begin(), files.end(), [&](const auto& entry) {
                return entry.second == rel;
            }) == files.end()) {
            files.push_back({source, rel});
        }
        return true;
    };

    if (!add_file(source_root / "pulp.package.json", fs::path("pulp.package.json")))
        return false;

    std::error_code ec;
    for (const auto& declared : declared_paths) {
        const fs::path rel_root(declared);
        if (!safe_rel_path(rel_root)) {
            error = "Unsafe export path while installing content";
            return false;
        }
        const auto source = source_root / rel_root;
        if (fs::is_symlink(fs::symlink_status(source, ec))) {
            error = "Symlinks are not allowed in content packs";
            return false;
        }
        if (fs::is_regular_file(source, ec)) {
            if (!add_file(source, rel_root)) return false;
            continue;
        }
        if (!fs::is_directory(source, ec)) continue;
        for (fs::recursive_directory_iterator it(source, ec), end; !ec && it != end; it.increment(ec)) {
            if (fs::is_symlink(it->symlink_status(ec))) {
                error = "Symlinks are not allowed in content packs";
                return false;
            }
            if (!it->is_regular_file(ec)) continue;
            auto rel = fs::relative(it->path(), source_root, ec);
            if (ec || !add_file(it->path(), rel)) return false;
        }
        if (ec) {
            error = "Failed to scan content pack export: " + ec.message();
            return false;
        }
    }

    fs::create_directories(dest_root, ec);
    if (ec) {
        error = "Failed to create install root: " + ec.message();
        return false;
    }
    for (const auto& [source, rel] : files) {
        auto dest = dest_root / rel;
        fs::create_directories(dest.parent_path(), ec);
        if (ec) {
            error = "Failed to create " + dest.parent_path().string();
            return false;
        }
        fs::copy_file(source, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "Failed to copy `" + rel.generic_string() + "`: " + ec.message();
            return false;
        }
    }
    return true;
}

std::optional<ContentPackInfo> load_pack(const fs::path& root) {
    const auto manifest_text = read_text(root / "pulp.package.json");
    if (manifest_text.empty()) return std::nullopt;

    try {
        auto json = choc::json::parse(manifest_text);
        auto kinds = string_array_member(json, "kind");
        if (!contains(kinds, "content-pack")) return std::nullopt;

        ContentPackInfo pack;
        pack.id = string_member(json, "id");
        pack.name = string_member(json, "name");
        pack.version = string_member(json, "version");
        pack.root = root;
        pack.kinds = std::move(kinds);
        pack.capabilities = string_array_member(json, "capabilities");

        auto exports = json["exports"];
        if (exports.isObject()) {
            const auto presets = string_array_member(exports, "presets");
            const auto themes = string_array_member(exports, "themes");
            const auto samples = string_array_member(exports, "samples");
            const auto wavetables = string_array_member(exports, "wavetables");
            std::vector<std::string> issues;
            validate_export_paths_exist(root, presets, issues);
            validate_export_paths_exist(root, themes, issues);
            validate_export_paths_exist(root, samples, issues);
            validate_export_paths_exist(root, wavetables, issues);
            if (!issues.empty()) return std::nullopt;

            append_files_under(root, presets, pack.presets);
            append_files_under(root, themes, pack.themes);
            append_files_under(root, samples, pack.samples);
            append_files_under(root, wavetables, pack.wavetables);
        }

        if (pack.id.empty() || pack.version.empty()) return std::nullopt;
        pack.presets = preset_files(pack.presets);
        return pack;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::string> exported_kinds(const ContentPackInfo& pack) {
    std::vector<std::string> kinds;
    if (!pack.presets.empty()) kinds.emplace_back("presets");
    if (!pack.themes.empty()) kinds.emplace_back("themes");
    if (!pack.samples.empty()) kinds.emplace_back("samples");
    if (!pack.wavetables.empty()) kinds.emplace_back("wavetables");
    return kinds;
}

std::optional<LocalContentManifest> load_local_content_manifest(const fs::path& input,
                                                               std::vector<std::string>& issues) {
    LocalContentManifest manifest;
    manifest.source = input;

    std::error_code ec;
    if (fs::is_directory(input, ec)) {
        manifest.root = input;
        manifest.json = read_text(input / "pulp.package.json");
        validate_directory_entries(input, issues);
    } else if (input.extension() == ".pulpcontent" || input.extension() == ".zip") {
        manifest.archive = true;
        mz_zip_archive zip{};
        if (!mz_zip_reader_init_file(&zip, input.string().c_str(), 0)) {
            issues.push_back("archive-open: failed to open " + input.string());
            return std::nullopt;
        }
        if (!zip_read_file(zip, "pulp.package.json", manifest.json))
            issues.push_back("missing-field: pulp.package.json");
        mz_zip_reader_end(&zip);
    } else {
        manifest.manifest_file = true;
        manifest.root = input.parent_path();
        manifest.json = read_text(input);
    }

    if (manifest.json.empty()) {
        issues.push_back("missing-field: pulp.package.json");
        return std::nullopt;
    }
    manifest.manifest_sha256 = "sha256-" + pulp::runtime::sha256_hex(manifest.json);

    try {
        auto json = choc::json::parse(manifest.json);
        auto kinds = string_array_member(json, "kind");
        if (!contains(kinds, "content-pack"))
            issues.push_back("invalid-kind: content installer requires kind content-pack");

        manifest.pack.id = string_member(json, "id");
        manifest.pack.name = string_member(json, "name");
        manifest.pack.version = string_member(json, "version");
        manifest.pack.root = manifest.root;
        manifest.pack.kinds = std::move(kinds);
        manifest.pack.capabilities = string_array_member(json, "capabilities");

        if (manifest.pack.id.empty()) issues.push_back("missing-field: id");
        if (manifest.pack.version.empty()) issues.push_back("missing-field: version");
        if (!manifest.pack.id.empty() && !safe_id_component(manifest.pack.id))
            issues.push_back("invalid-id: content pack id is not a safe path component");
        if (!manifest.pack.version.empty() && !safe_id_component(manifest.pack.version))
            issues.push_back("invalid-version: content pack version is not a safe path component");

        auto exports = json["exports"];
        if (exports.isObject() && !manifest.archive) {
            const auto presets = string_array_member(exports, "presets");
            const auto themes = string_array_member(exports, "themes");
            const auto samples = string_array_member(exports, "samples");
            const auto wavetables = string_array_member(exports, "wavetables");
            const auto licenses = string_array_member(exports, "licenses");
            manifest.exported_paths.insert(manifest.exported_paths.end(), presets.begin(), presets.end());
            manifest.exported_paths.insert(manifest.exported_paths.end(), themes.begin(), themes.end());
            manifest.exported_paths.insert(manifest.exported_paths.end(), samples.begin(), samples.end());
            manifest.exported_paths.insert(manifest.exported_paths.end(), wavetables.begin(), wavetables.end());
            manifest.exported_paths.insert(manifest.exported_paths.end(), licenses.begin(), licenses.end());
            validate_export_paths(presets, issues);
            validate_export_paths(themes, issues);
            validate_export_paths(samples, issues);
            validate_export_paths(wavetables, issues);
            validate_export_paths(licenses, issues);
            validate_export_paths_exist(manifest.root, presets, issues);
            validate_export_paths_exist(manifest.root, themes, issues);
            validate_export_paths_exist(manifest.root, samples, issues);
            validate_export_paths_exist(manifest.root, wavetables, issues);
            validate_export_paths_exist(manifest.root, licenses, issues);
            append_files_under(manifest.root, presets, manifest.pack.presets);
            append_files_under(manifest.root, themes, manifest.pack.themes);
            append_files_under(manifest.root, samples, manifest.pack.samples);
            append_files_under(manifest.root, wavetables, manifest.pack.wavetables);
            manifest.pack.presets = preset_files(manifest.pack.presets);
            if (!presets.empty()) manifest.exported_kind_names.emplace_back("presets");
            if (!themes.empty()) manifest.exported_kind_names.emplace_back("themes");
            if (!samples.empty()) manifest.exported_kind_names.emplace_back("samples");
            if (!wavetables.empty()) manifest.exported_kind_names.emplace_back("wavetables");
        } else if (exports.isObject()) {
            const auto presets = string_array_member(exports, "presets");
            const auto themes = string_array_member(exports, "themes");
            const auto samples = string_array_member(exports, "samples");
            const auto wavetables = string_array_member(exports, "wavetables");
            const auto licenses = string_array_member(exports, "licenses");
            manifest.exported_paths.insert(manifest.exported_paths.end(), presets.begin(), presets.end());
            manifest.exported_paths.insert(manifest.exported_paths.end(), themes.begin(), themes.end());
            manifest.exported_paths.insert(manifest.exported_paths.end(), samples.begin(), samples.end());
            manifest.exported_paths.insert(manifest.exported_paths.end(), wavetables.begin(), wavetables.end());
            manifest.exported_paths.insert(manifest.exported_paths.end(), licenses.begin(), licenses.end());
            if (!presets.empty()) manifest.exported_kind_names.emplace_back("presets");
            if (!themes.empty()) manifest.exported_kind_names.emplace_back("themes");
            if (!samples.empty()) manifest.exported_kind_names.emplace_back("samples");
            if (!wavetables.empty()) manifest.exported_kind_names.emplace_back("wavetables");
            validate_export_paths(presets, issues);
            validate_export_paths(themes, issues);
            validate_export_paths(samples, issues);
            validate_export_paths(wavetables, issues);
            validate_export_paths(licenses, issues);
        }

        if (manifest.archive)
            validate_archive_entries(input, issues, &manifest.exported_paths);
    } catch (const std::exception& e) {
        issues.push_back(std::string("invalid-json: ") + e.what());
        return std::nullopt;
    } catch (...) {
        issues.push_back("invalid-json");
        return std::nullopt;
    }

    if (!issues.empty()) return std::nullopt;
    return manifest;
}

std::vector<ContentPackInfo> sorted_packs(std::vector<ContentPackInfo> packs) {
    std::sort(packs.begin(), packs.end(), [](const auto& a, const auto& b) {
        if (a.id != b.id) return a.id < b.id;
        return a.version < b.version;
    });
    return packs;
}

bool matches_manifest(const ContentPackInfo& pack, const ContentCapabilityManifest& manifest) {
    if (!manifest.content_kinds.empty()) {
        if (!intersects(exported_kinds(pack), manifest.content_kinds)) return false;
    }

    if (manifest.capabilities.empty()) return true;
    return intersects(pack.capabilities, manifest.capabilities);
}

bool is_content_update_backup_dir(const fs::path& path) {
    return path.filename().string().starts_with(".pulp-content-update-backup-");
}

ContentReloadPolicy reload_policy_for_kind(const ContentCapabilityManifest& manifest,
                                           const std::string& kind) {
    if (contains(manifest.hot_reload_kinds, kind)) return ContentReloadPolicy::hot_reload;
    if (contains(manifest.manual_rescan_kinds, kind)) return ContentReloadPolicy::manual_rescan;
    return ContentReloadPolicy::restart_required;
}

std::vector<LocalContentManifest> installed_content(const fs::path& data_root) {
    std::vector<LocalContentManifest> entries;
    std::error_code ec;
    const auto root = ContentRegistry::content_root_for_data_root(data_root);
    if (!fs::exists(root, ec)) return entries;

    for (fs::directory_iterator plugins(root, ec), plugins_end; !ec && plugins != plugins_end; plugins.increment(ec)) {
        if (!plugins->is_directory(ec)) continue;
        for (fs::directory_iterator ids(plugins->path(), ec), ids_end; !ec && ids != ids_end; ids.increment(ec)) {
            if (!ids->is_directory(ec)) continue;
            for (fs::directory_iterator versions(ids->path(), ec), versions_end; !ec && versions != versions_end; versions.increment(ec)) {
                if (!versions->is_directory(ec)) continue;
                if (is_content_update_backup_dir(versions->path())) continue;
                std::vector<std::string> issues;
                if (auto manifest = load_local_content_manifest(versions->path(), issues)) {
                    manifest->root = versions->path();
                    manifest->pack.root = versions->path();
                    entries.push_back(std::move(*manifest));
                }
            }
        }
    }

    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        if (a.pack.id != b.pack.id) return a.pack.id < b.pack.id;
        return a.pack.version < b.pack.version;
    });
    return entries;
}

bool write_content_index(const fs::path& data_root) {
    const auto entries = installed_content(data_root);
    std::string out = "{\n  \"version\": 1,\n  \"updated_at\": " + json_string(now_string())
        + ",\n  \"content\": [";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const auto rel = fs::relative(entry.root, data_root).generic_string();
        std::string plugin_id;
        const auto rel_path = fs::relative(entry.root, ContentRegistry::content_root_for_data_root(data_root));
        auto it = rel_path.begin();
        if (it != rel_path.end()) plugin_id = it->string();

        out += i == 0 ? "\n" : ",\n";
        out += "    {\"id\":" + json_string(entry.pack.id)
            + ",\"version\":" + json_string(entry.pack.version)
            + ",\"plugin_id\":" + json_string(plugin_id)
            + ",\"root\":" + json_string(rel)
            + ",\"manifest_sha256\":" + json_string(entry.manifest_sha256)
            + ",\"kind\":" + strings_json(entry.pack.kinds)
            + ",\"capabilities\":" + strings_json(entry.pack.capabilities)
            + "}";
    }
    out += entries.empty() ? "\n  ]\n}\n" : "\n  ]\n}\n";
    return write_text(ContentRegistry::content_root_for_data_root(data_root) / "index.json", out);
}

} // namespace

std::optional<ContentCapabilityManifest>
parse_content_capability_manifest(std::string_view text, std::string* error) {
    auto fail = [&](std::string message) -> std::optional<ContentCapabilityManifest> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };

    try {
        auto json = choc::json::parse(std::string(text));
        if (!json.isObject()) return fail("manifest root must be a JSON object");
        const auto schema = string_member(json, "schema");
        if (schema != "pulp.plugin-runtime.v1")
            return fail("schema must be pulp.plugin-runtime.v1");

        ContentCapabilityManifest manifest;
        manifest.schema = schema;
        manifest.plugin_id = string_member(json, "pluginId");
        if (manifest.plugin_id.empty())
            return fail("pluginId is required");

        auto content = json["content"];
        if (!content.isObject())
            return fail("content object is required");

        manifest.capabilities = string_array_member(content, "capabilities");
        manifest.content_kinds = string_array_member(content, "kinds");
        if (manifest.capabilities.empty())
            return fail("content.capabilities must contain at least one capability");
        if (manifest.content_kinds.empty())
            return fail("content.kinds must contain at least one content kind");

        auto reload = content["reload"];
        if (reload.isObject()) {
            manifest.hot_reload_kinds = string_array_member(reload, "hotReloadKinds");
            manifest.manual_rescan_kinds = string_array_member(reload, "manualRescanKinds");

            std::string unsupported;
            if (!all_in(manifest.hot_reload_kinds, manifest.content_kinds, &unsupported))
                return fail("content.reload.hotReloadKinds contains unsupported kind: " + unsupported);
            if (!all_in(manifest.manual_rescan_kinds, manifest.content_kinds, &unsupported))
                return fail("content.reload.manualRescanKinds contains unsupported kind: " + unsupported);
        }

        if (error) error->clear();
        return manifest;
    } catch (const std::exception& e) {
        return fail(std::string("invalid JSON: ") + e.what());
    } catch (...) {
        return fail("invalid JSON");
    }
}

std::optional<ContentCapabilityManifest>
load_content_capability_manifest(const fs::path& path, std::string* error) {
    auto text = read_text(path);
    if (text.empty()) {
        if (error) *error = "manifest file is missing or empty";
        return std::nullopt;
    }
    return parse_content_capability_manifest(text, error);
}

std::string content_capability_manifest_to_json(const ContentCapabilityManifest& manifest) {
    auto obj = choc::value::createObject("PulpPluginRuntime");
    obj.addMember("schema", manifest.schema.empty() ? "pulp.plugin-runtime.v1" : manifest.schema);
    obj.addMember("pluginId", manifest.plugin_id);

    auto content = choc::value::createObject("ContentCapabilities");
    auto caps = choc::value::createEmptyArray();
    for (const auto& capability : manifest.capabilities)
        caps.addArrayElement(choc::value::Value(capability));
    auto kinds = choc::value::createEmptyArray();
    for (const auto& kind : manifest.content_kinds)
        kinds.addArrayElement(choc::value::Value(kind));
    content.addMember("capabilities", std::move(caps));
    content.addMember("kinds", std::move(kinds));
    if (!manifest.hot_reload_kinds.empty() || !manifest.manual_rescan_kinds.empty()) {
        auto reload = choc::value::createObject("ContentReloadPolicy");
        auto hot = choc::value::createEmptyArray();
        for (const auto& kind : manifest.hot_reload_kinds)
            hot.addArrayElement(choc::value::Value(kind));
        auto manual = choc::value::createEmptyArray();
        for (const auto& kind : manifest.manual_rescan_kinds)
            manual.addArrayElement(choc::value::Value(kind));
        reload.addMember("hotReloadKinds", std::move(hot));
        reload.addMember("manualRescanKinds", std::move(manual));
        content.addMember("reload", std::move(reload));
    }
    obj.addMember("content", std::move(content));
    return choc::json::toString(obj, true);
}

ContentRegistry::ContentRegistry(fs::path data_root)
    : data_root_(std::move(data_root)) {}

fs::path ContentRegistry::platform_data_root() {
    if (auto override = runtime::get_env("PULP_USER_DATA_DIR"); override && !override->empty())
        return fs::path(*override);
#ifdef _WIN32
    if (auto appdata = runtime::get_env("APPDATA"); appdata && !appdata->empty())
        return fs::path(*appdata) / "Pulp";
    if (auto profile = runtime::get_env("USERPROFILE"); profile && !profile->empty())
        return fs::path(*profile) / "AppData" / "Roaming" / "Pulp";
#elif defined(__APPLE__)
    if (auto home = runtime::get_env("HOME"); home && !home->empty())
        return fs::path(*home) / "Library" / "Application Support" / "Pulp";
#else
    if (auto xdg = runtime::get_env("XDG_DATA_HOME"); xdg && !xdg->empty())
        return fs::path(*xdg) / "pulp";
    if (auto home = runtime::get_env("HOME"); home && !home->empty())
        return fs::path(*home) / ".local" / "share" / "pulp";
#endif
    return fs::current_path() / ".pulp-user-data";
}

fs::path ContentRegistry::content_root_for_data_root(const fs::path& data_root) {
    return data_root / "Content";
}

fs::path ContentRegistry::content_root() const {
    return content_root_for_data_root(data_root_);
}

std::vector<ContentPackInfo> ContentRegistry::packs_for_plugin(const std::string& plugin_id) const {
    std::vector<ContentPackInfo> packs;
    const auto plugin_root = content_root() / plugin_id;
    std::error_code ec;
    if (!fs::is_directory(plugin_root, ec)) return packs;

    for (fs::directory_iterator package_it(plugin_root, ec), package_end;
         !ec && package_it != package_end;
         package_it.increment(ec)) {
        if (!package_it->is_directory(ec)) continue;
        for (fs::directory_iterator version_it(package_it->path(), ec), version_end;
             !ec && version_it != version_end;
             version_it.increment(ec)) {
            if (!version_it->is_directory(ec)) continue;
            if (is_content_update_backup_dir(version_it->path())) continue;
            if (auto pack = load_pack(version_it->path())) packs.push_back(std::move(*pack));
        }
    }

    return sorted_packs(std::move(packs));
}

std::vector<ContentPackInfo> ContentRegistry::packs_for_plugin(
    const ContentCapabilityManifest& manifest) const {
    auto packs = packs_for_plugin(manifest.plugin_id);
    packs.erase(std::remove_if(packs.begin(), packs.end(), [&](const auto& pack) {
                    return !matches_manifest(pack, manifest);
                }),
                packs.end());
    return packs;
}

std::vector<PresetInfo> ContentRegistry::presets_for_plugin(
    const ContentCapabilityManifest& manifest) const {
    std::vector<PresetInfo> presets;
    if (!contains(manifest.content_kinds, "presets")
        || !contains(manifest.capabilities, "content.presets.v1")) {
        return presets;
    }
    for (const auto& pack : packs_for_plugin(manifest)) {
        for (const auto& path : pack.presets) {
            PresetInfo info;
            info.name = path.stem().string();
            info.path = path;
            info.is_factory = true;
            info.folder = pack.name.empty() ? pack.id : pack.name;
            info.tags = pack.capabilities;
            presets.push_back(std::move(info));
        }
    }

    std::sort(presets.begin(), presets.end(), [](const auto& a, const auto& b) {
        if (a.name != b.name) return a.name < b.name;
        return a.path.string() < b.path.string();
    });
    return presets;
}

const char* to_string(ContentReloadPolicy policy) {
    switch (policy) {
        case ContentReloadPolicy::hot_reload: return "hot-reload";
        case ContentReloadPolicy::manual_rescan: return "manual-rescan";
        case ContentReloadPolicy::restart_required: return "restart-required";
    }
    return "restart-required";
}

ContentInstallPreview preview_content_pack_install(const fs::path& input,
                                                   const ContentCapabilityManifest& plugin,
                                                   const fs::path& data_root) {
    ContentInstallPreview preview;
    preview.source = input;
    preview.plugin = plugin;
    if (!safe_id_component(plugin.plugin_id)) {
        preview.issues.push_back("invalid-plugin-id: plugin id is not a safe path component");
        return preview;
    }

    std::vector<std::string> issues;
    auto local = load_local_content_manifest(input, issues);
    if (!local) {
        preview.issues = std::move(issues);
        return preview;
    }

    preview.pack = std::move(local->pack);
    preview.install_root = ContentRegistry::content_root_for_data_root(data_root)
        / plugin.plugin_id / preview.pack.id / preview.pack.version;

    auto kinds = local->exported_kind_names;
    if (kinds.empty()) kinds = exported_kinds(preview.pack);
    if (!intersects(kinds, plugin.content_kinds)) {
        issues.push_back("content-kind-mismatch: pack exports " + strings_json(kinds)
                         + " but plugin accepts " + strings_json(plugin.content_kinds));
    }
    if (!preview.pack.capabilities.empty()
        && !intersects(preview.pack.capabilities, plugin.capabilities)) {
        issues.push_back("capability-mismatch: pack capabilities do not match plugin runtime capabilities");
    }

    for (const auto& kind : kinds) {
        if (!contains(plugin.content_kinds, kind)) continue;
        preview.policies.push_back(ContentInstallPolicy{kind, reload_policy_for_kind(plugin, kind)});
    }

    preview.issues = std::move(issues);
    preview.ok = preview.issues.empty();
    return preview;
}

ContentInstallResult install_content_pack(const fs::path& input,
                                          const ContentCapabilityManifest& plugin,
                                          const fs::path& data_root,
                                          bool approved) {
    ContentInstallResult result;
    result.preview = preview_content_pack_install(input, plugin, data_root);
    result.issues = result.preview.issues;
    if (!result.preview.ok) return result;
    if (!approved) {
        result.issues.push_back("approval-required: content install requires explicit approval");
        return result;
    }

    std::vector<std::string> issues;
    auto local = load_local_content_manifest(input, issues);
    if (!local) {
        result.issues.insert(result.issues.end(), issues.begin(), issues.end());
        return result;
    }
    if (local->manifest_file) {
        result.issues.push_back(
            "input-kind: content install requires a content-pack directory or .pulpcontent archive, not a bare manifest file");
        return result;
    }

    std::error_code ec;
    if (fs::exists(result.preview.install_root, ec)) {
        result.issues.push_back(
            "already-installed: content pack already installed; use an explicit update flow to replace it");
        return result;
    }
    fs::remove_all(result.preview.install_root, ec);
    std::string error;
    const bool copied = local->archive
        ? extract_archive_content(input, result.preview.install_root, error)
        : copy_declared_content(local->root, result.preview.install_root, local->exported_paths, error);
    if (!copied) {
        fs::remove_all(result.preview.install_root, ec);
        result.issues.push_back(error);
        return result;
    }
    if (!write_content_index(data_root)) {
        fs::remove_all(result.preview.install_root, ec);
        result.issues.push_back("index-write: failed to write content index");
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace pulp::state
