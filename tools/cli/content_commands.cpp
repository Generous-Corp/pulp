// SPDX-License-Identifier: MIT
//
// Data-only content-pack commands. These commands validate, install, update,
// list, remove, and reveal user content without executing package code.

#include "content_commands.hpp"

#include "json_parser.hpp"
#include "json_writer.hpp"
#include "kit_commands.hpp"

#include <pulp/runtime/crypto.hpp>

#include "../../external/miniz/miniz.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace pulp::cli::content {

namespace fs = std::filesystem;
using pulp::cli::pkg::JsonParser;
using pulp::cli::pkg::JsonValue;

namespace {

struct ContentManifest {
    fs::path source;
    fs::path directory_root;
    bool archive = false;
    bool manifest_file = false;
    std::string json;
    std::string manifest_sha256;
    std::string id;
    std::string name;
    std::string version;
    std::vector<std::string> kinds;
    std::vector<std::string> capabilities;
    std::vector<std::string> exported_kinds;
    std::vector<std::string> exported_paths;
};

struct RuntimeManifest {
    fs::path source;
    std::string plugin_id;
    std::vector<std::string> capabilities;
    std::vector<std::string> content_kinds;
    std::vector<std::string> hot_reload_kinds;
    std::vector<std::string> manual_rescan_kinds;
};

std::string read_text(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool write_text(const fs::path& path, const std::string& body) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << body;
    return f.good();
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

bool vector_contains(const std::vector<std::string>& values, const std::string& needle) {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

bool vector_intersects(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    for (const auto& value : a) {
        if (vector_contains(b, value)) return true;
    }
    return false;
}

const JsonValue* object_field(const JsonValue& value, const std::string& key) {
    auto* field = value.get(key);
    return field && field->type == JsonValue::Object ? field : nullptr;
}

std::string string_field(const JsonValue& value, const std::string& key) {
    auto* field = value.get(key);
    return field && field->type == JsonValue::String ? field->str_val : std::string{};
}

std::vector<std::string> string_array_field(const JsonValue& value, const std::string& key) {
    auto* field = value.get(key);
    return field && field->type == JsonValue::Array ? field->as_string_array() : std::vector<std::string>{};
}

JsonValue parse_json(const std::string& text) {
    JsonParser parser{text};
    return parser.parse();
}

bool safe_rel_path(const fs::path& rel) {
    if (rel.empty() || rel.is_absolute()) return false;
    for (const auto& part : rel) {
        const auto s = part.string();
        if (s.empty() || s == "." || s == "..") return false;
    }
    return true;
}

bool safe_content_component(const std::string& value) {
    if (value.empty() || value == "." || value == "..") return false;
    for (const unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            continue;
        return false;
    }
    return true;
}

bool reject_unsafe_content_component(const std::string& label, const std::string& value) {
    if (safe_content_component(value)) return false;
    std::cerr << "Error: unsafe " << label << " `" << value << "`\n";
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

fs::path default_data_root() {
    if (const char* override = std::getenv("PULP_USER_DATA_DIR"); override && *override)
        return fs::path(override);
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata)
        return fs::path(appdata) / "Pulp";
    if (const char* profile = std::getenv("USERPROFILE"); profile && *profile)
        return fs::path(profile) / "AppData" / "Roaming" / "Pulp";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / "Library" / "Application Support" / "Pulp";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        return fs::path(xdg) / "pulp";
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".local" / "share" / "pulp";
#endif
    return fs::current_path() / ".pulp-user-data";
}

fs::path content_root(const fs::path& data_root) {
    return data_root / "Content";
}

fs::path install_root(const fs::path& data_root,
                      const std::string& plugin,
                      const std::string& id,
                      const std::string& version) {
    return content_root(data_root) / plugin / id / version;
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

bool zip_read_file(mz_zip_archive& zip, const char* name, std::string& out) {
    size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&zip, name, &size, 0);
    if (!data) return false;
    out.assign(static_cast<const char*>(data), size);
    mz_free(data);
    return true;
}

ContentManifest load_manifest(const fs::path& input, std::string& error) {
    ContentManifest manifest;
    manifest.source = input;
    std::error_code ec;
    if (fs::is_directory(input, ec)) {
        manifest.directory_root = input;
        manifest.json = read_text(input / "pulp.package.json");
    } else if (input.extension() == ".pulpcontent" || input.extension() == ".zip") {
        manifest.archive = true;
        mz_zip_archive zip{};
        if (!mz_zip_reader_init_file(&zip, input.string().c_str(), 0)) {
            error = "Failed to open content archive " + input.string();
            return manifest;
        }
        if (!zip_read_file(zip, "pulp.package.json", manifest.json)) {
            error = "Content archive is missing pulp.package.json";
        }
        mz_zip_reader_end(&zip);
    } else {
        manifest.manifest_file = true;
        manifest.directory_root = input.parent_path();
        manifest.json = read_text(input);
    }
    if (!error.empty()) return manifest;
    if (manifest.json.empty()) {
        error = "Missing or empty pulp.package.json";
        return manifest;
    }
    manifest.manifest_sha256 = "sha256-" + pulp::runtime::sha256_hex(manifest.json);
    auto root = parse_json(manifest.json);
    manifest.id = string_field(root, "id");
    manifest.name = string_field(root, "name");
    manifest.version = string_field(root, "version");
    manifest.kinds = string_array_field(root, "kind");
    manifest.capabilities = string_array_field(root, "capabilities");
    if (auto* exports = object_field(root, "exports")) {
        for (const auto* kind : {"presets", "themes", "samples", "wavetables"}) {
            auto paths = string_array_field(*exports, kind);
            if (!paths.empty())
                manifest.exported_kinds.emplace_back(kind);
            manifest.exported_paths.insert(manifest.exported_paths.end(), paths.begin(), paths.end());
        }
        auto licenses = string_array_field(*exports, "licenses");
        manifest.exported_paths.insert(manifest.exported_paths.end(), licenses.begin(), licenses.end());
    }
    return manifest;
}

RuntimeManifest load_runtime_manifest(const fs::path& path, std::string& error) {
    RuntimeManifest manifest;
    manifest.source = path;
    const auto text = read_text(path);
    if (text.empty()) {
        error = "Missing or empty plugin runtime manifest";
        return manifest;
    }
    auto root = parse_json(text);
    if (string_field(root, "schema") != "pulp.plugin-runtime.v1") {
        error = "plugin runtime manifest schema must be pulp.plugin-runtime.v1";
        return manifest;
    }
    manifest.plugin_id = string_field(root, "pluginId");
    auto* content = object_field(root, "content");
    if (manifest.plugin_id.empty() || content == nullptr) {
        error = "plugin runtime manifest requires pluginId and content";
        return manifest;
    }
    manifest.capabilities = string_array_field(*content, "capabilities");
    manifest.content_kinds = string_array_field(*content, "kinds");
    if (manifest.capabilities.empty() || manifest.content_kinds.empty()) {
        error = "plugin runtime manifest requires content.capabilities and content.kinds";
        return manifest;
    }
    if (auto* reload = object_field(*content, "reload")) {
        manifest.hot_reload_kinds = string_array_field(*reload, "hotReloadKinds");
        manifest.manual_rescan_kinds = string_array_field(*reload, "manualRescanKinds");
    }
    for (const auto& kind : manifest.hot_reload_kinds) {
        if (!vector_contains(manifest.content_kinds, kind)) {
            error = "content.reload.hotReloadKinds contains unsupported kind `" + kind + "`";
            return manifest;
        }
    }
    for (const auto& kind : manifest.manual_rescan_kinds) {
        if (!vector_contains(manifest.content_kinds, kind)) {
            error = "content.reload.manualRescanKinds contains unsupported kind `" + kind + "`";
            return manifest;
        }
    }
    return manifest;
}

bool validate_archive_entries(const fs::path& archive, std::vector<std::string>& issues) {
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
            ok = false;
            issues.push_back("archive-entry: failed to read entry metadata");
            continue;
        }
        const fs::path rel(stat.m_filename);
        if (!safe_rel_path(rel)) {
            ok = false;
            issues.push_back("archive-entry: unsafe path `" + rel.generic_string() + "`");
        }
        if (!stat.m_is_directory && rel.generic_string() != "files.sha256.json") {
            archived_payloads.push_back(rel.generic_string());
        }
    }
    std::string sha_json;
    if (!zip_read_file(zip, "files.sha256.json", sha_json)) {
        ok = false;
        issues.push_back("sha256: missing files.sha256.json");
    } else {
        auto sha_root = parse_json(sha_json);
        if (auto* files = object_field(sha_root, "files")) {
            std::vector<std::string> declared_files;
            for (const auto& [name, value] : files->obj()) {
                if (value.type != JsonValue::String || !safe_rel_path(fs::path(name))) {
                    ok = false;
                    issues.push_back("sha256: invalid entry `" + name + "`");
                    continue;
                }
                declared_files.push_back(name);
                size_t size = 0;
                void* data = mz_zip_reader_extract_file_to_heap(&zip, name.c_str(), &size, 0);
                if (!data) {
                    ok = false;
                    issues.push_back("sha256: missing archived file `" + name + "`");
                    continue;
                }
                const auto hash = pulp::runtime::sha256_hex(static_cast<const unsigned char*>(data), size);
                mz_free(data);
                if (value.str_val != "sha256-" + hash) {
                    ok = false;
                    issues.push_back("sha256: digest mismatch for `" + name + "`");
                }
            }
            for (const auto& name : archived_payloads) {
                if (std::find(declared_files.begin(), declared_files.end(), name) == declared_files.end()) {
                    ok = false;
                    issues.push_back("sha256: unlisted archived file `" + name + "`");
                }
            }
        } else {
            ok = false;
            issues.push_back("sha256: invalid files.sha256.json");
        }
    }
    mz_zip_reader_end(&zip);
    return ok;
}

fs::path temporary_extract_root() {
    static std::atomic<unsigned> seq{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
           ("pulp-content-validate-" + std::to_string(ticks) + "-" +
            std::to_string(seq.fetch_add(1)));
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
    const auto count = mz_zip_reader_get_num_files(&zip);
    auto root_norm = fs::absolute(dest_root, ec).lexically_normal();
    if (ec) root_norm = dest_root.lexically_normal();
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

std::vector<ContentManifest> installed_content(const fs::path& data_root,
                                               const std::string& plugin_filter = {}) {
    std::vector<ContentManifest> entries;
    std::error_code ec;
    const auto root = content_root(data_root);
    if (!fs::exists(root, ec)) return entries;
    for (fs::directory_iterator plugins(root, ec), plugins_end; !ec && plugins != plugins_end; plugins.increment(ec)) {
        if (!plugins->is_directory(ec)) continue;
        const auto plugin = plugins->path().filename().string();
        if (!plugin_filter.empty() && plugin != plugin_filter) continue;
        for (fs::directory_iterator ids(plugins->path(), ec), ids_end; !ec && ids != ids_end; ids.increment(ec)) {
            if (!ids->is_directory(ec)) continue;
            for (fs::directory_iterator versions(ids->path(), ec), versions_end; !ec && versions != versions_end; versions.increment(ec)) {
                if (!versions->is_directory(ec)) continue;
                if (versions->path().filename().string().starts_with(".pulp-content-update-backup-")) continue;
                std::string error;
                auto manifest = load_manifest(versions->path(), error);
                if (error.empty()) entries.push_back(std::move(manifest));
            }
        }
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.id < b.id || (a.id == b.id && a.version < b.version);
    });
    return entries;
}

bool write_index(const fs::path& data_root, std::size_t* entry_count = nullptr) {
    auto entries = installed_content(data_root);
    if (entry_count) *entry_count = entries.size();
    std::string out = "{\n  \"version\": 1,\n  \"updated_at\": " + json_string(now_string()) + ",\n  \"content\": [";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        auto rel = fs::relative(entry.directory_root, data_root);
        std::string plugin_id;
        const auto rel_to_content = fs::relative(entry.directory_root, content_root(data_root));
        auto rel_it = rel_to_content.begin();
        if (rel_it != rel_to_content.end()) plugin_id = rel_it->string();
        out += i == 0 ? "\n" : ",\n";
        out += "    {\"id\":" + json_string(entry.id)
            + ",\"version\":" + json_string(entry.version)
            + ",\"plugin_id\":" + json_string(plugin_id)
            + ",\"root\":" + json_string(rel.generic_string())
            + ",\"manifest_sha256\":" + json_string(entry.manifest_sha256)
            + ",\"kind\":" + strings_json(entry.kinds)
            + ",\"capabilities\":" + strings_json(entry.capabilities)
            + "}";
    }
    out += entries.empty() ? "\n  ]\n}\n" : "\n  ]\n}\n";
    return write_text(content_root(data_root) / "index.json", out);
}

bool install_manifest_content(const ContentManifest& manifest,
                              const fs::path& input,
                              const fs::path& dest,
                              std::string& error) {
    if (manifest.manifest_file) {
        error = "content install/update requires a content-pack directory or .pulpcontent archive, not a bare manifest file";
        return false;
    }
    return manifest.archive
        ? extract_archive_content(input, dest, error)
        : copy_declared_content(manifest.directory_root, dest, manifest.exported_paths, error);
}

std::string manifest_json(const ContentManifest& manifest) {
    return "{\"id\":" + json_string(manifest.id)
        + ",\"name\":" + json_string(manifest.name)
        + ",\"version\":" + json_string(manifest.version)
        + ",\"kind\":" + strings_json(manifest.kinds)
        + ",\"capabilities\":" + strings_json(manifest.capabilities)
        + ",\"exports\":" + strings_json(manifest.exported_kinds)
        + ",\"manifest_sha256\":" + json_string(manifest.manifest_sha256)
        + ",\"source\":" + json_string(manifest.source.string())
        + "}";
}

std::string runtime_manifest_json(const RuntimeManifest& manifest) {
    return "{\"plugin_id\":" + json_string(manifest.plugin_id)
        + ",\"capabilities\":" + strings_json(manifest.capabilities)
        + ",\"kinds\":" + strings_json(manifest.content_kinds)
        + ",\"hot_reload_kinds\":" + strings_json(manifest.hot_reload_kinds)
        + ",\"manual_rescan_kinds\":" + strings_json(manifest.manual_rescan_kinds)
        + ",\"source\":" + json_string(manifest.source.string())
        + "}";
}

std::string reload_policy_for_kind(const RuntimeManifest& runtime, const std::string& kind) {
    if (vector_contains(runtime.hot_reload_kinds, kind)) return "hot-reload";
    if (vector_contains(runtime.manual_rescan_kinds, kind)) return "manual-rescan";
    return "restart-required";
}

std::string install_policy_json(const ContentManifest& content,
                                const RuntimeManifest& runtime,
                                bool& requires_restart) {
    std::string out = "[";
    bool first = true;
    requires_restart = false;
    for (const auto& kind : content.exported_kinds) {
        if (!vector_contains(runtime.content_kinds, kind)) continue;
        const auto policy = reload_policy_for_kind(runtime, kind);
        if (policy == "restart-required") requires_restart = true;
        if (!first) out += ",";
        first = false;
        out += "{\"kind\":" + json_string(kind)
            + ",\"policy\":" + json_string(policy)
            + ",\"hot_reload\":" + (policy == "hot-reload" ? "true" : "false")
            + ",\"manual_rescan\":" + (policy == "manual-rescan" ? "true" : "false")
            + ",\"requires_restart\":" + (policy == "restart-required" ? "true" : "false")
            + "}";
    }
    out += "]";
    return out;
}

bool validate_content_manifest(const fs::path& input,
                               ContentManifest& manifest,
                               std::vector<std::string>& issues) {
    std::string error;
    manifest = load_manifest(input, error);
    if (!error.empty()) {
        issues.push_back(error);
        return false;
    }
    bool ok = true;
    if (manifest.id.empty()) {
        ok = false;
        issues.push_back("missing-field: id");
    }
    if (manifest.version.empty()) {
        ok = false;
        issues.push_back("missing-field: version");
    }
    if (!vector_contains(manifest.kinds, "content-pack")) {
        ok = false;
        issues.push_back("invalid-kind: content commands require kind content-pack");
    }
    if (manifest.archive) {
        ok = validate_archive_entries(input, issues) && ok;
        if (ok) {
            const auto temp_root = temporary_extract_root();
            std::string extract_error;
            if (!extract_archive_content(input, temp_root, extract_error)) {
                ok = false;
                issues.push_back("archive-validate: " + extract_error);
            } else {
                auto kit_result = pulp::cli::kit::validate_manifest_path(temp_root, true);
                if (!kit_result.ok()) {
                    ok = false;
                    for (const auto& issue : kit_result.issues)
                        issues.push_back(issue.code + ": " + issue.message);
                }
            }
            std::error_code ec;
            fs::remove_all(temp_root, ec);
        }
    } else {
        auto kit_result = pulp::cli::kit::validate_manifest_path(input, true);
        if (!kit_result.ok()) {
            ok = false;
            for (const auto& issue : kit_result.issues)
                issues.push_back(issue.code + ": " + issue.message);
        }
    }
    return ok;
}

void print_usage() {
    std::cout
        << "pulp content — validate and install data-only content packs\n\n"
        << "Usage:\n"
        << "  pulp content validate <path> [--json]\n"
        << "  pulp content preview <path> --plugin-runtime <manifest> [--plugin <plugin-id>] [--json]\n"
        << "  pulp content install <path> --plugin <plugin-id> --yes [--root <dir>]\n"
        << "  pulp content update <path> --plugin <plugin-id> --yes [--root <dir>]\n"
        << "  pulp content list [--plugin <plugin-id>] [--json] [--root <dir>]\n"
        << "  pulp content rescan [--json] [--root <dir>]\n"
        << "  pulp content remove <package-id> --plugin <plugin-id> [--version <version>] --yes [--root <dir>]\n"
        << "  pulp content reveal <package-id> --plugin <plugin-id> [--version <version>] [--root <dir>]\n\n"
        << "Use content packs for presets, themes, samples, and wavetables installed into a known plugin.\n"
        << "They are not curated dependency packages and they do not transform projects.\n"
        << "Content commands copy data only. Preview reads a trusted plugin runtime manifest to report compatibility and reload policy before install. Update uses an explicit local path and rolls back a replaced version on failure. Rescan rebuilds the metadata index only. They do not execute package code.\n";
}

int cmd_validate(const std::vector<std::string>& args) {
    bool json = false;
    std::string path;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--json") json = true;
        else if (args[i] == "--help" || args[i] == "-h") { print_usage(); return 0; }
        else if (!args[i].starts_with("-") && path.empty()) path = args[i];
        else { std::cerr << "Error: unknown content validate argument: " << args[i] << "\n"; return 2; }
    }
    if (path.empty()) {
        std::cerr << "Error: pulp content validate requires a path\n";
        return 2;
    }
    ContentManifest manifest;
    std::vector<std::string> issues;
    const bool ok = validate_content_manifest(path, manifest, issues);
    if (json) {
        std::cout << "{\"ok\":" << (ok ? "true" : "false")
                  << ",\"content\":" << manifest_json(manifest)
                  << ",\"issues\":" << strings_json(issues) << "}\n";
    } else {
        std::cout << (ok ? "OK: " : "Error: ")
                  << "Content pack " << (ok ? "valid" : "invalid") << ": "
                  << manifest.id << "\n";
        for (const auto& issue : issues) std::cout << "  - " << issue << "\n";
    }
    return ok ? 0 : 1;
}

