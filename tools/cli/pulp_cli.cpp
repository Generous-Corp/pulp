// pulp — CLI tool for the Pulp audio plugin framework
// Command dispatch via structured table. See cmd_*.cpp for implementations.

#include "cli_common.hpp"
#include "package_commands.hpp"
#include "package_registry.hpp"
#include "tool_registry.hpp"

#include <cstring>
#include <iomanip>
#include <iostream>

// ── Command Table ───────────────────────────────────────────────────────────

struct Command {
    const char* name;
    const char* summary;
    int (*handler)(const std::vector<std::string>&);
};

static const Command commands[] = {
    {"build",    "Configure and build the project",       cmd_build},
    {"run",      "Launch a standalone Pulp application",  cmd_run},
    {"test",     "Run the test suite",                    cmd_test},
    {"status",   "Show project status and info",          cmd_status},
    {"create",   "Scaffold a new plugin project",         cmd_create},
    {"validate", "Run plugin format validators",          cmd_validate},
    {"doctor",   "Diagnose environment issues",           cmd_doctor},
    {"ship",     "Sign, package, and distribute",         cmd_ship},
    {"design",   "Launch the AI design tool",             cmd_design},
    {"docs",     "Browse local documentation",            cmd_docs},
    {"clean",    "Remove build directory",                cmd_clean},
    {"cache",    "Manage SDK and asset cache",            cmd_cache},
    {"audio",    "Repo-level audio model and bundle tooling", cmd_audio},
    {"sdk",      "Manage the Pulp SDK installation",      cmd_sdk},
    {"upgrade",  "Update the CLI to the latest version",  cmd_upgrade},
    {"version",  "Show, bump, or check version info",     cmd_version},
    {"dev",      "Unified dev loop: watch, build, test, run", cmd_dev},
    {"pr",       "One-shot push-a-PR: gates + bump + ship",   cmd_pr},
};

static constexpr int command_count = sizeof(commands) / sizeof(commands[0]);

// Script commands: delegate to a Python script in the project tree
struct ScriptCommand {
    const char* name;
    const char* script_path;  // relative to project root
    const char* summary;
};

static const ScriptCommand script_commands[] = {
    {"ci-local", "tools/local-ci/local_ci.py",       "Local-first CI across configured hosts"},
};

static constexpr int script_command_count = sizeof(script_commands) / sizeof(script_commands[0]);

// Binary commands: delegate to a built binary
struct BinaryCommand {
    const char* name;
    const char* binary_path;  // relative to project build dir
    const char* summary;
    const char* extra_arg;    // prepended arg (e.g., "--demo"), or nullptr
};

static const BinaryCommand binary_commands[] = {
    {"design-debug",  "tools/design/pulp-design-debug",            "Headless design debug runner", nullptr},
    {"inspect",       "tools/screenshot/pulp-screenshot",          "Launch the component inspector", "--demo"},
    {"import-design", "tools/import-design/pulp-import-design",    "Import designs from Figma/Stitch/v0/Pencil", nullptr},
    {"export-tokens", "tools/import-design/pulp-import-design",    "Export theme as W3C Design Tokens", "--export-tokens"},
};

static constexpr int binary_command_count = sizeof(binary_commands) / sizeof(binary_commands[0]);

// ── Delegation Helpers ─────────────────────────────────────────────────────

static int delegate_to_script(const ScriptCommand& sc, const std::vector<std::string>& args) {
    return delegate_to_python_script(sc.script_path, args);
}

static int delegate_to_binary(const BinaryCommand& bc, const std::vector<std::string>& args) {
    return delegate_to_build_binary(bc.binary_path, args,
                                    bc.extra_arg ? std::string(bc.extra_arg) : std::string{});
}

// ── Package Manager Commands ───────────────────────────────────────────��───

static int handle_audit(const std::vector<std::string>& args) {
    bool pkg_flag = false, plat_flag = false, lic_flag = false;
    for (auto& a : args) {
        if (a == "--packages") pkg_flag = true;
        if (a == "--platforms") plat_flag = true;
        if (a == "--licenses") lic_flag = true;
    }
    if (pkg_flag || plat_flag || lic_flag) {
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        int rc = 0;
        if (pkg_flag) rc |= pulp::cli::pkg::audit_packages(root);
        if (plat_flag) rc |= pulp::cli::pkg::audit_platforms(root);
        if (lic_flag) rc |= pulp::cli::pkg::audit_licenses(root);
        return rc;
    }
    return delegate_to_python_script("tools/audit.py", args);
}

// ── Usage (auto-generated from command tables) ──────────────────────────────

