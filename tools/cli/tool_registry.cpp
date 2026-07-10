// SPDX-License-Identifier: MIT
#include "tool_registry.hpp"
#include "json_parser.hpp"
#include <pulp/platform/child_process.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace pulp::cli::tools {

// ── Colors ──

static std::string green(const std::string& s) { return "\033[32m" + s + "\033[0m"; }
static std::string red(const std::string& s) { return "\033[31m" + s + "\033[0m"; }
static std::string yellow(const std::string& s) { return "\033[33m" + s + "\033[0m"; }
static std::string dim(const std::string& s) { return "\033[2m" + s + "\033[0m"; }

static void print_ok(const std::string& msg) { std::cout << green("✓") << " " << msg << "\n"; }
static void print_fail(const std::string& msg) { std::cerr << red("✗") << " " << msg << "\n"; }

// ── File Helpers ──

static std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static bool write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    if (!f) return false;
    f << content;
    return f.good();
}

// ── Pulp Home ──

fs::path pulp_home() {
    if (auto env = std::getenv("PULP_HOME")) return env;
#ifdef __APPLE__
    if (auto home = std::getenv("HOME")) return fs::path(home) / ".pulp";
#elif defined(_WIN32)
    if (auto appdata = std::getenv("LOCALAPPDATA")) return fs::path(appdata) / "Pulp";
#else
    if (auto home = std::getenv("HOME")) return fs::path(home) / ".pulp";
#endif
    return fs::temp_directory_path() / "pulp";
}

fs::path tools_dir() { return pulp_home() / "tools"; }

// ── Platform Detection ──

std::string current_platform_key() {
#if defined(__APPLE__)
#if defined(__aarch64__)
    return "macOS-arm64";
#else
    return "macOS-x64";
#endif
#elif defined(_WIN32)
#if defined(_M_ARM64)
    return "Windows-arm64";
#else
    return "Windows-x64";
#endif
#elif defined(__linux__)
#if defined(__aarch64__)
    return "Linux-arm64";
#else
    return "Linux-x64";
#endif
#else
    return "unknown";
#endif
}

// ── Registry Loading ──

using pulp::cli::pkg::JsonParser;
using pulp::cli::pkg::JsonValue;

ToolRegistryLoadResult load_tool_registry(const fs::path& path) {
    ToolRegistryLoadResult result;
    auto content = read_file(path);
    if (content.empty()) {
        result.error = "Cannot read tool registry: " + path.string();
        return result;
    }

    JsonParser parser{content};
    auto root = parser.parse();
    if (root.type != JsonValue::Object) {
        result.error = "Tool registry is not a valid JSON object";
        return result;
    }

    if (auto v = root.get("schema_version"))
        result.registry.schema_version = v->as_int();

    if (auto tools = root.get("tools"); tools && tools->type == JsonValue::Object) {
        for (auto& [id, val] : tools->obj()) {
            ToolDescriptor tool;
            tool.id = id;
            if (auto v = val.get("display_name")) tool.display_name = v->as_string();
            if (auto v = val.get("category")) tool.category = v->as_string();
            if (auto v = val.get("description")) tool.description = v->as_string();
            if (auto v = val.get("license")) tool.license = v->as_string();
            if (auto v = val.get("install_method")) tool.install_method = v->as_string();
            if (auto v = val.get("pip_package")) tool.pip_package = v->as_string();
            if (auto v = val.get("source_dir")) tool.source_dir = v->as_string();
            if (auto v = val.get("module")) tool.module = v->as_string();
            if (auto v = val.get("npm_package_root")) tool.npm_package_root = v->as_string();
            if (auto v = val.get("npm_default_script")) tool.npm_default_script = v->as_string();
            if (auto v = val.get("pinned_version")) tool.pinned_version = v->as_string();
            if (auto v = val.get("requires_tools")) tool.requires_tools = v->as_string_array();
            if (auto v = val.get("managed_by_pulp")) tool.managed_by_pulp = v->as_bool();
            if (auto v = val.get("bundleable")) tool.bundleable = v->as_bool();
            if (auto v = val.get("install_scope")) tool.install_scope = v->as_string();
            if (auto v = val.get("distribution_lane")) tool.distribution_lane = v->as_string();
            if (auto v = val.get("package_format")) tool.package_format = v->as_string();
            if (auto v = val.get("artifact_status")) tool.artifact_status = v->as_string();
            if (auto v = val.get("artifact_policy")) tool.artifact_policy = v->as_string();
            if (auto v = val.get("artifact_pack_command")) tool.artifact_pack_command = v->as_string();
            if (auto v = val.get("artifact_pack_npm_script")) tool.artifact_pack_npm_script = v->as_string();
            if (auto v = val.get("artifact_verify_command")) tool.artifact_verify_command = v->as_string();
            if (auto v = val.get("artifact_manifest_schema")) tool.artifact_manifest_schema = v->as_string();

            // Optional project-importer fields (DATA-only; present on
            // framework-importer add-on tools).
            if (auto v = val.get("frameworks")) tool.frameworks = v->as_string_array();
            if (auto v = val.get("spi_min")) tool.spi_min = v->as_int();
            if (auto v = val.get("spi_max")) tool.spi_max = v->as_int();
            if (auto v = val.get("sdk_min")) tool.sdk_min = v->as_string();
            if (auto v = val.get("sdk_max")) tool.sdk_max = v->as_string();
            if (auto v = val.get("capabilities")) tool.capabilities = v->as_string_array();
            if (auto v = val.get("health_check")) tool.health_check = v->as_string();
            if (auto v = val.get("skill_source")) tool.skill_source = v->as_string();
            if (auto v = val.get("skill_name")) tool.skill_name = v->as_string();

            // Checksummed per-platform importer artifacts (#19).
            if (auto ia = val.get("importer_artifacts");
                ia && ia->type == JsonValue::Object) {
                for (auto& [platform, src] : ia->obj()) {
                    ImporterArtifact a;
                    if (auto v = src.get("url_template")) a.url_template = v->as_string();
                    if (auto v = src.get("archive_format")) a.archive_format = v->as_string();
                    if (auto v = src.get("sha256")) a.sha256 = v->as_string();
                    tool.importer_artifacts[platform] = a;
                }
            }

            // IMPORTER_TERMS DATA (vendor-supplied; surfaced by the accept gate).
            if (auto v = val.get("terms_text")) tool.terms_text = v->as_string();
            if (auto v = val.get("terms_version")) tool.terms_version = v->as_string();
            if (auto v = val.get("vendor_id")) tool.vendor_id = v->as_string();

            if (auto bs = val.get("binary_sources"); bs && bs->type == JsonValue::Object) {
                for (auto& [platform, src] : bs->obj()) {
                    BinarySource s;
                    if (auto v = src.get("url_template")) s.url_template = v->as_string();
                    if (auto v = src.get("archive_format")) s.archive_format = v->as_string();
                    if (auto v = src.get("binary_name")) s.binary_name = v->as_string();
                    tool.binary_sources[platform] = s;
                }
            }

            result.registry.tools[id] = tool;
        }
    }

    return result;
}