int cmd_preview(const std::vector<std::string>& args) {
    bool json = false;
    std::string path;
    std::string plugin;
    fs::path runtime_path;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto require_value = [&](const char* flag) -> bool {
            if (i + 1 >= args.size()) {
                std::cerr << "Error: " << flag << " requires a value\n";
                return false;
            }
            return true;
        };
        if (args[i] == "--json") json = true;
        else if (args[i] == "--plugin") { if (!require_value("--plugin")) return 2; plugin = args[++i]; }
        else if (args[i] == "--plugin-runtime") { if (!require_value("--plugin-runtime")) return 2; runtime_path = args[++i]; }
        else if (args[i] == "--help" || args[i] == "-h") { print_usage(); return 0; }
        else if (!args[i].starts_with("-") && path.empty()) path = args[i];
        else { std::cerr << "Error: unknown content preview argument: " << args[i] << "\n"; return 2; }
    }
    if (path.empty() || runtime_path.empty()) {
        std::cerr << "Error: pulp content preview requires <path> and --plugin-runtime <manifest>\n";
        return 2;
    }

    ContentManifest manifest;
    std::vector<std::string> issues;
    bool ok = validate_content_manifest(path, manifest, issues);

    std::string runtime_error;
    auto runtime = load_runtime_manifest(runtime_path, runtime_error);
    if (!runtime_error.empty()) {
        ok = false;
        issues.push_back("plugin-runtime: " + runtime_error);
    }

    if (!plugin.empty() && runtime.plugin_id != plugin) {
        ok = false;
        issues.push_back("plugin-mismatch: runtime manifest pluginId is `" +
                         runtime.plugin_id + "`");
    }
    if (!manifest.exported_kinds.empty() &&
        !vector_intersects(manifest.exported_kinds, runtime.content_kinds)) {
        ok = false;
        issues.push_back("content-kind-mismatch: pack exports " +
                         strings_json(manifest.exported_kinds) +
                         " but plugin accepts " + strings_json(runtime.content_kinds));
    }
    if (!manifest.capabilities.empty() &&
        !vector_intersects(manifest.capabilities, runtime.capabilities)) {
        ok = false;
        issues.push_back("capability-mismatch: pack capabilities do not match plugin runtime capabilities");
    }

    bool requires_restart = false;
    const auto policies = install_policy_json(manifest, runtime, requires_restart);
    if (json) {
        std::cout << "{\"ok\":" << (ok ? "true" : "false")
                  << ",\"content\":" << manifest_json(manifest)
                  << ",\"plugin_runtime\":" << runtime_manifest_json(runtime)
                  << ",\"install_policy\":" << policies
                  << ",\"requires_restart\":" << (requires_restart ? "true" : "false")
                  << ",\"issues\":" << strings_json(issues) << "}\n";
    } else {
        std::cout << (ok ? "OK: " : "Error: ")
                  << "Content preview for " << manifest.id
                  << " against " << runtime.plugin_id << "\n";
        for (const auto& issue : issues) std::cout << "  - " << issue << "\n";
        std::cout << "  reload policy: " << policies << "\n";
    }
    return ok ? 0 : 1;
}

