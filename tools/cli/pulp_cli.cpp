// pulp — CLI tool for the Pulp audio plugin framework
// Command dispatch and usage. See cmd_*.cpp for individual commands.

#include "cli_common.hpp"

#include <cstring>
#include <iostream>

// ── Command Forward Declarations ────────────────────────────────────────────
// cmd_build is declared in cli_common.hpp (needed by cmd_test)

int cmd_test(const std::vector<std::string>& args);
int cmd_status(const std::vector<std::string>& args);
int cmd_clean(const std::vector<std::string>& args);
int cmd_cache(const std::vector<std::string>& args);
int cmd_upgrade(const std::vector<std::string>& args);
int cmd_run(const std::vector<std::string>& args);
int cmd_design(const std::vector<std::string>& args);
int cmd_validate(const std::vector<std::string>& args);
int cmd_ship(const std::vector<std::string>& args);
int cmd_doctor(const std::vector<std::string>& args);
int cmd_create(const std::vector<std::string>& args);
int cmd_docs(const std::vector<std::string>& args);

// ── Usage ───────────────────────────────────────────────────────────────────

static void print_usage() {
    std::cout << "pulp — Pulp audio plugin framework CLI\n\n";
    std::cout << "Usage: pulp <command> [options]\n\n";
    std::cout << "Commands:\n";
    std::cout << "  create   Create a new plugin project (scaffold + build + test)\n";
    std::cout << "  build    Build the project (configure + compile)\n";
    std::cout << "  run      Launch a standalone Pulp application\n";
    std::cout << "  test     Run the test suite\n";
    std::cout << "  status   Show project status and info\n";
    std::cout << "  validate Run plugin format validators (CLAP, VST3, AU, optional AAX)\n";
    std::cout << "  ship     Sign, package, and check plugins\n";
    std::cout << "  cache    Manage SDK and asset cache (~/.pulp/)\n";
    std::cout << "  docs     Browse local documentation\n";
    std::cout << "  doctor   Diagnose environment issues (--fix, --ci, --dry-run)\n";
    std::cout << "  ci-local Run local-first CI across this Mac and configured hosts\n";
    std::cout << "  upgrade  Update the Pulp CLI to the latest version\n";
    std::cout << "  clean    Remove build directory\n";
    std::cout << "  inspect  Launch the component inspector\n";
    std::cout << "  design          AI-powered style design (natural language -> token diffs)\n";
    std::cout << "  design-debug    Headless before/after/diff runner for design chat prompts\n";
    std::cout << "  import-design   Import designs from Figma/Stitch/v0/Pencil\n";
    std::cout << "  export-tokens   Export theme as W3C Design Tokens\n";
    std::cout << "  audit           License and clean-room audit\n";
    std::cout << "  help     Show this help\n";
    std::cout << "\nExamples:\n";
    std::cout << "  pulp create MyPlugin              # Create a new effect plugin\n";
    std::cout << "  pulp create MySynth --type instrument  # Create an instrument\n";
    std::cout << "  pulp create DebugKnob --in-tree   # Add an example under examples/\n";
    std::cout << "  pulp doctor             # Check environment for issues\n";
    std::cout << "  pulp doctor --fix       # Auto-fix issues where possible\n";
    std::cout << "  pulp build              # Build all targets\n";
    std::cout << "  pulp build --target X   # Build specific target\n";
    std::cout << "  pulp test               # Run all tests\n";
    std::cout << "  pulp test -R Knob       # Run tests matching 'Knob'\n";
    std::cout << "  pulp validate           # Validate built plugins\n";
    std::cout << "  pulp cache status       # Show cached SDKs and assets\n";
    std::cout << "  pulp cache fetch skia   # Download Skia GPU binaries\n";
    std::cout << "  pulp docs index         # List available docs\n";
    std::cout << "  pulp status             # Show project info\n";
    std::cout << "  pulp ci-local cloud workflows\n";
    std::cout << "  pulp ci-local cloud run build feature/my-branch\n";
    std::cout << "  pulp design             # Build and launch the design tool\n";
    std::cout << "  pulp design --script path/to/design-tool.js\n";
    std::cout << "  pulp design --build-dir /tmp/pulp-design-parity-build\n";
}