// ── URL Template Expansion ──

static std::string expand_url(const std::string& tmpl, const std::string& version) {
    std::string url = tmpl;
    auto pos = url.find("${version}");
    if (pos != std::string::npos)
        url.replace(pos, 10, version);
    return url;
}

static bool tool_available_on_platform(const ToolDescriptor& tool, const std::string& platform) {
    return tool.binary_sources.count(platform) > 0
        || tool.install_method == "python_pip"
        || tool.install_method == "npm_package";
}

static std::string json_escape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::ostringstream hex;
                    hex << "\\u" << std::hex << std::uppercase;
                    hex.width(4);
                    hex.fill('0');
                    hex << static_cast<int>(static_cast<unsigned char>(c));
                    out += hex.str();
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

static void print_tool_info_json(const ToolDescriptor& tool,
                                 const ToolLocateResult& loc,
                                 const std::string& platform) {
    std::cout << "{"
              << "\"id\":\"" << json_escape(tool.id) << "\","
              << "\"display_name\":\"" << json_escape(tool.display_name) << "\","
              << "\"category\":\"" << json_escape(tool.category) << "\","
              << "\"description\":\"" << json_escape(tool.description) << "\","
              << "\"install_method\":\"" << json_escape(tool.install_method) << "\","
              << "\"install_scope\":\"" << json_escape(tool.install_scope) << "\","
              << "\"distribution_lane\":\"" << json_escape(tool.distribution_lane) << "\","
              << "\"package_format\":\"" << json_escape(tool.package_format) << "\","
              << "\"artifact_status\":\"" << json_escape(tool.artifact_status) << "\","
              << "\"artifact_policy\":\"" << json_escape(tool.artifact_policy) << "\","
              << "\"artifact_pack_command\":\"" << json_escape(tool.artifact_pack_command) << "\","
              << "\"artifact_pack_npm_script\":\"" << json_escape(tool.artifact_pack_npm_script) << "\","
              << "\"artifact_verify_command\":\"" << json_escape(tool.artifact_verify_command) << "\","
              << "\"artifact_manifest_schema\":\"" << json_escape(tool.artifact_manifest_schema) << "\","
              << "\"pinned_version\":\"" << json_escape(tool.pinned_version) << "\","
              << "\"bundleable\":" << (tool.bundleable ? "true" : "false") << ","
              << "\"managed_by_pulp\":" << (tool.managed_by_pulp ? "true" : "false") << ","
              << "\"platform\":\"" << json_escape(platform) << "\","
              << "\"available_on_platform\":"
              << (tool_available_on_platform(tool, platform) ? "true" : "false") << ","
              << "\"installed\":" << (loc.found ? "true" : "false") << ","
              << "\"location_source\":\"" << json_escape(loc.source) << "\","
              << "\"path\":\"" << json_escape(loc.path.string()) << "\""
              << "}\n";
}

static void print_tool_info_text(const ToolDescriptor& tool,
                                 const ToolLocateResult& loc,
                                 const std::string& platform) {
    std::cout << tool.display_name << " " << dim("(" + tool.id + ")") << "\n\n";
    if (!tool.description.empty())
        std::cout << tool.description << "\n\n";
    std::cout << "Install method: " << tool.install_method << "\n";
    if (!tool.install_scope.empty())
        std::cout << "Install scope: " << tool.install_scope << "\n";
    if (!tool.distribution_lane.empty())
        std::cout << "Distribution lane: " << tool.distribution_lane << "\n";
    if (!tool.package_format.empty())
        std::cout << "Package format: " << tool.package_format << "\n";
    if (!tool.artifact_status.empty())
        std::cout << "Artifact status: " << tool.artifact_status << "\n";
    if (!tool.artifact_policy.empty())
        std::cout << "Artifact policy: " << tool.artifact_policy << "\n";
    if (!tool.artifact_pack_command.empty())
        std::cout << "Artifact pack command: " << tool.artifact_pack_command << "\n";
    if (!tool.artifact_pack_npm_script.empty())
        std::cout << "Artifact pack npm script: " << tool.artifact_pack_npm_script << "\n";
    if (!tool.artifact_verify_command.empty())
        std::cout << "Artifact verify command: " << tool.artifact_verify_command << "\n";
    if (!tool.artifact_manifest_schema.empty())
        std::cout << "Artifact manifest schema: " << tool.artifact_manifest_schema << "\n";
    if (!tool.pinned_version.empty())
        std::cout << "Pinned version: " << tool.pinned_version << "\n";
    std::cout << "Platform: " << platform << "\n";
    std::cout << "Available: " << (tool_available_on_platform(tool, platform) ? "yes" : "no") << "\n";
    if (loc.found)
        std::cout << "Installed: yes (" << loc.source << ", " << loc.path.string() << ")\n";
    else
        std::cout << "Installed: no\n";
}

static fs::path repo_root_from_registry(const fs::path& registry_path) {
    auto packages = registry_path.parent_path();
    auto tools = packages.parent_path();
    return tools.parent_path();
}

static fs::path unique_temp_dir(const std::string& prefix) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / (prefix + "-" + std::to_string(stamp));
}

