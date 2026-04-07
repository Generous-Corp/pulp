// SPDX-License-Identifier: MIT
#include "tool_registry.hpp"
#include "json_parser.hpp"
#include <pulp/platform/child_process.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
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
static void print_warn(const std::string& msg) { std::cout << yellow("⚠") << " " << msg << "\n"; }

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
        for (auto& [id, val] : tools->obj) {
            ToolDescriptor tool;
            tool.id = id;
            if (auto v = val.get("display_name")) tool.display_name = v->as_string();
            if (auto v = val.get("category")) tool.category = v->as_string();
            if (auto v = val.get("description")) tool.description = v->as_string();
            if (auto v = val.get("license")) tool.license = v->as_string();
            if (auto v = val.get("install_method")) tool.install_method = v->as_string();
            if (auto v = val.get("pip_package")) tool.pip_package = v->as_string();
            if (auto v = val.get("pinned_version")) tool.pinned_version = v->as_string();
            if (auto v = val.get("requires_tools")) tool.requires_tools = v->as_string_array();
            if (auto v = val.get("managed_by_pulp")) tool.managed_by_pulp = v->as_bool();
            if (auto v = val.get("bundleable")) tool.bundleable = v->as_bool();

            if (auto bs = val.get("binary_sources"); bs && bs->type == JsonValue::Object) {
                for (auto& [platform, src] : bs->obj) {
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

// ── Archive Extraction ──

bool extract_archive(const fs::path& archive, const fs::path& dest,
                     const std::string& format) {
    fs::create_directories(dest);
    std::string cmd;

#ifdef _WIN32
    if (format == "zip") {
        cmd = "powershell -Command \"Expand-Archive -Force -Path '"
              + archive.string() + "' -DestinationPath '" + dest.string() + "'\"";
    } else if (format == "tar.gz" || format == "tar.xz") {
        cmd = "tar xf \"" + archive.string() + "\" -C \"" + dest.string() + "\"";
    }
    auto r = pulp::platform::exec("cmd", {"/c", cmd}, 120000);
#else
    if (format == "zip") {
        cmd = "unzip -o -q '" + archive.string() + "' -d '" + dest.string() + "'";
    } else if (format == "tar.gz") {
        cmd = "tar xzf '" + archive.string() + "' -C '" + dest.string() + "'";
    } else if (format == "tar.xz") {
        cmd = "tar xJf '" + archive.string() + "' -C '" + dest.string() + "'";
    }
    auto r = pulp::platform::exec("/bin/sh", {"-c", cmd}, 120000);
#endif

    return r.exit_code == 0;
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
            result.ok = true;
            result.binary_path = existing.path;
            result.installed_version = tool.pinned_version;
            return result;
        }
    }

    auto platform = current_platform_key();
    auto it = tool.binary_sources.find(platform);
    if (it == tool.binary_sources.end()) {
        result.error = tool.display_name + " is not available for " + platform;
        return result;
    }

    auto& source = it->second;
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

ToolInstallResult install_python_tool(const ToolDescriptor& tool,
                                       const ToolRegistry& registry,
                                       bool force) {
    ToolInstallResult result;

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

    // Install pip package
    std::string pip_spec = tool.pip_package + "==" + tool.pinned_version;
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
        result.error = "Failed to install " + tool.pip_package + ": " + pip_result.stderr_output;
        return result;
    }

    // Create wrapper script
#ifdef _WIN32
    auto wrapper_path = venv_dir / "run.bat";
    std::string wrapper = "@echo off\n\"" + python_path + "\" -m "
                        + tool.pip_package + " %*\n";
#else
    auto wrapper_path = venv_dir / "run.sh";
    std::string wrapper = "#!/bin/sh\nexec '" + python_path + "' -m "
                        + tool.pip_package + " \"$@\"\n";
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
             << "  \"pip_package\": \"" << tool.pip_package << "\",\n"
             << "  \"venv_path\": \"" << venv_path.string() << "\"\n"
             << "}\n";
    write_file(venv_dir / "manifest.json", manifest.str());

    result.ok = true;
    result.binary_path = wrapper_path;
    result.installed_version = tool.pinned_version;
    return result;
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
    return false;
}

// ── CLI Command: pulp tool ──

static fs::path find_tool_registry() {
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

int cmd_tool(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cout << "Usage: pulp tool <command> [options]\n\n"
                  << "Commands:\n"
                  << "  list                    Show available and installed tools\n"
                  << "  install <tool>          Download and install a tool\n"
                  << "  install --all           Install all tools for current platform\n"
                  << "  uninstall <tool>        Remove a pulp-managed tool\n"
                  << "  path <tool>             Print path to a tool's binary\n"
                  << "  run <tool> [args]       Run a tool with arguments\n"
                  << "  doctor                  Check tool health\n";
        return 0;
    }

    auto reg_path = find_tool_registry();
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
            else if (tool.binary_sources.count(platform) || tool.install_method == "python_pip")
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

    if (subcmd == "install") {
        if (args.size() < 2) {
            print_fail("Usage: pulp tool install <tool-id> [--force]");
            return 1;
        }

        bool force = false;
        bool all = false;
        std::string tool_id;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--force") force = true;
            else if (args[i] == "--all") all = true;
            else tool_id = args[i];
        }

        auto install_one = [&](const std::string& id) -> int {
            auto it = reg.tools.find(id);
            if (it == reg.tools.end()) {
                print_fail("Tool '" + id + "' not found in registry");
                return 1;
            }

            auto& tool = it->second;

            // Install dependencies first
            for (auto& dep : tool.requires_tools) {
                auto dep_it = reg.tools.find(dep);
                if (dep_it == reg.tools.end()) continue;
                auto dep_loc = locate_tool(dep_it->second);
                if (!dep_loc.found) {
                    std::cout << "  Installing dependency: " << dep << "\n";
                    if (dep_it->second.install_method == "binary_download")
                        install_binary_tool(dep_it->second);
                    else if (dep_it->second.install_method == "python_pip")
                        install_python_tool(dep_it->second, reg);
                }
            }

            ToolInstallResult result;
            if (tool.install_method == "binary_download")
                result = install_binary_tool(tool, force);
            else if (tool.install_method == "python_pip")
                result = install_python_tool(tool, reg, force);
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
        std::cout << "Tool Health " << dim("(" + platform + ")") << ":\n\n";
        int issues = 0;
        for (auto& [id, tool] : reg.tools) {
            auto loc = locate_tool(tool);
            if (loc.found) {
                print_ok(tool.display_name + " — " + loc.source + " (" + loc.path.string() + ")");
            } else {
                bool available = tool.binary_sources.count(platform) || tool.install_method == "python_pip";
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