// ── Main ─────────────────────────────────────────────────────────────────────

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

    // ── Named commands (implemented in cmd_*.cpp) ───────────────────────
    if (command == "build")    return cmd_build(args);
    if (command == "run")      return cmd_run(args);
    if (command == "test")     return cmd_test(args);
    if (command == "status")   return cmd_status(args);
    if (command == "validate") return cmd_validate(args);
    if (command == "doctor")   return cmd_doctor(args);
    if (command == "upgrade")  return cmd_upgrade(args);
    if (command == "ship")     return cmd_ship(args);
    if (command == "docs")     return cmd_docs(args);
    if (command == "clean")    return cmd_clean(args);
    if (command == "cache")    return cmd_cache(args);
    if (command == "create")   return cmd_create(args);
    if (command == "design")   return cmd_design(args);

    // ── Script-delegation commands ──────────────────────────────────────
    if (command == "add") {
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        auto script = root / "tools" / "add-component.py";
        if (!fs::exists(script)) {
            std::cerr << "Error: add-component script not found\n";
            return 1;
        }
        std::string cmd = "python3 \"" + script.string() + "\"";
        for (auto& arg : args) cmd += " \"" + arg + "\"";
        return run(cmd);
    }
    if (command == "audit") {
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        auto script = root / "tools" / "audit.py";
        if (!fs::exists(script)) {
            std::cerr << "Error: audit script not found at " << script.string() << "\n";
            return 1;
        }
        std::string cmd = "python3 \"" + script.string() + "\"";
        for (auto& arg : args) cmd += " \"" + arg + "\"";
        return run(cmd);
    }
    if (command == "ci-local") {
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        auto script = root / "tools" / "local-ci" / "local_ci.py";
        if (!fs::exists(script)) {
            std::cerr << "Error: local-ci script not found at " << script.string() << "\n";
            return 1;
        }
        std::string cmd = "python3 \"" + script.string() + "\"";
        for (auto& arg : args) cmd += " \"" + arg + "\"";
        return run(cmd);
    }

    // ── Binary-delegation commands ──────────────────────────────────────
    if (command == "design-debug") {
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        auto debug_bin = root / "build" / "tools" / "design" / "pulp-design-debug";
        if (!fs::exists(debug_bin)) {
            std::cerr << "Error: pulp-design-debug not built. Run `pulp build` first.\n";
            return 1;
        }
        std::string cmd = debug_bin.string();
        for (auto& arg : args) cmd += " \"" + arg + "\"";
        return run(cmd);
    }
    if (command == "inspect") {
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        auto screenshot_bin = root / "build" / "tools" / "screenshot" / "pulp-screenshot";
        if (!fs::exists(screenshot_bin)) {
            std::cerr << "Error: pulp-screenshot not built. Run `pulp build` first.\n";
            return 1;
        }
        std::string cmd = screenshot_bin.string() + " --demo";
        for (auto& arg : args) cmd += " " + arg;
        return run(cmd);
    }
    if (command == "import-design" || command == "export-tokens") {
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        auto import_bin = root / "build" / "tools" / "import-design" / "pulp-import-design";
        if (!fs::exists(import_bin)) {
            std::cerr << "Error: pulp-import-design not built. Run `pulp build` first.\n";
            return 1;
        }
        std::string cmd = import_bin.string();
        if (command == "export-tokens") cmd += " --export-tokens";
        for (auto& arg : args) cmd += " \"" + arg + "\"";
        return run(cmd);
    }

    // ── Help ────────────────────────────────────────────────────────────
    if (command == "help" || command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    std::cerr << "Run `pulp help` for usage\n";
    return 1;
}