static fs::path absolute_from(const fs::path& base, const std::string& path) {
    fs::path p(path);
    if (p.is_absolute()) return p;
    return base / p;
}

static const JsonValue* object_field(const JsonValue& value, const std::string& key) {
    auto* field = value.get(key);
    return field && field->type == JsonValue::Object ? field : nullptr;
}

static std::string string_field(const JsonValue& value, const std::string& key) {
    auto* field = value.get(key);
    return field && field->type == JsonValue::String ? field->str_val : std::string{};
}

static fs::path npm_tool_dir(const ToolDescriptor& tool) {
    return tools_dir() / "npm-packages" / tool.id;
}

static bool copy_directory_tree(const fs::path& source, const fs::path& dest, std::string& error) {
    std::error_code ec;
    fs::remove_all(dest, ec);
    if (ec) {
        error = "failed to remove existing package root " + dest.string() + ": " + ec.message();
        return false;
    }
    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
        error = "failed to create package parent " + dest.parent_path().string() + ": " + ec.message();
        return false;
    }
    fs::copy(source,
             dest,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             ec);
    if (ec) {
        error = "failed to copy artifact package into managed install root: " + ec.message();
        return false;
    }
    return true;
}

static fs::path npm_tool_wrapper_path(const ToolDescriptor& tool) {
#ifdef _WIN32
    return npm_tool_dir(tool) / "run.bat";
#else
    return npm_tool_dir(tool) / "run.sh";
#endif
}

// ── Archive Extraction ──

bool extract_archive(const fs::path& archive, const fs::path& dest,
                     const std::string& format) {
    fs::create_directories(dest);
    std::string cmd;

#ifdef _WIN32
    // LCOV_EXCL_START
    // PR diff coverage is macOS-only; Windows archive extraction is covered by
    // the Windows x64 build/test gate and cannot be exercised in this lane.
    if (format == "zip") {
        auto r = pulp::platform::exec(
            "tar", {"xf", archive.string(), "-C", dest.string()},
            120000);
        return r.exit_code == 0;
    }
    if (format == "tar.gz" || format == "tar.xz") {
        auto r = pulp::platform::exec(
            "tar", {"xf", archive.string(), "-C", dest.string()}, 120000);
        return r.exit_code == 0;
    }
    return false;
    // LCOV_EXCL_STOP
#else
    if (format == "zip") {
        cmd = "unzip -o -q '" + archive.string() + "' -d '" + dest.string() + "'";
    } else if (format == "tar.gz") {
        cmd = "tar xzf '" + archive.string() + "' -C '" + dest.string() + "'";
    } else if (format == "tar.xz") {
        cmd = "tar xJf '" + archive.string() + "' -C '" + dest.string() + "'";
    } else {
        return false;
    }
    auto r = pulp::platform::exec("/bin/sh", {"-c", cmd}, 120000);
    return r.exit_code == 0;
#endif
}

static bool verify_video_proof_artifact_manifest(const ToolDescriptor& tool,
                                                 const fs::path& registry_path,
                                                 const fs::path& manifest_path,
                                                 std::string& error) {
    if (tool.artifact_verify_command.empty()) {
        error = "tool '" + tool.id + "' does not expose an artifact verifier";
        return false;
    }
    auto script = repo_root_from_registry(registry_path) /
                  "tools" / "local-ci" / "pack_video_proof_tool.py";
    if (!fs::exists(script)) {
        error = "artifact verifier script not found at " + script.string();
        return false;
    }
    auto verify = pulp::platform::exec(
        "python3", {script.string(), "--verify", manifest_path.string(), "--json"}, 300000);
    if (verify.exit_code != 0) {
        error = "artifact verification failed: " + verify.stderr_output + verify.stdout_output;
        return false;
    }
    return true;
}

static bool resolve_verified_artifact_package_root(const ToolDescriptor& tool,
                                                   const fs::path& registry_path,
                                                   const fs::path& manifest_path,
                                                   fs::path& package_root,
                                                   fs::path& artifact_path,
                                                   std::string& error) {
    auto resolved_manifest = fs::absolute(manifest_path);
    if (!fs::exists(resolved_manifest)) {
        error = "artifact manifest not found: " + resolved_manifest.string();
        return false;
    }
    if (!verify_video_proof_artifact_manifest(tool, registry_path, resolved_manifest, error))
        return false;

    JsonParser parser{read_file(resolved_manifest)};
    auto root = parser.parse();
    if (root.type != JsonValue::Object) {
        error = "artifact manifest is not a JSON object: " + resolved_manifest.string();
        return false;
    }
    if (string_field(root, "schema") != "pulp.video-proof-tool-package.v1") {
        error = "unsupported artifact manifest schema: " + string_field(root, "schema");
        return false;
    }
    if (string_field(root, "tool_id") != tool.id) {
        error = "artifact manifest tool_id does not match " + tool.id;
        return false;
    }
    if (string_field(root, "distribution_lane") != "tool_addon" ||
        string_field(root, "package_format") != "not_pulp_add") {
        error = "artifact manifest is not a tool add-on package";
        return false;
    }
    auto* artifact = object_field(root, "artifact");
    if (!artifact) {
        error = "artifact manifest is missing artifact object";
        return false;
    }
    artifact_path = absolute_from(resolved_manifest.parent_path(), string_field(*artifact, "path"));
    if (!fs::exists(artifact_path)) {
        error = "artifact zip not found: " + artifact_path.string();
        return false;
    }

    auto staging = unique_temp_dir("pulp-video-proof-tool-artifact");
    std::error_code ec;
    fs::remove_all(staging, ec);
    if (!extract_archive(artifact_path, staging, "zip")) {
        error = "failed to extract artifact zip: " + artifact_path.string();
        return false;
    }
    package_root = staging / "tools" / "local-ci";
    if (!fs::exists(package_root / "package.json")) {
        error = "artifact zip did not contain tools/local-ci/package.json";
        return false;
    }
    return true;
}

// ── Tool Location ──

