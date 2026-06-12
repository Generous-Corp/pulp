// mcp_tools.cpp — MCP tool-call handlers for pulp-mcp.
//
// Extracted from tools/mcp/pulp_mcp.cpp in the 2026-05 Phase 6 (B4)
// refactor. See mcp_tools.hpp for the dispatch surface. Each handler
// shells out to the `pulp` CLI (build/test/validate) or queries the
// bundled audio services (model store, excerpt service), then wraps
// the result via json_tool_payload.

#include "mcp_tools.hpp"
#include "mcp_json.hpp"
#include "mcp_shell.hpp"

#include <pulp/tools/audio/model_store.hpp>
#include <pulp/tools/audio/excerpt_service.hpp>
#include <pulp/tools/audio/service.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace fs = std::filesystem;

namespace pulp_mcp {

namespace {

std::string trim_copy(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string strip_quotes_copy(const std::string& s) {
    if (s.size() >= 2
        && ((s.front() == '"' && s.back() == '"')
            || (s.front() == '\'' && s.back() == '\'')))
        return s.substr(1, s.size() - 2);
    return s;
}

std::string normalize_pref(std::string value) {
    value = strip_quotes_copy(trim_copy(value));
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool valid_import_design_mode(const std::string& value) {
    return value == "live" || value == "baked";
}

bool valid_import_design_emit(const std::string& value) {
    return value == "js" || value == "ir-json" || value == "cpp";
}

fs::path pulp_home_path() {
    if (const char* home = std::getenv("PULP_HOME"); home && *home)
        return fs::path(home);
#ifdef _WIN32
    if (const char* home = std::getenv("USERPROFILE"); home && *home)
        return fs::path(home) / ".pulp";
#else
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".pulp";
#endif
    return {};
}

std::string read_config_value(const std::string& section, const std::string& key) {
    const auto home = pulp_home_path();
    if (home.empty()) return {};
    std::ifstream f(home / "config.toml");
    if (!f.is_open()) return {};
    std::string line;
    std::string current_section;
    while (std::getline(f, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos) line = line.substr(0, comment);
        const auto trimmed = trim_copy(line);
        if (trimmed.empty()) continue;
        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            current_section = trim_copy(trimmed.substr(1, trimmed.size() - 2));
            continue;
        }
        if (current_section != section) continue;
        const auto eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        if (trim_copy(trimmed.substr(0, eq)) != key) continue;
        return strip_quotes_copy(trim_copy(trimmed.substr(eq + 1)));
    }
    return {};
}

std::string import_design_defaults_line() {
    std::string mode = "live";
    std::string emit = "js";
    std::string mode_source = "built-in";
    std::string emit_source = "built-in";

    auto apply_emit = [&](std::string raw, std::string source) -> bool {
        raw = normalize_pref(std::move(raw));
        if (!valid_import_design_emit(raw)) {
            return false;
        }
        emit = std::move(raw);
        emit_source = std::move(source);
        return true;
    };
    auto apply_mode = [&](std::string raw, std::string source) -> bool {
        raw = normalize_pref(std::move(raw));
        if (!valid_import_design_mode(raw)) {
            return false;
        }
        mode = std::move(raw);
        mode_source = std::move(source);
        return true;
    };

    if (const char* env = std::getenv("PULP_IMPORT_DESIGN_DEFAULT_EMIT"); env && *env) {
        if (!apply_emit(env, "env:PULP_IMPORT_DESIGN_DEFAULT_EMIT")) {
            return "Import design defaults: invalid (import_design.default_emit must be one of: js, ir-json, cpp from env:PULP_IMPORT_DESIGN_DEFAULT_EMIT)\n";
        }
    } else if (auto configured = read_config_value("import_design", "default_emit");
               !configured.empty()) {
        if (!apply_emit(configured, "config:import_design.default_emit")) {
            return "Import design defaults: invalid (import_design.default_emit must be one of: js, ir-json, cpp from config:import_design.default_emit)\n";
        }
    }
    if (const char* env = std::getenv("PULP_IMPORT_DESIGN_DEFAULT_MODE"); env && *env) {
        if (!apply_mode(env, "env:PULP_IMPORT_DESIGN_DEFAULT_MODE")) {
            return "Import design defaults: invalid (import_design.default_mode must be one of: live, baked from env:PULP_IMPORT_DESIGN_DEFAULT_MODE)\n";
        }
    } else if (auto configured = read_config_value("import_design", "default_mode");
               !configured.empty()) {
        if (!apply_mode(configured, "config:import_design.default_mode")) {
            return "Import design defaults: invalid (import_design.default_mode must be one of: live, baked from config:import_design.default_mode)\n";
        }
    }

    if (emit_source == "built-in" && mode == "baked") {
        emit = "ir-json";
        emit_source = "implied by " + mode_source;
    }
    if (mode_source == "built-in" && (emit == "ir-json" || emit == "cpp")) {
        mode = "baked";
        mode_source = "implied by " + emit_source;
    }

    return "Import design defaults: --mode " + mode + " (" + mode_source
        + "), --emit " + emit + " (" + emit_source + ")\n";
}

} // namespace

std::string handle_kit_validate(const std::string& params_json);
std::string handle_kit_search(const std::string& params_json);
std::string handle_kit_inspect(const std::string& params_json);
std::string handle_kit_plan(const std::string& params_json);
std::string handle_kit_apply(const std::string& params_json);
std::string handle_kit_remove(const std::string& params_json);
std::string handle_kit_pack(const std::string& params_json);
std::string handle_kit_publish_check(const std::string& params_json);
std::string handle_kit_init(const std::string& params_json);
std::string handle_content_validate(const std::string& params_json);
std::string handle_content_preview(const std::string& params_json);
std::string handle_content_install(const std::string& params_json);
std::string handle_content_update(const std::string& params_json);
std::string handle_content_list(const std::string& params_json);
std::string handle_content_rescan(const std::string& params_json);
std::string handle_content_remove(const std::string& params_json);
std::string handle_content_reveal(const std::string& params_json);

std::string handle_build(const std::string& /*params_json*/) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto build_dir = root / "build";
    std::string output;

