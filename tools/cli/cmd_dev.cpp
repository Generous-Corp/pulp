// cmd_dev.cpp — pulp dev: unified development loop
// Combines build --watch + test + validate + process supervision in one command.

#include "cli_common.hpp"

#include <iostream>

int cmd_dev(const std::vector<std::string>& args) {
    bool standalone_mode = false;
    auto project_root = resolve_active_project_root(&standalone_mode);
    if (project_root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto build_dir = project_root / "build";

    // Parse flags
    bool run_tests = false;
    bool run_validate = false;
    std::string test_filter;
    std::string launch_target;
    std::vector<std::string> launch_args;
    std::vector<std::string> build_args;
    bool after_separator = false;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help" || args[i] == "-h") {
            std::cout << "pulp dev — unified development loop\n\n";
            std::cout << "Usage: pulp dev [options] [-- launch-args...]\n\n";
            std::cout << "Watches source files for changes and rebuilds automatically.\n";
            std::cout << "Optionally runs tests, validates plugins, and manages a launched app.\n\n";
            std::cout << "Options:\n";
            std::cout << "  --test, -t             Run tests after each successful build\n";
            std::cout << "  --test-filter=PATTERN  Run only tests matching PATTERN\n";
            std::cout << "  --validate             Run quick plugin validation (dlopen) after build\n";
            std::cout << "  --run TARGET           Launch TARGET from build dir, relaunch on rebuild\n";
            std::cout << "  --design SCRIPT        Launch design tool with SCRIPT, relaunch on rebuild\n";
            std::cout << "  --target T             Pass --target T to cmake --build\n";
            std::cout << "  -- args...             Arguments passed to the launched app\n\n";
            std::cout << "Examples:\n";
            std::cout << "  pulp dev                          # Watch and rebuild\n";
            std::cout << "  pulp dev --test                   # Watch, rebuild, test\n";
            std::cout << "  pulp dev --test --validate        # Watch, rebuild, test, validate\n";
            std::cout << "  pulp dev --run pulp-gain-standalone  # Watch, rebuild, relaunch app\n";
            std::cout << "  pulp dev --design ui.js           # Watch, rebuild design tool, relaunch\n";
            std::cout << "  pulp dev --test-filter=Knob       # Watch, rebuild, run Knob tests only\n";
            return 0;
        }

        if (args[i] == "--") {
            after_separator = true;
            continue;
        }

        if (after_separator) {
            launch_args.push_back(args[i]);
            continue;
        }

        if (args[i] == "--test" || args[i] == "-t") {
            run_tests = true;
        } else if (args[i].rfind("--test-filter=", 0) == 0) {
            test_filter = args[i].substr(14);
            run_tests = true;
        } else if (args[i] == "--validate") {
            run_validate = true;
        } else if (args[i] == "--run" && i + 1 < args.size()) {
            launch_target = args[++i];
        } else if (args[i] == "--design" && i + 1 < args.size()) {
            // Build the design tool target and launch it with the script
            auto script = args[++i];
            build_args.push_back("--target");
            build_args.push_back("pulp-design-tool");

            // Find design binary
            std::vector<fs::path> candidates = {
                platform_executable(build_dir / "tools" / "design" / "pulp-design"),
                platform_executable(build_dir / "examples" / "design-tool" / "pulp-design-tool"),
            };
            for (const auto& c : candidates) {
                if (fs::exists(c)) { launch_target = c.string(); break; }
            }
            if (launch_target.empty()) {
                // Will be found after first build
                launch_target = (build_dir / "tools" / "design" / "pulp-design").string();
            }
            launch_args.insert(launch_args.begin(), script);
        } else if (args[i] == "--target" && i + 1 < args.size()) {
            build_args.push_back("--target");
            build_args.push_back(args[++i]);
        } else {
            build_args.push_back(args[i]);
        }
    }

    // Ensure configured
    if (!fs::exists(build_dir / "CMakeCache.txt")) {
        std::cout << "Project not configured. Building first...\n";
        int rc = cmd_build({});
        if (rc != 0) return rc;
    }

    // Initial build
    std::string build_cmd = "cmake --build " + build_dir.string();
    for (auto& arg : build_args) build_cmd += " " + arg;
    int rc = run_with_spinner(build_cmd, "Building");
    if (rc != 0) {
        std::cerr << "Initial build failed.\n";
        // Continue anyway — the watch loop will retry
    }

    // Enter the watch loop
    WatchOptions opts;
    opts.root = project_root;
    opts.build_dir = build_dir;
    opts.build_args = build_args;
    opts.run_tests = run_tests;
    opts.test_filter = test_filter;
    opts.run_validate = run_validate;
    opts.launch_target = launch_target;
    opts.launch_args = launch_args;
    return watch_loop(opts);
}