ToolLocateResult locate_tool(const ToolDescriptor& tool) {
    ToolLocateResult result;

    // Check pulp-managed location first
    auto managed_dir = tools_dir() / tool.id;
    if (fs::exists(managed_dir)) {
        // Find the binary in the managed directory tree
        auto platform = current_platform_key();
        auto it = tool.binary_sources.find(platform);
        std::string binary_name = tool.id;
        if (it != tool.binary_sources.end())
            binary_name = it->second.binary_name;

        // Search recursively for the binary
        for (auto& entry : fs::recursive_directory_iterator(managed_dir)) {
            if (entry.path().filename() == binary_name) {
                result.found = true;
                result.path = entry.path();
                result.source = "pulp-managed";
                return result;
            }
        }
    }

    // Check for python_pip tools
    if (tool.install_method == "python_pip") {
        auto venv_dir = tools_dir() / "python-envs" / tool.id;
#ifdef _WIN32
        auto wrapper = venv_dir / "run.bat";
#else
        auto wrapper = venv_dir / "run.sh";
#endif
        if (fs::exists(wrapper)) {
            result.found = true;
            result.path = wrapper;
            result.source = "pulp-managed";
            return result;
        }
    }

    // Check for repo-local npm package tools
    if (tool.install_method == "npm_package") {
        auto wrapper = npm_tool_wrapper_path(tool);
        if (fs::exists(wrapper)) {
            result.found = true;
            result.path = wrapper;
            result.source = "pulp-managed";
            return result;
        }
    }

    // Fall back to system PATH
    auto sys_path = pulp::platform::find_on_path(tool.id);
    if (sys_path) {
        result.found = true;
        result.path = *sys_path;
        result.source = "system-path";
        return result;
    }

    result.source = "not-found";
    return result;
}

// ── Binary Tool Install ──

ToolInstallResult install_binary_tool(const ToolDescriptor& tool, bool force) {
    ToolInstallResult result;

    if (!force) {
        auto existing = locate_tool(tool);
        if (existing.found && existing.source == "pulp-managed") {
            // Check if installed version matches pinned version
            auto manifest = existing.path.parent_path() / "manifest.json";
            bool version_ok = true;
            if (fs::exists(manifest)) {
                auto content = read_file(manifest);
                // Simple check: does manifest contain the pinned version?
                version_ok = content.find(tool.pinned_version) != std::string::npos;
            }
            if (version_ok) {
                result.ok = true;
                result.binary_path = existing.path;
                result.installed_version = tool.pinned_version;
                return result;
            }
            // Version mismatch — reinstall
        }
    }

    auto platform = current_platform_key();
    auto it = tool.binary_sources.find(platform);
    if (it == tool.binary_sources.end()) {
        result.error = tool.display_name + " is not available for " + platform;
        return result;
    }

    auto& source = it->second;

    // Bare, un-archived binaries (a source with no `archive_format`, e.g.
    // Perfetto's `trace_processor_shell`) are installed by the SHA-256-verified
    // fetcher behind `pulp tool install <id>` / `pulp trace fetch`, which the
    // Rust front-end routes to directly. This generic path neither verifies the
    // download nor matches that fetcher's platform-nested cache layout, and
    // `extract_archive` below would fail trying to untar a raw binary. Defer
    // rather than install a divergent, unverified copy — so an `--all` sweep
    // that reaches here (e.g. via `pulp-cpp` directly) skips it cleanly instead
    // of erroring.
    if (source.archive_format.empty()) {
        std::cout << "  " << tool.display_name
                  << " is installed by the verified fetcher; run `pulp tool install "
                  << tool.id << "`. Skipping generic download.\n";
        result.ok = true;
        result.installed_version = tool.pinned_version;
        return result;
    }

    auto url = expand_url(source.url_template, tool.pinned_version);
    auto download_dir = tools_dir() / ".downloads";
    fs::create_directories(download_dir);

    // Determine download filename
    std::string ext = source.archive_format == "zip" ? ".zip" :
                      source.archive_format == "tar.gz" ? ".tar.gz" : ".tar.xz";
    auto archive_path = download_dir / (tool.id + "-" + tool.pinned_version + ext);

    // Download
    std::cout << "  Downloading " << tool.display_name << " " << tool.pinned_version << "...\n";
#ifdef _WIN32
    auto dl = pulp::platform::exec("powershell", {"-Command",
        "Invoke-WebRequest -Uri '" + url + "' -OutFile '" + archive_path.string() + "'"}, 300000);
#else
    auto dl = pulp::platform::exec("curl", {"-sSfL", "-o", archive_path.string(), url}, 300000);
#endif

    if (dl.exit_code != 0) {
        result.error = "Download failed: " + url;
        return result;
    }

    // Extract
    auto extract_dir = download_dir / (tool.id + "-extract");
    fs::remove_all(extract_dir);
    if (!extract_archive(archive_path, extract_dir, source.archive_format)) {
        result.error = "Failed to extract " + archive_path.string();
        return result;
    }

    // Find the binary in extracted contents
    fs::path binary_path;
    for (auto& entry : fs::recursive_directory_iterator(extract_dir)) {
        if (entry.path().filename() == source.binary_name) {
            binary_path = entry.path();
            break;
        }
    }

    if (binary_path.empty()) {
        result.error = "Binary '" + source.binary_name + "' not found in archive";
        return result;
    }

    // Install to managed directory
    auto install_dir = tools_dir() / tool.id / tool.pinned_version;
    fs::create_directories(install_dir);
    auto dest_path = install_dir / source.binary_name;
    fs::copy_file(binary_path, dest_path, fs::copy_options::overwrite_existing);

#ifndef _WIN32
    // Make executable
    fs::permissions(dest_path, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add);
    // Remove quarantine on macOS
    pulp::platform::exec("xattr", {"-d", "com.apple.quarantine", dest_path.string()}, 5000);
#endif

    // Write manifest
    std::ostringstream manifest;
    manifest << "{\n"
             << "  \"tool_id\": \"" << tool.id << "\",\n"
             << "  \"version\": \"" << tool.pinned_version << "\",\n"
             << "  \"platform\": \"" << platform << "\",\n"
             << "  \"binary_path\": \"" << dest_path.string() << "\"\n"
             << "}\n";
    write_file(install_dir / "manifest.json", manifest.str());

    // Cleanup
    fs::remove_all(extract_dir);
    fs::remove(archive_path);

    result.ok = true;
    result.binary_path = dest_path;
    result.installed_version = tool.pinned_version;
    return result;
}

