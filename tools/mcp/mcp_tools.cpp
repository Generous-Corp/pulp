// mcp_tools.cpp — Project build, test, status, and validation MCP handlers.

#include "mcp_tools.hpp"
#include "mcp_json.hpp"
#include "mcp_shell.hpp"
#include "mcp_tools_internal.hpp"

#include <filesystem>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace pulp_mcp {

std::string handle_build(const std::string& /*params_json*/) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto build_dir = root / "build";
    std::string output;

    if (!fs::exists(build_dir / "CMakeCache.txt")) {
        output += exec("cmake -B " + shell_quote(build_dir.string()) + " -S " +
                       shell_quote(root.string()) + " 2>&1");
    }
    output += exec("cmake --build " + shell_quote(build_dir.string()) + " 2>&1");

    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_test(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto build_dir = root / "build";
    std::string cmd =
        "ctest --test-dir " + shell_quote(build_dir.string()) + " --output-on-failure";

    auto filter = extract_string(params_json, "filter");
    if (!filter.empty())
        cmd += " -R " + shell_quote(filter);

    auto output = exec(cmd + " 2>&1");
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_status(const std::string& /*params_json*/) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    std::ostringstream out;
    out << "Pulp Project: " << root.string() << "\n";

    auto branch =
        exec("git -C " + shell_quote(root.string()) + " branch --show-current 2>/dev/null");
    if (!branch.empty()) {
        while (!branch.empty() && branch.back() == '\n')
            branch.pop_back();
        out << "Branch: " << branch << "\n";
    }

    auto build_dir = root / "build";
    out << "Build: " << (fs::exists(build_dir / "CMakeCache.txt") ? "configured" : "not configured")
        << "\n";
    out << import_design_defaults_line();

    int src = 0, hdr = 0, tests = 0;
    for (auto& e : fs::recursive_directory_iterator(root / "core")) {
        auto ext = e.path().extension().string();
        if (ext == ".cpp" || ext == ".mm")
            ++src;
        if (ext == ".hpp" || ext == ".h")
            ++hdr;
    }
    if (fs::exists(root / "test")) {
        for (auto& e : fs::directory_iterator(root / "test")) {
            if (e.path().extension() == ".cpp")
                ++tests;
        }
    }
    out << "Sources: " << src << " impl, " << hdr << " headers, " << tests << " test files\n";

    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(out.str()) + "}]}";
}

std::string handle_validate(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " validate --json";
    if (extract_bool(params_json, "all", false)) {
        cmd += " --all";
    }
    if (extract_bool(params_json, "screenshot", false)) {
        cmd += " --screenshot";
    }
    cmd += " 2>&1";

    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

std::string handle_minos(const std::string& params_json) {
    auto root = find_project_root();
    if (root.empty())
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";

    auto binary = extract_string(params_json, "binary");
    if (binary.empty()) {
        return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: binary is required\"}]}";
    }
    // Delegate to `pulp minos measure <binary>` so the CLI, slash command,
    // and this MCP tool all share one path down to measure_min_os.py.
    std::string cmd = shell_quote(resolve_cli_binary(root).string()) + " minos measure " +
                      shell_quote(binary) + " 2>&1";
    auto output = exec(cmd);
    return "{\"content\":[{\"type\":\"text\",\"text\":" + json_string(output) + "}]}";
}

} // namespace pulp_mcp