int cmd_install(const std::vector<std::string>& args) {
    bool yes = false;
    std::string path;
    std::string plugin;
    fs::path data_root = default_data_root();
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto require_value = [&](const char* flag) -> bool {
            if (i + 1 >= args.size()) {
                std::cerr << "Error: " << flag << " requires a value\n";
                return false;
            }
            return true;
        };
        if (args[i] == "--yes") yes = true;
        else if (args[i] == "--plugin") { if (!require_value("--plugin")) return 2; plugin = args[++i]; }
        else if (args[i] == "--root") { if (!require_value("--root")) return 2; data_root = args[++i]; }
        else if (args[i] == "--help" || args[i] == "-h") { print_usage(); return 0; }
        else if (!args[i].starts_with("-") && path.empty()) path = args[i];
        else { std::cerr << "Error: unknown content install argument: " << args[i] << "\n"; return 2; }
    }
    if (path.empty() || plugin.empty()) {
        std::cerr << "Error: pulp content install requires <path> and --plugin <plugin-id>\n";
        return 2;
    }
    if (reject_unsafe_content_component("plugin id", plugin)) return 1;
    if (!yes) {
        std::cerr << "Error: --yes is required after reviewing the content install target\n";
        return 2;
    }
    ContentManifest manifest;
    std::vector<std::string> issues;
    if (!validate_content_manifest(path, manifest, issues)) {
        std::cerr << "Error: content pack invalid\n";
        for (const auto& issue : issues) std::cerr << "  - " << issue << "\n";
        return 1;
    }
    if (reject_unsafe_content_component("content package id", manifest.id)
        || reject_unsafe_content_component("content package version", manifest.version)) {
        return 1;
    }
    if (manifest.manifest_file) {
        std::cerr << "Error: content install requires a content-pack directory or .pulpcontent archive, not a bare manifest file\n";
        return 1;
    }
    const auto dest = install_root(data_root, plugin, manifest.id, manifest.version);
    std::error_code ec;
    if (fs::exists(dest, ec)) {
        std::cerr << "Error: content pack already installed; use `pulp content update` to replace it\n";
        return 1;
    }
    std::string error;
    const bool copied = install_manifest_content(manifest, path, dest, error);
    if (!copied) {
        fs::remove_all(dest, ec);
        std::cerr << "Error: " << error << "\n";
        return 1;
    }
    if (!write_index(data_root)) {
        fs::remove_all(dest, ec);
        std::cerr << "Error: failed to write content index\n";
        return 1;
    }
    std::cout << "OK: Installed content pack " << manifest.id << " for " << plugin
              << "\n" << dest.string() << "\n";
    return 0;
}