// ── Python Tool Install ──

std::string python_pip_install_spec(const ToolDescriptor& tool,
                                    const fs::path& repo_root) {
    // An IN-TREE tool sets `source_dir` (a repo-relative package directory with
    // a pyproject.toml); we pip-install that directory so the tool's own code +
    // its declared deps land in the venv. A classic PyPI tool installs
    // `<pip_package>==<version>` instead.
    if (!tool.source_dir.empty()) {
        return (repo_root / tool.source_dir).string();
    }
    return tool.pip_package + "==" + tool.pinned_version;
}

std::string python_run_module(const ToolDescriptor& tool) {
    // The run wrapper invokes `python -m <module>`. `module` lets an in-tree
    // package expose a CLI submodule (e.g. quality_lab.cli) that differs from
    // its distribution name; it defaults to pip_package for classic PyPI tools.
    return tool.module.empty() ? tool.pip_package : tool.module;
}

ToolInstallResult install_python_tool(const ToolDescriptor& tool,
                                       const ToolRegistry& registry,
                                       const fs::path& registry_path,
                                       bool force) {
    ToolInstallResult result;

    // An in-tree tool installs from `<repo_root>/<source_dir>`. Resolve the repo
    // root from the registry that defined this tool (not a fresh cwd walk), and
    // fail fast — before touching uv or creating a venv — with a clear message
    // if the source tree isn't present (e.g. a user who installed only the
    // `pulp` binary with no Pulp checkout).
    const fs::path repo_root = repo_root_from_registry(registry_path);
    if (!tool.source_dir.empty()) {
        const fs::path src = repo_root / tool.source_dir;
        if (!fs::exists(src / "pyproject.toml")) {
            result.error = tool.display_name + " installs from its in-tree source ("
                + tool.source_dir + "), which was not found at " + src.string()
                + ". This tool requires a Pulp source checkout.";
            return result;
        }
    }

    // Ensure UV is installed first
    auto uv_it = registry.tools.find("uv");
    if (uv_it == registry.tools.end()) {
        result.error = "UV not found in tool registry";
        return result;
    }

    auto uv_loc = locate_tool(uv_it->second);
    if (!uv_loc.found) {
        std::cout << "  Installing UV (required for Python tools)...\n";
        auto uv_result = install_binary_tool(uv_it->second);
        if (!uv_result.ok) {
            result.error = "Failed to install UV: " + uv_result.error;
            return result;
        }
        uv_loc.path = uv_result.binary_path;
    }

    auto venv_dir = tools_dir() / "python-envs" / tool.id;
    auto venv_path = venv_dir / ".venv";

    if (!force && fs::exists(venv_path)) {
        // Already installed
#ifdef _WIN32
        result.binary_path = venv_dir / "run.bat";
#else
        result.binary_path = venv_dir / "run.sh";
#endif
        result.ok = fs::exists(result.binary_path);
        result.installed_version = tool.pinned_version;
        return result;
    }

    // Create venv
    fs::create_directories(venv_dir);
    std::cout << "  Creating Python environment for " << tool.display_name << "...\n";
    auto venv_result = pulp::platform::exec(
        uv_loc.path.string(), {"venv", venv_path.string(), "--python", "3.12"}, 120000);

    if (venv_result.exit_code != 0) {
        result.error = "Failed to create venv: " + venv_result.stderr_output;
        return result;
    }

    const std::string pip_spec = python_pip_install_spec(tool, repo_root);
    std::cout << "  Installing " << pip_spec << "...\n";

#ifdef _WIN32
    auto python_path = (venv_path / "Scripts" / "python.exe").string();
#else
    auto python_path = (venv_path / "bin" / "python").string();
#endif

    auto pip_result = pulp::platform::exec(
        uv_loc.path.string(),
        {"pip", "install", "--python", python_path, pip_spec}, 300000);

    if (pip_result.exit_code != 0) {
        result.error = "Failed to install " + pip_spec + ": " + pip_result.stderr_output;
        return result;
    }

    const std::string run_module = python_run_module(tool);

    // Create wrapper script
#ifdef _WIN32
    auto wrapper_path = venv_dir / "run.bat";
    std::string wrapper = "@echo off\n\"" + python_path + "\" -m "
                        + run_module + " %*\n";
#else
    auto wrapper_path = venv_dir / "run.sh";
    std::string wrapper = "#!/bin/sh\nexec '" + python_path + "' -m "
                        + run_module + " \"$@\"\n";
#endif

    write_file(wrapper_path, wrapper);
#ifndef _WIN32
    fs::permissions(wrapper_path, fs::perms::owner_exec | fs::perms::group_exec,
                    fs::perm_options::add);
#endif

    // Write manifest
    std::ostringstream manifest;
    manifest << "{\n"
             << "  \"tool_id\": \"" << tool.id << "\",\n"
             << "  \"version\": \"" << tool.pinned_version << "\",\n"
             << "  \"method\": \"python_pip\",\n"
             << "  \"source\": \"" << (tool.source_dir.empty()
                                          ? json_escape(tool.pip_package)
                                          : json_escape(tool.source_dir)) << "\",\n"
             << "  \"module\": \"" << json_escape(run_module) << "\",\n"
             << "  \"venv_path\": \"" << venv_path.string() << "\"\n"
             << "}\n";
    write_file(venv_dir / "manifest.json", manifest.str());

    result.ok = true;
    result.binary_path = wrapper_path;
    result.installed_version = tool.pinned_version;
    return result;
}

// ── Repo-Local npm Tool Install ──