    if (!fs::exists(build_dir / "CMakeCache.txt")) {
        output += exec("cmake -B " + shell_quote(build_dir.string()) +
                       " -S " + shell_quote(root.string()) + " 2>&1");
    }
    output += exec("cmake --build " + shell_quote(build_dir.string()) + " 2>&1");

    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_test(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto build_dir = root / "build";
    std::string cmd = "ctest --test-dir " + shell_quote(build_dir.string()) + " --output-on-failure";

    auto filter = extract_string(params_json, "filter");
    if (!filter.empty()) cmd += " -R " + shell_quote(filter);

    auto output = exec(cmd + " 2>&1");
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_status(const std::string& /*params_json*/) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    std::ostringstream out;
    out << "Pulp Project: " << root.string() << "\n";

    auto branch = exec("git -C " + shell_quote(root.string()) + " branch --show-current 2>/dev/null");
    if (!branch.empty()) {
        while (!branch.empty() && branch.back() == '\n') branch.pop_back();
        out << "Branch: " << branch << "\n";
    }

    auto build_dir = root / "build";
    out << "Build: " << (fs::exists(build_dir / "CMakeCache.txt") ? "configured" : "not configured") << "\n";
    out << import_design_defaults_line();

    int src = 0, hdr = 0, tests = 0;
    for (auto& e : fs::recursive_directory_iterator(root / "core")) {
        auto ext = e.path().extension().string();
        if (ext == ".cpp" || ext == ".mm") ++src;
        if (ext == ".hpp" || ext == ".h") ++hdr;
    }
    if (fs::exists(root / "test")) {
        for (auto& e : fs::directory_iterator(root / "test")) {
            if (e.path().extension() == ".cpp") ++tests;
        }
    }
    out << "Sources: " << src << " impl, " << hdr << " headers, " << tests << " test files\n";

    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(out.str()) + "}]}";
}

std::string handle_validate(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " validate --json";
    if (extract_bool(params_json, "all", false)) {
        cmd += " --all";
    }
    cmd += " 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_kit_validate(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    if (path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) +
        " kit validate " + shell_quote(path) + " --json";
    if (extract_bool(params_json, "strict", false)) {
        cmd += " --strict";
    }
    cmd += " 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_kit(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto subcommand = extract_string(params_json, "subcommand");
    if (subcommand.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: subcommand is required\"}]}";
    }
    if (subcommand == "search") return handle_kit_search(params_json);
    if (subcommand == "validate") return handle_kit_validate(params_json);
    if (subcommand == "inspect" || subcommand == "show") return handle_kit_inspect(params_json);
    if (subcommand == "plan" || subcommand == "preview") return handle_kit_plan(params_json);
    if (subcommand == "verify") return handle_kit_verify(params_json);
    if (subcommand == "apply") return handle_kit_apply(params_json);
    if (subcommand == "remove" || subcommand == "uninstall") return handle_kit_remove(params_json);
    if (subcommand == "pack") return handle_kit_pack(params_json);
    if (subcommand == "publish" || subcommand == "publish-check") return handle_kit_publish_check(params_json);
    if (subcommand == "init") return handle_kit_init(params_json);
    return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: unsupported kit subcommand\"}]}";
}

std::string handle_kit_search(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " kit search";
    auto query = extract_string(params_json, "query");
    if (!query.empty()) cmd += " " + shell_quote(query);
    auto search_root = extract_string(params_json, "root");
    if (!search_root.empty()) cmd += " --root " + shell_quote(search_root);
    auto kind = extract_string(params_json, "kind");
    if (!kind.empty()) cmd += " --kind " + shell_quote(kind);
    auto lane = extract_string(params_json, "lane");
    if (!lane.empty()) cmd += " --lane " + shell_quote(lane);
    cmd += " --json 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_kit_inspect(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    if (path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) +
        " kit inspect " + shell_quote(path) + " --json 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_kit_plan(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    if (path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) +
        " kit plan " + shell_quote(path) + " --project " + shell_quote(root.string()) + " --json 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_kit_verify(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    if (path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) +
        " kit verify " + shell_quote(path) + " --project " + shell_quote(root.string()) + " --json 2>&1";
    if (extract_bool(params_json, "execute_screenshots", false)) {
        cmd = shell_quote(resolve_cli_binary(root).string()) +
            " kit verify " + shell_quote(path) + " --project " + shell_quote(root.string())
            + " --json --execute-screenshots";
        auto backend = extract_string(params_json, "screenshot_backend");
        if (!backend.empty()) cmd += " --screenshot-backend " + shell_quote(backend);
        auto output_dir = extract_string(params_json, "screenshot_output_dir");
        if (!output_dir.empty()) cmd += " --screenshot-output-dir " + shell_quote(output_dir);
        cmd += " 2>&1";
    }

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_kit_apply(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    if (path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    }
    if (!extract_bool(params_json, "yes", false)) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: yes=true is required after reviewing the kit plan\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) +
        " kit apply " + shell_quote(path) + " --project " + shell_quote(root.string()) + " --yes 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_kit_remove(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto id = extract_string(params_json, "id");
    if (id.empty()) {
        id = extract_string(params_json, "kit_id");
    }
    if (id.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: id is required\"}]}";
    }
    if (!extract_bool(params_json, "yes", false)) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: yes=true is required after reviewing installed kit ownership\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) +
        " kit remove " + shell_quote(id) + " --project " + shell_quote(root.string()) + " --yes 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_kit_pack(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    if (path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) +
        " kit pack " + shell_quote(path) + " --json";
    auto output = extract_string(params_json, "output");
    if (!output.empty()) cmd += " --output " + shell_quote(output);
    cmd += " 2>&1";