int cmd_update(const std::vector<std::string>& args) {
    bool yes = false;
    std::string path;
    std::string plugin;
    fs::path data_root = default_data_root();
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto require_value = [&](const char* flag) -> bool {
            if (i + 1 >= args.size()) {
                std::cerr << "Error: " << flag << " requires a value\n";
                return false;
            }
            return true;
        };
        if (args[i] == "--yes") yes = true;
        else if (args[i] == "--plugin") { if (!require_value("--plugin")) return 2; plugin = args[++i]; }
        else if (args[i] == "--root") { if (!require_value("--root")) return 2; data_root = args[++i]; }
        else if (args[i] == "--help" || args[i] == "-h") { print_usage(); return 0; }
        else if (!args[i].starts_with("-") && path.empty()) path = args[i];
        else { std::cerr << "Error: unknown content update argument: " << args[i] << "\n"; return 2; }
    }
    if (path.empty() || plugin.empty()) {
        std::cerr << "Error: pulp content update requires <path> and --plugin <plugin-id>\n";
        return 2;
    }
    if (reject_unsafe_content_component("plugin id", plugin)) return 1;
    if (!yes) {
        std::cerr << "Error: --yes is required after reviewing the content update target\n";
        return 2;
    }
    ContentManifest manifest;
    std::vector<std::string> issues;
    if (!validate_content_manifest(path, manifest, issues)) {
        std::cerr << "Error: content pack invalid\n";
        for (const auto& issue : issues) std::cerr << "  - " << issue << "\n";
        return 1;
    }
    if (reject_unsafe_content_component("content package id", manifest.id)
        || reject_unsafe_content_component("content package version", manifest.version)) {
        return 1;
    }
    if (manifest.manifest_file) {
        std::cerr << "Error: content update requires a content-pack directory or .pulpcontent archive, not a bare manifest file\n";
        return 1;
    }

    const auto dest = install_root(data_root, plugin, manifest.id, manifest.version);
    const auto content_base = content_root(data_root);
    std::error_code ec;
    auto canonical_base = fs::absolute(content_base, ec).lexically_normal();
    if (ec) canonical_base = content_base.lexically_normal();
    auto canonical_dest = fs::absolute(dest, ec).lexically_normal();
    if (ec) canonical_dest = dest.lexically_normal();
    if (!path_within(canonical_dest, canonical_base)) {
        std::cerr << "Error: refusing unsafe content update target\n";
        return 1;
    }

    const bool had_previous = fs::exists(dest, ec);
    const auto backup = dest.parent_path() /
        (".pulp-content-update-backup-" + manifest.id + "-" + manifest.version);
    if (had_previous) {
        fs::remove_all(backup, ec);
        if (ec) {
            std::cerr << "Error: failed to clear update backup: " << ec.message() << "\n";
            return 1;
        }
        fs::create_directories(backup.parent_path(), ec);
        if (ec) {
            std::cerr << "Error: failed to prepare update backup: " << ec.message() << "\n";
            return 1;
        }
        fs::rename(dest, backup, ec);
        if (ec) {
            std::cerr << "Error: failed to back up existing content before update: " << ec.message() << "\n";
            return 1;
        }
    }

    auto rollback = [&]() {
        std::error_code rollback_ec;
        fs::remove_all(dest, rollback_ec);
        if (had_previous) {
            fs::rename(backup, dest, rollback_ec);
        }
        write_index(data_root);
    };

    std::string error;
    if (!install_manifest_content(manifest, path, dest, error)) {
        rollback();
        std::cerr << "Error: " << error;
        if (had_previous) std::cerr << " (rolled back previous content)";
        std::cerr << "\n";
        return 1;
    }
    if (!write_index(data_root)) {
        rollback();
        std::cerr << "Error: failed to write content index";
        if (had_previous) std::cerr << " (rolled back previous content)";
        std::cerr << "\n";
        return 1;
    }
    if (had_previous) {
        fs::remove_all(backup, ec);
        if (ec) {
            std::cerr << "Error: installed update but failed to remove backup: " << ec.message() << "\n";
            return 1;
        }
    }
    std::cout << "OK: Updated content pack " << manifest.id << " for " << plugin
              << "\n" << dest.string() << "\n";
    return 0;
}