static ToolInstallResult install_npm_tool_from_package_root(const ToolDescriptor& tool,
                                                            fs::path package_root,
                                                            bool force,
                                                            const fs::path& artifact_manifest,
                                                            const fs::path& artifact_path) {
    ToolInstallResult result;

    auto wrapper_path = npm_tool_wrapper_path(tool);
    if (!force && fs::exists(wrapper_path)) {
        result.ok = true;
        result.binary_path = wrapper_path;
        result.installed_version = tool.pinned_version;
        return result;
    }

    auto npm = pulp::platform::find_on_path("npm");
    if (!npm) {
        result.error = "npm not found on PATH";
        return result;
    }

    auto install_dir = npm_tool_dir(tool);
    if (!artifact_manifest.empty()) {
        std::string copy_error;
        auto managed_package_root = install_dir / "package";
        if (!copy_directory_tree(package_root, managed_package_root, copy_error)) {
            result.error = copy_error;
            return result;
        }
        package_root = managed_package_root;
    }

    std::error_code ec;
    package_root = fs::weakly_canonical(package_root, ec);
    if (ec) package_root = fs::absolute(package_root);

    if (!fs::exists(package_root / "package.json")) {
        result.error = "package.json not found at " + package_root.string();
        return result;
    }

    std::cout << "  Installing npm dependencies for " << tool.display_name << "...\n";
    auto install = pulp::platform::exec(
        npm->string(), {"--prefix", package_root.string(), "install"}, 300000);
    if (install.exit_code != 0) {
        result.error = "npm install failed: " + install.stderr_output;
        return result;
    }

    fs::create_directories(install_dir);
#ifdef _WIN32
    std::string wrapper = "@echo off\r\n"
        "set PULP_VIDEO_PROOF_PACKAGE_ROOT=" + package_root.string() + "\r\n"
        "if \"%1\"==\"\" (\r\n"
        "  npm --prefix \"" + package_root.string() + "\" run "
            + (tool.npm_default_script.empty() ? "smoke-video-proof" : tool.npm_default_script)
            + "\r\n"
        ") else (\r\n"
        "  npm --prefix \"" + package_root.string() + "\" run %*\r\n"
        ")\r\n";
#else
    std::string wrapper = "#!/bin/sh\n"
        "set -e\n"
        "PACKAGE_ROOT='" + package_root.string() + "'\n"
        "export PULP_VIDEO_PROOF_PACKAGE_ROOT=\"$PACKAGE_ROOT\"\n"
        "if [ \"$#\" -eq 0 ]; then\n"
        "  exec npm --prefix \"$PACKAGE_ROOT\" run "
            + (tool.npm_default_script.empty() ? "smoke-video-proof" : tool.npm_default_script)
            + "\n"
        "fi\n"
        "exec npm --prefix \"$PACKAGE_ROOT\" run \"$@\"\n";
#endif
    if (!write_file(wrapper_path, wrapper)) {
        result.error = "failed to write wrapper: " + wrapper_path.string();
        return result;
    }
#ifndef _WIN32
    fs::permissions(wrapper_path, fs::perms::owner_exec | fs::perms::group_exec,
                    fs::perm_options::add);
#endif

    std::ostringstream manifest;
    manifest << "{\n"
             << "  \"tool_id\": \"" << tool.id << "\",\n"
             << "  \"version\": \"" << tool.pinned_version << "\",\n"
             << "  \"method\": \"" << (artifact_manifest.empty() ? "npm_package" : "npm_package_artifact") << "\",\n"
             << "  \"package_root\": \"" << package_root.string() << "\",\n"
             << "  \"wrapper_path\": \"" << wrapper_path.string() << "\"";
    if (!artifact_manifest.empty())
        manifest << ",\n  \"artifact_manifest\": \"" << json_escape(fs::absolute(artifact_manifest).string()) << "\"";
    if (!artifact_path.empty())
        manifest << ",\n  \"artifact_path\": \"" << json_escape(fs::absolute(artifact_path).string()) << "\"";
    if (!tool.artifact_pack_command.empty())
        manifest << ",\n  \"artifact_pack_command\": \"" << json_escape(tool.artifact_pack_command) << "\"";
    if (!tool.artifact_pack_npm_script.empty())
        manifest << ",\n  \"artifact_pack_npm_script\": \"" << json_escape(tool.artifact_pack_npm_script) << "\"";
    if (!tool.artifact_verify_command.empty())
        manifest << ",\n  \"artifact_verify_command\": \"" << json_escape(tool.artifact_verify_command) << "\"";
    if (!tool.artifact_manifest_schema.empty())
        manifest << ",\n  \"artifact_manifest_schema\": \"" << json_escape(tool.artifact_manifest_schema) << "\"";
    manifest << "\n"
             << "}\n";
    write_file(install_dir / "manifest.json", manifest.str());

    result.ok = true;
    result.binary_path = wrapper_path;
    result.installed_version = tool.pinned_version;
    return result;
}

ToolInstallResult install_npm_tool(const ToolDescriptor& tool,
                                   const fs::path& registry_path,
                                   bool force,
                                   const fs::path& artifact_manifest) {
    if (!artifact_manifest.empty()) {
        fs::path package_root;
        fs::path artifact_path;
        std::string error;
        if (!resolve_verified_artifact_package_root(
                tool, registry_path, artifact_manifest, package_root, artifact_path, error)) {
            ToolInstallResult result;
            result.error = error;
            return result;
        }
        return install_npm_tool_from_package_root(
            tool, package_root, force, artifact_manifest, artifact_path);
    }

    if (tool.npm_package_root.empty()) {
        ToolInstallResult result;
        result.error = "npm_package_root is required for npm_package tools";
        return result;
    }

    auto package_root = fs::path(tool.npm_package_root);
    if (package_root.is_relative())
        package_root = repo_root_from_registry(registry_path) / package_root;
    return install_npm_tool_from_package_root(tool, package_root, force, {}, {});
}

// ── Uninstall ──

bool uninstall_tool(const std::string& tool_id) {
    auto dir = tools_dir() / tool_id;
    if (fs::exists(dir)) {
        fs::remove_all(dir);
        return true;
    }
    auto venv_dir = tools_dir() / "python-envs" / tool_id;
    if (fs::exists(venv_dir)) {
        fs::remove_all(venv_dir);
        return true;
    }
    auto npm_dir = tools_dir() / "npm-packages" / tool_id;
    if (fs::exists(npm_dir)) {
        fs::remove_all(npm_dir);
        return true;
    }
    return false;
}