static void print_usage() {
    std::cout << "pulp — Pulp audio plugin framework CLI\n\n";
    std::cout << "Usage: pulp <command> [options]\n\n";
    std::cout << "Commands:\n";
    for (int i = 0; i < command_count; ++i) {
        std::cout << "  " << std::left << std::setw(14) << commands[i].name
                  << " " << commands[i].summary << "\n";
    }
    std::cout << "\n";
    for (int i = 0; i < script_command_count; ++i) {
        std::cout << "  " << std::left << std::setw(14) << script_commands[i].name
                  << " " << script_commands[i].summary << "\n";
    }
    for (int i = 0; i < binary_command_count; ++i) {
        std::cout << "  " << std::left << std::setw(14) << binary_commands[i].name
                  << " " << binary_commands[i].summary << "\n";
    }
    std::cout << "  " << std::left << std::setw(14) << "audit" << " License and clean-room audit\n";
    std::cout << "  " << std::left << std::setw(14) << "add" << " Add a component to the project\n";
    std::cout << "  " << std::left << std::setw(14) << "help" << " Show this help\n";
    std::cout << "\nExamples:\n";
    std::cout << "  pulp create MyPlugin              # Create a new effect plugin\n";
    std::cout << "  pulp create MySynth --type instrument  # Create an instrument\n";
    std::cout << "  pulp doctor             # Check environment for issues\n";
    std::cout << "  pulp build              # Build all targets\n";
    std::cout << "  pulp test               # Run all tests\n";
    std::cout << "  pulp validate           # Validate built plugins\n";
    std::cout << "  pulp docs index         # List available docs\n";
    std::cout << "  pulp status             # Show project info\n";
}

// ── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-color") == 0) {
            g_no_color = true;
        }
    }
    init_color();

    if (argc < 2) {
        print_usage();
        return 0;
    }

    std::string command = argv[1];
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-color") == 0) continue;
        args.push_back(argv[i]);
    }

    // Lookup in command table
    for (int i = 0; i < command_count; ++i) {
        if (command == commands[i].name) return commands[i].handler(args);
    }

    // Lookup in script commands
    for (int i = 0; i < script_command_count; ++i) {
        if (command == script_commands[i].name) return delegate_to_script(script_commands[i], args);
    }

    // Lookup in binary commands
    for (int i = 0; i < binary_command_count; ++i) {
        if (command == binary_commands[i].name) return delegate_to_binary(binary_commands[i], args);
    }

    // Package manager commands
    if (command == "add")      return pulp::cli::pkg::cmd_add(args);
    if (command == "remove")   return pulp::cli::pkg::cmd_remove(args);
    if (command == "list")     return pulp::cli::pkg::cmd_list(args);
    if (command == "search")   return pulp::cli::pkg::cmd_search(args);
    if (command == "update")   return pulp::cli::pkg::cmd_update(args);
    if (command == "suggest")  return pulp::cli::pkg::cmd_suggest(args);
    if (command == "target")   return pulp::cli::pkg::cmd_target(args);
    if (command == "audit")    return handle_audit(args);
    if (command == "tool")     return pulp::cli::tools::cmd_tool(args);

    // Legacy aliases
    if (command == "add-component") {
        return delegate_to_python_script("tools/add-component.py", args);
    }

    if (command == "install") {
        std::cout << "Installing Pulp SDK...\n";
        std::vector<std::string> cache_args = {"fetch", "skia"};
        return cmd_cache(cache_args);
    }

    // Help
    if (command == "help" || command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    // Fuzzy "did you mean?" suggestion
    std::cerr << "Unknown command: " << command << "\n";
    int best_dist = 999;
    const char* best_name = nullptr;
    auto levenshtein = [](const std::string& a, const std::string& b) -> int {
        size_t m = a.size(), n = b.size();
        std::vector<int> dp((m + 1) * (n + 1));
        for (size_t i = 0; i <= m; ++i) dp[i * (n + 1)] = static_cast<int>(i);
        for (size_t j = 0; j <= n; ++j) dp[j] = static_cast<int>(j);
        for (size_t i = 1; i <= m; ++i)
            for (size_t j = 1; j <= n; ++j) {
                int cost = (a[i-1] == b[j-1]) ? 0 : 1;
                dp[i*(n+1)+j] = std::min({dp[(i-1)*(n+1)+j]+1, dp[i*(n+1)+j-1]+1,
                                           dp[(i-1)*(n+1)+j-1]+cost});
            }
        return dp[m*(n+1)+n];
    };
    for (int i = 0; i < command_count; ++i) {
        int d = levenshtein(command, commands[i].name);
        if (d < best_dist) { best_dist = d; best_name = commands[i].name; }
    }
    if (best_dist <= 3 && best_name) {
        std::cerr << "Did you mean: pulp " << best_name << "?\n";
    } else {
        std::cerr << "Run `pulp help` for usage\n";
    }
    return 1;
}