int cmd_list(const std::vector<std::string>& args) {
    bool json = false;
    std::string plugin;
    fs::path data_root = default_data_root();
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--json") json = true;
        else if (args[i] == "--plugin" && i + 1 < args.size()) plugin = args[++i];
        else if (args[i] == "--root" && i + 1 < args.size()) data_root = args[++i];
        else if (args[i] == "--help" || args[i] == "-h") { print_usage(); return 0; }
        else { std::cerr << "Error: unknown content list argument: " << args[i] << "\n"; return 2; }
    }
    auto entries = installed_content(data_root, plugin);
    if (json) {
        std::cout << "{\"root\":" << json_string(data_root.string()) << ",\"content\":[";
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i) std::cout << ",";
            std::cout << manifest_json(entries[i]);
        }
        std::cout << "]}\n";
    } else {
        for (const auto& entry : entries)
            std::cout << entry.id << " " << entry.version << " " << entry.directory_root.string() << "\n";
    }
    return 0;
}

int cmd_rescan(const std::vector<std::string>& args) {
    bool json = false;
    fs::path data_root = default_data_root();
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto require_value = [&](const char* flag) -> bool {
            if (i + 1 >= args.size()) {
                std::cerr << "Error: " << flag << " requires a value\n";
                return false;
            }
            return true;
        };
        if (args[i] == "--json") json = true;
        else if (args[i] == "--root") { if (!require_value("--root")) return 2; data_root = args[++i]; }
        else if (args[i] == "--help" || args[i] == "-h") { print_usage(); return 0; }
        else { std::cerr << "Error: unknown content rescan argument: " << args[i] << "\n"; return 2; }
    }

    std::size_t count = 0;
    if (!write_index(data_root, &count)) {
        std::cerr << "Error: failed to write content index\n";
        return 1;
    }
    const auto index = content_root(data_root) / "index.json";
    if (json) {
        std::cout << "{\"ok\":true,\"root\":" << json_string(data_root.string())
                  << ",\"index\":" << json_string(index.string())
                  << ",\"content_count\":" << count << "}\n";
    } else {
        std::cout << "OK: Rescanned " << count << " content pack"
                  << (count == 1 ? "" : "s") << "\n"
                  << index.string() << "\n";
    }
    return 0;
}