    auto result = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(result) + "}]}";
}

std::string handle_kit_publish_check(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    if (path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) +
        " kit publish " + shell_quote(path) + " --dry-run --json";
    auto registry_manifest = extract_string(params_json, "registry_manifest");
    if (registry_manifest.empty()) registry_manifest = extract_string(params_json, "registryManifest");
    if (!registry_manifest.empty())
        cmd += " --registry-manifest " + shell_quote(registry_manifest);
    cmd += " 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_kit_init(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto kind = extract_string(params_json, "kind");
    auto id = extract_string(params_json, "id");
    if (kind.empty() || id.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: kind and id are required\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) +
        " kit init --kind " + shell_quote(kind) + " --id " + shell_quote(id);
    auto name = extract_string(params_json, "name");
    if (!name.empty()) cmd += " --name " + shell_quote(name);
    auto dir = extract_string(params_json, "dir");
    if (!dir.empty()) cmd += " --dir " + shell_quote(dir);
    if (extract_bool(params_json, "force", false)) cmd += " --force";
    cmd += " 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_content(const std::string& params_json) {
    auto subcommand = extract_string(params_json, "subcommand");
    if (subcommand.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: subcommand is required\"}]}";
    }
    if (subcommand == "validate") return handle_content_validate(params_json);
    if (subcommand == "preview") return handle_content_preview(params_json);
    if (subcommand == "install") return handle_content_install(params_json);
    if (subcommand == "update") return handle_content_update(params_json);
    if (subcommand == "list") return handle_content_list(params_json);
    if (subcommand == "rescan") return handle_content_rescan(params_json);
    if (subcommand == "remove" || subcommand == "uninstall") return handle_content_remove(params_json);
    if (subcommand == "reveal") return handle_content_reveal(params_json);
    return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: unsupported content subcommand\"}]}";
}

std::string handle_content_validate(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    if (path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) +
        " content validate " + shell_quote(path) + " --json 2>&1";
    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_content_preview(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    auto plugin_runtime = extract_string(params_json, "plugin_runtime");
    if (plugin_runtime.empty()) plugin_runtime = extract_string(params_json, "pluginRuntime");
    if (path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    }
    if (plugin_runtime.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: plugin_runtime is required\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) +
        " content preview " + shell_quote(path) +
        " --plugin-runtime " + shell_quote(plugin_runtime) +
        " --json";
    auto plugin = extract_string(params_json, "plugin");
    if (!plugin.empty()) cmd += " --plugin " + shell_quote(plugin);
    cmd += " 2>&1";
    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_content_install(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    auto plugin = extract_string(params_json, "plugin");
    if (path.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    if (plugin.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: plugin is required\"}]}";
    if (!extract_bool(params_json, "yes", false)) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: yes=true is required after reviewing the content install target\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) +
        " content install " + shell_quote(path) + " --plugin " + shell_quote(plugin) + " --yes";
    auto data_root = extract_string(params_json, "root");
    if (!data_root.empty()) cmd += " --root " + shell_quote(data_root);
    cmd += " 2>&1";
    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_content_update(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto path = extract_string(params_json, "path");
    auto plugin = extract_string(params_json, "plugin");
    if (path.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: path is required\"}]}";
    if (plugin.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: plugin is required\"}]}";
    if (!extract_bool(params_json, "yes", false)) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: yes=true is required after reviewing the content update target\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) +
        " content update " + shell_quote(path) + " --plugin " + shell_quote(plugin) + " --yes";
    auto data_root = extract_string(params_json, "root");
    if (!data_root.empty()) cmd += " --root " + shell_quote(data_root);
    cmd += " 2>&1";
    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_content_list(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " content list --json";
    auto plugin = extract_string(params_json, "plugin");
    if (!plugin.empty()) cmd += " --plugin " + shell_quote(plugin);
    auto data_root = extract_string(params_json, "root");
    if (!data_root.empty()) cmd += " --root " + shell_quote(data_root);
    cmd += " 2>&1";
    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_content_rescan(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " content rescan --json";
    auto data_root = extract_string(params_json, "root");
    if (!data_root.empty()) cmd += " --root " + shell_quote(data_root);
    cmd += " 2>&1";
    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_content_remove(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto id = extract_string(params_json, "id");
    if (id.empty()) id = extract_string(params_json, "package_id");
    auto plugin = extract_string(params_json, "plugin");
    if (id.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: id is required\"}]}";
    if (plugin.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: plugin is required\"}]}";
    if (!extract_bool(params_json, "yes", false)) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: yes=true is required after reviewing installed content\"}]}";
    }

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) +
        " content remove " + shell_quote(id) + " --plugin " + shell_quote(plugin) + " --yes";
    auto version = extract_string(params_json, "version");
    if (!version.empty()) cmd += " --version " + shell_quote(version);
    auto data_root = extract_string(params_json, "root");
    if (!data_root.empty()) cmd += " --root " + shell_quote(data_root);
    cmd += " 2>&1";
    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_content_reveal(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto id = extract_string(params_json, "id");
    if (id.empty()) id = extract_string(params_json, "package_id");
    auto plugin = extract_string(params_json, "plugin");
    if (id.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: id is required\"}]}";
    if (plugin.empty()) return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: plugin is required\"}]}";

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) +
        " content reveal " + shell_quote(id) + " --plugin " + shell_quote(plugin);
    auto version = extract_string(params_json, "version");
    if (!version.empty()) cmd += " --version " + shell_quote(version);
    auto data_root = extract_string(params_json, "root");
    if (!data_root.empty()) cmd += " --root " + shell_quote(data_root);
    cmd += " 2>&1";
    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_audio_model_status(const std::string& /*params_json*/) {
    auto status = pulp::tools::audio::query_model_status();
    return json_tool_payload(pulp::tools::audio::to_json(status));
}

std::string handle_audio_model_list(const std::string& /*params_json*/) {
    auto result = pulp::tools::audio::list_models();
    return json_tool_payload(pulp::tools::audio::to_json(result));
}

std::string handle_audio_model_activate(const std::string& params_json) {
    auto model_id = extract_string(params_json, "model_id");
    if (model_id.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: model_id is required\"}]}";
    }

    auto result = pulp::tools::audio::activate_model(model_id);
    return json_tool_payload(pulp::tools::audio::to_json(result));
}

std::string handle_audio_read_bundle(const std::string& params_json) {
    auto bundle_path = extract_string(params_json, "bundle_path");
    if (bundle_path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: bundle_path is required\"}]}";
    }

    auto bundle = pulp::tools::audio::read_excerpt_bundle(bundle_path);
    return json_tool_payload(pulp::tools::audio::to_json(bundle));
}


std::string handle_audio_excerpt_find(const std::string& params_json) {
    auto text = extract_string(params_json, "text");
    auto input_path = extract_string(params_json, "input_path");
    if (text.empty() || input_path.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: text and input_path are required\"}]}";
    }

    pulp::tools::audio::ExcerptFindRequest request;
    request.text = text;
    request.input_path = input_path;
    request.model_id = extract_string(params_json, "model_id");
    request.recursive = extract_bool(params_json, "recursive", false);
    request.top_k = static_cast<std::size_t>(extract_int(params_json, "top", 5));
    request.window_ms = static_cast<uint64_t>(extract_int(params_json, "window_ms", 1500));
    request.hop_ms = static_cast<uint64_t>(extract_int(params_json, "hop_ms", 250));
    request.min_score = extract_double(params_json, "min_score", 0.0);
    request.max_candidates_per_file =
        static_cast<std::size_t>(extract_int(params_json, "max_candidates_per_file", 3));
    request.bundle_out = extract_string(params_json, "bundle_out");

    auto result = pulp::tools::audio::run_excerpt_find(request);
    return json_tool_payload(pulp::tools::audio::to_json(result));
}

}  // namespace pulp_mcp