// ── CLI Command: pulp tool ──

fs::path find_tool_registry_path() {
    auto cwd = fs::current_path();
    while (true) {
        auto p = cwd / "tools" / "packages" / "tool-registry.json";
        if (fs::exists(p)) return p;
        if (cwd.has_parent_path() && cwd.parent_path() != cwd)
            cwd = cwd.parent_path();
        else break;
    }
    return {};
}

std::optional<int> try_add_importer_alias(const std::string& id,
                                          const std::string& from_override,
                                          bool force) {
    auto reg_path = find_tool_registry_path();
    if (reg_path.empty()) return std::nullopt;
    auto [reg, err] = load_tool_registry(reg_path);
    if (!err.empty()) return std::nullopt;
    auto it = reg.tools.find(id);
    if (it == reg.tools.end() || it->second.category != "importer")
        return std::nullopt;
    return handle_importer_install(reg, id, from_override, force);
}

int cmd_tool(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cout << "Usage: pulp tool <command> [options]\n\n"
                  << "Commands:\n"
                  << "  list                    Show available and installed tools\n"
                  << "  info <tool> [--json]    Show install/package metadata for one tool\n"
                  << "  install <tool>          Download and install a tool\n"
                  << "  install <tool> --artifact-manifest <path>\n"
                  << "                          Install an npm tool from a verified local artifact\n"
                  << "  install <importer>      Install a framework importer (checksummed,\n"
                  << "                          SDK/SPI-window-checked; --from <path> for local)\n"
                  << "  install --all           Install all tools for current platform\n"
                  << "  uninstall <tool>        Remove a pulp-managed tool or importer\n"
                  << "  path <tool>             Print path to a tool's binary\n"
                  << "  run <tool> [args]       Run a tool with arguments\n"
                  << "  doctor                  Check tool health\n";
        return 0;
    }

    auto reg_path = find_tool_registry_path();
    if (reg_path.empty()) {
        print_fail("Tool registry not found at tools/packages/tool-registry.json");
        return 1;
    }

    auto [reg, err] = load_tool_registry(reg_path);
    if (!err.empty()) {
        print_fail(err);
        return 1;
    }

    auto subcmd = args[0];

    if (subcmd == "list") {
        auto platform = current_platform_key();
        std::cout << "Available tools " << dim("(" + platform + ")") << ":\n\n";
        for (auto& [id, tool] : reg.tools) {
            auto loc = locate_tool(tool);
            std::string status;
            if (loc.found && loc.source == "pulp-managed")
                status = green("installed");
            else if (loc.found)
                status = yellow("system (" + loc.path.string() + ")");
            else if (tool_available_on_platform(tool, platform))
                status = dim("available");
            else
                status = red("not available for " + platform);

            std::cout << "  " << id;
            int pad = std::max(1, 20 - static_cast<int>(id.size()));
            std::cout << std::string(pad, ' ') << status;
            std::cout << "  " << dim(tool.description) << "\n";
        }
        return 0;
    }

    if (subcmd == "info") {
        if (args.size() < 2) {
            print_fail("Usage: pulp tool info <tool-id> [--json]");
            return 1;
        }
        std::string tool_id;
        bool json = false;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--json") {
                json = true;
            } else if (tool_id.empty()) {
                tool_id = args[i];
            } else {
                print_fail("Usage: pulp tool info <tool-id> [--json]");
                return 1;
            }
        }
        if (tool_id.empty()) {
            print_fail("Usage: pulp tool info <tool-id> [--json]");
            return 1;
        }
        auto it = reg.tools.find(tool_id);
        if (it == reg.tools.end()) {
            print_fail("Tool '" + tool_id + "' not found");
            return 1;
        }
        auto platform = current_platform_key();
        auto loc = locate_tool(it->second);
        if (json)
            print_tool_info_json(it->second, loc, platform);
        else
            print_tool_info_text(it->second, loc, platform);
        return 0;
    }

    if (subcmd == "install") {
        if (args.size() < 2) {
            print_fail("Usage: pulp tool install <tool-id> [--force] [--artifact-manifest <path>]");
            return 1;
        }

        bool force = false;
        bool all = false;
        std::string tool_id;
        std::string from_override;
        fs::path artifact_manifest;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--force") force = true;
            else if (args[i] == "--all") all = true;
            else if (args[i] == "--from") {
                if (i + 1 >= args.size()) {
                    print_fail("--from requires a path or file:// URL");
                    return 1;
                }
                from_override = args[++i];
            }
            else if (args[i] == "--artifact-manifest") {
                if (i + 1 >= args.size()) {
                    print_fail("--artifact-manifest requires a manifest path");
                    return 1;
                }
                artifact_manifest = args[++i];
            }
            else tool_id = args[i];
        }
        if (all && !artifact_manifest.empty()) {
            print_fail("--artifact-manifest is only valid with one npm_package tool");
            return 1;
        }

        // Importer add-ons (#19) install via the checksummed, version-window
        // path. handle_importer_install returns nullopt for non-importers so we
        // fall through to the generic binary/python install below.
        if (!all && !tool_id.empty()) {
            if (auto rc = handle_importer_install(reg, tool_id, from_override, force))
                return *rc;
        }
        if (!from_override.empty()) {
            print_fail("--from is only valid for importer add-ons");
            return 1;
        }

        auto install_one = [&](const std::string& id) -> int {
            auto it = reg.tools.find(id);
            if (it == reg.tools.end()) {
                print_fail("Tool '" + id + "' not found in registry");
                return 1;
            }

            auto& tool = it->second;
            if (!artifact_manifest.empty() && tool.install_method != "npm_package") {
                print_fail("--artifact-manifest is only valid with npm_package tools");
                return 1;
            }

            // Install dependencies first
            for (auto& dep : tool.requires_tools) {
                auto dep_it = reg.tools.find(dep);
                if (dep_it == reg.tools.end()) continue;
                auto dep_loc = locate_tool(dep_it->second);
                if (!dep_loc.found) {
                    std::cout << "  Installing dependency: " << dep << "\n";
                    ToolInstallResult dep_result;
                    if (dep_it->second.install_method == "binary_download")
                        dep_result = install_binary_tool(dep_it->second);
                    else if (dep_it->second.install_method == "python_pip")
                        dep_result = install_python_tool(dep_it->second, reg, reg_path);
                    if (!dep_result.ok) {
                        print_fail("Failed to install dependency " + dep + ": " + dep_result.error);
                        return 1;
                    }
                }
            }

            ToolInstallResult result;
            if (tool.install_method == "binary_download")
                result = install_binary_tool(tool, force);
            else if (tool.install_method == "python_pip")
                result = install_python_tool(tool, reg, reg_path, force);
            else if (tool.install_method == "npm_package")
                result = install_npm_tool(tool, reg_path, force, artifact_manifest);
            else {
                print_fail("Unknown install method: " + tool.install_method);
                return 1;
            }

            if (result.ok) {
                print_ok("Installed " + tool.display_name + " " + result.installed_version);
                std::cout << "  " << dim(result.binary_path.string()) << "\n";
                return 0;
            } else {
                print_fail(result.error);
                return 1;
            }
        };

        if (all) {
            int rc = 0;
            for (auto& [id, tool] : reg.tools) rc |= install_one(id);
            return rc;
        }
        return install_one(tool_id);
    }

    if (subcmd == "uninstall") {
        if (args.size() < 2) {
            print_fail("Usage: pulp tool uninstall <tool-id>");
            return 1;
        }
        // Importer add-ons remove their skill + install record + tree.
        if (auto rc = handle_importer_uninstall(reg, args[1]))
            return *rc;
        if (uninstall_tool(args[1])) {
            print_ok("Uninstalled " + args[1]);
            return 0;
        }
        print_fail(args[1] + " is not installed (pulp-managed)");
        return 1;
    }

    if (subcmd == "path") {
        if (args.size() < 2) {
            print_fail("Usage: pulp tool path <tool-id>");
            return 1;
        }
        auto it = reg.tools.find(args[1]);
        if (it == reg.tools.end()) {
            print_fail("Tool '" + args[1] + "' not found");
            return 1;
        }
        auto loc = locate_tool(it->second);
        if (loc.found) {
            std::cout << loc.path.string() << "\n";
            return 0;
        }
        print_fail(args[1] + " not installed");
        return 1;
    }

    if (subcmd == "run") {
        if (args.size() < 2) {
            print_fail("Usage: pulp tool run <tool-id> [args...]");
            return 1;
        }
        auto it = reg.tools.find(args[1]);
        if (it == reg.tools.end()) {
            print_fail("Tool '" + args[1] + "' not found");
            return 1;
        }
        auto loc = locate_tool(it->second);
        if (!loc.found) {
            print_fail(args[1] + " not installed. Run: pulp tool install " + args[1]);
            return 1;
        }

        // Build command with remaining args
        std::vector<std::string> tool_args(args.begin() + 2, args.end());
        auto result = pulp::platform::ChildProcess::run(
            loc.path.string(), tool_args, {});
        std::cout << result.stdout_output;
        if (!result.stderr_output.empty())
            std::cerr << result.stderr_output;
        return result.exit_code;
    }

    if (subcmd == "doctor") {
        auto platform = current_platform_key();
        if (args.size() > 1) {
            bool run_check = false;
            std::string tool_id;
            for (size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--run") {
                    run_check = true;
                } else if (tool_id.empty()) {
                    tool_id = args[i];
                } else {
                    print_fail("Usage: pulp tool doctor [tool-id] [--run]");
                    return 1;
                }
            }
            if (tool_id.empty()) {
                print_fail("Usage: pulp tool doctor [tool-id] [--run]");
                return 1;
            }

            auto it = reg.tools.find(tool_id);
            if (it == reg.tools.end()) {
                print_fail("Tool '" + tool_id + "' not found");
                return 1;
            }

            auto& tool = it->second;
            std::cout << "Tool Health " << dim("(" + platform + ")") << ":\n\n";
            if (!tool_available_on_platform(tool, platform)) {
                std::cout << "  " << red("✗") << " " << tool.display_name
                          << " — not available for " << platform << "\n";
                return 1;
            }

            auto loc = locate_tool(tool);
            if (!loc.found) {
                std::cout << "  " << red("✗") << " " << tool.display_name
                          << " — not installed " << dim("(pulp tool install " + tool_id + ")") << "\n";
                return 1;
            }

            print_ok(tool.display_name + " — " + loc.source + " (" + loc.path.string() + ")");
            if (!run_check) {
                if (tool.install_method == "npm_package")
                    std::cout << "  " << dim("Run `pulp tool doctor " + tool_id
                              + " --run` to execute its smoke check.") << "\n";
                return 0;
            }

            auto result = pulp::platform::ChildProcess::run(loc.path.string(), {}, {});
            std::cout << result.stdout_output;
            if (!result.stderr_output.empty())
                std::cerr << result.stderr_output;
            if (result.exit_code == 0) {
                print_ok(tool.display_name + " smoke check passed");
                return 0;
            }

            print_fail(tool.display_name + " smoke check failed with exit code "
                       + std::to_string(result.exit_code));
            return result.exit_code;
        }

        std::cout << "Tool Health " << dim("(" + platform + ")") << ":\n\n";
        int issues = 0;
        for (auto& [id, tool] : reg.tools) {
            auto loc = locate_tool(tool);
            if (loc.found) {
                print_ok(tool.display_name + " — " + loc.source + " (" + loc.path.string() + ")");
            } else {
                bool available = tool_available_on_platform(tool, platform);
                if (available) {
                    std::cout << "  " << yellow("–") << " " << tool.display_name
                              << " — not installed " << dim("(pulp tool install " + id + ")") << "\n";
                } else {
                    std::cout << "  " << red("✗") << " " << tool.display_name
                              << " — not available for " << platform << "\n";
                    ++issues;
                }
            }
        }
        return issues > 0 ? 1 : 0;
    }

    print_fail("Unknown tool subcommand: " + subcmd);
    return 1;
}

}  // namespace pulp::cli::tools