int cmd_remove(const std::vector<std::string>& args) {
    bool yes = false;
    std::string id;
    std::string plugin;
    std::string version;
    fs::path data_root = default_data_root();
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto require_value = [&](const char* flag) -> bool {
            if (i + 1 >= args.size()) {
                std::cerr << "Error: " << flag << " requires a value\n";
                return false;
            }
            return true;
        };
        if (args[i] == "--yes") yes = true;
        else if (args[i] == "--plugin") { if (!require_value("--plugin")) return 2; plugin = args[++i]; }
        else if (args[i] == "--version") { if (!require_value("--version")) return 2; version = args[++i]; }
        else if (args[i] == "--root") { if (!require_value("--root")) return 2; data_root = args[++i]; }
        else if (args[i] == "--help" || args[i] == "-h") { print_usage(); return 0; }
        else if (!args[i].starts_with("-") && id.empty()) id = args[i];
        else { std::cerr << "Error: unknown content remove argument: " << args[i] << "\n"; return 2; }
    }
    if (id.empty() || plugin.empty()) {
        std::cerr << "Error: pulp content remove requires <package-id> and --plugin <plugin-id>\n";
        return 2;
    }
    if (reject_unsafe_content_component("content package id", id)
        || reject_unsafe_content_component("plugin id", plugin)
        || (!version.empty() && reject_unsafe_content_component("content package version", version))) {
        return 1;
    }
    if (!yes) {
        std::cerr << "Error: --yes is required after reviewing the installed content target\n";
        return 2;
    }
    auto root = content_root(data_root) / plugin / id;
    if (!version.empty()) root /= version;
    std::error_code ec;
    auto canonical_root = fs::absolute(content_root(data_root), ec).lexically_normal();
    if (ec) canonical_root = content_root(data_root).lexically_normal();
    auto target = fs::absolute(root, ec).lexically_normal();
    if (ec) target = root.lexically_normal();
    if (!path_within(target, canonical_root)) {
        std::cerr << "Error: refusing unsafe content remove target\n";
        return 1;
    }
    fs::remove_all(target, ec);
    if (ec) {
        std::cerr << "Error: failed to remove content: " << ec.message() << "\n";
        return 1;
    }
    write_index(data_root);
    std::cout << "OK: Removed content pack " << id << " for " << plugin
              << " (removed " << target.string() << ")\n";
    return 0;
}

int cmd_reveal(const std::vector<std::string>& args) {
    std::string id;
    std::string plugin;
    std::string version;
    fs::path data_root = default_data_root();
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--plugin" && i + 1 < args.size()) plugin = args[++i];
        else if (args[i] == "--version" && i + 1 < args.size()) version = args[++i];
        else if (args[i] == "--root" && i + 1 < args.size()) data_root = args[++i];
        else if (args[i] == "--help" || args[i] == "-h") { print_usage(); return 0; }
        else if (!args[i].starts_with("-") && id.empty()) id = args[i];
        else { std::cerr << "Error: unknown content reveal argument: " << args[i] << "\n"; return 2; }
    }
    if (id.empty() || plugin.empty()) {
        std::cerr << "Error: pulp content reveal requires <package-id> and --plugin <plugin-id>\n";
        return 2;
    }
    if (reject_unsafe_content_component("content package id", id)
        || reject_unsafe_content_component("plugin id", plugin)
        || (!version.empty() && reject_unsafe_content_component("content package version", version))) {
        return 1;
    }
    fs::path root = content_root(data_root) / plugin / id;
    if (!version.empty()) root /= version;
    std::cout << root.string() << "\n";
    return fs::exists(root) ? 0 : 1;
}

}  // namespace

int cmd_content(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
        print_usage();
        return 0;
    }
    const std::string sub = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());
    if (sub == "validate") return cmd_validate(rest);
    if (sub == "preview") return cmd_preview(rest);
    if (sub == "install") return cmd_install(rest);
    if (sub == "update") return cmd_update(rest);
    if (sub == "list") return cmd_list(rest);
    if (sub == "rescan") return cmd_rescan(rest);
    if (sub == "remove" || sub == "uninstall") return cmd_remove(rest);
    if (sub == "reveal") return cmd_reveal(rest);
    std::cerr << "Error: unknown content subcommand `" << sub << "`\n";
    print_usage();
    return 2;
}

}  // namespace pulp::cli::content
