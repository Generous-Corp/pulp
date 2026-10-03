// cmd_dev.cpp — pulp dev: unified development loop
// Combines build --watch + test + validate + process supervision in one command.

#include "build_plan.hpp"
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
    bool allow_unsupported_sdk = false;
    bool examples = false;
    bool hot_dsp = false;
    bool build_all = false;
    bool after_separator = false;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help" || args[i] == "-h") {
            std::cout << "pulp dev — unified development loop\n\n";
            std::cout << "Usage: pulp dev [options] [-- launch-args...]\n\n";
            std::cout << "Watches source files for changes and rebuilds automatically.\n";
            std::cout
                << "Optionally runs tests, validates plugins, and manages a launched app.\n\n";
            std::cout << "Options:\n";
            std::cout << "  --test, -t             Run tests after each successful build\n";
            std::cout << "  --test-filter=PATTERN  Run only tests matching PATTERN\n";
            std::cout
                << "  --validate             Run quick plugin validation (dlopen) after build\n";
            std::cout
                << "  --run TARGET           Launch TARGET from build dir, relaunch on rebuild\n";
            std::cout << "  --hot-dsp              With --run: keep the app alive across rebuilds "
                         "so its\n";
            std::cout << "                         ReloadableShell hot-swaps the rebuilt logic "
                         "live (no relaunch)\n";
            std::cout
                << "  --design SCRIPT        Launch design tool with SCRIPT, relaunch on rebuild\n";
            std::cout << "  --target T             Pass --target T to cmake --build\n";
            std::cout
                << "  --all                  Build every target and run every test (default: only\n"
                   "                         those affected by the working diff)\n";
            std::cout
                << "  --examples             Configure the source checkout with example projects\n";
            std::cout
                << "  --allow-unsupported-sdk  Bypass the CLI-vs-project SDK guard (unsupported)\n";
            std::cout << "  -- args...             Arguments passed to the launched app\n\n";
            std::cout << "Examples:\n";
            std::cout << "  pulp dev                          # Watch and rebuild\n";
            std::cout << "  pulp dev --test                   # Watch, rebuild, test\n";
            std::cout << "  pulp dev --test --validate        # Watch, rebuild, test, validate\n";
            std::cout << "  pulp dev --run pulp-gain-standalone  # Watch, rebuild, relaunch app\n";
            std::cout << "  pulp dev --hot-dsp --run pulp-hot-reload-demo-standalone  # Watch, "
                         "rebuild, live DSP hot-swap\n";
            std::cout
                << "  pulp dev --design ui.js           # Watch, rebuild design tool, relaunch\n";
            std::cout
                << "  pulp dev --test-filter=Knob       # Watch, rebuild, run Knob tests only\n";
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
        } else if (args[i] == "--allow-unsupported-sdk") {
            allow_unsupported_sdk = true;
        } else if (args[i] == "--examples") {
            examples = true;
        } else if (args[i] == "--hot-dsp") {
            hot_dsp = true;
        } else if (args[i] == "--all") {
            build_all = true;
        } else if (args[i] == "--validate") {
            run_validate = true;
        } else if (args[i] == "--run") {
            if (i + 1 >= args.size() || (!args[i + 1].empty() && args[i + 1][0] == '-')) {
                std::cerr << "pulp dev: --run requires a value\n";
                return 2;
            }
            launch_target = args[++i];
        } else if (args[i] == "--design") {
            if (i + 1 >= args.size() || (!args[i + 1].empty() && args[i + 1][0] == '-')) {
                std::cerr << "pulp dev: --design requires a value\n";
                return 2;
            }
            // Build the design tool target and launch it with the script.
            // pulp-design-tool lives under examples/.
            auto script = args[++i];
            examples = true;
            build_args.push_back("--target");
            build_args.push_back("pulp-design-tool");

            // Find design binary
            std::vector<fs::path> candidates = {
                platform_executable(build_dir / "tools" / "design" / "pulp-design-tool"),
                platform_executable(build_dir / "examples" / "design-tool" / "pulp-design-tool"),
            };
            for (const auto& c : candidates) {
                if (fs::exists(c)) {
                    launch_target = c.string();
                    break;
                }
            }
            if (launch_target.empty()) {
                // Will be found after first build
                launch_target = (build_dir / "tools" / "design" / "pulp-design-tool").string();
            }
            launch_args.insert(launch_args.begin(), script);
        } else if (args[i] == "--target") {
            if (i + 1 >= args.size() || (!args[i + 1].empty() && args[i + 1][0] == '-')) {
                std::cerr << "pulp dev: --target requires a value\n";
                return 2;
            }
            build_args.push_back("--target");
            build_args.push_back(args[++i]);
        } else {
            build_args.push_back(args[i]);
        }
    }

    if (!enforce_project_cli_compatibility(project_root, "pulp dev", allow_unsupported_sdk)) {
        return 1;
    }
    if (hot_dsp && launch_target.empty()) {
        std::cerr << "pulp dev: --hot-dsp requires --run <reloadable-shell target>\n";
        return 2;
    }

    BuildPlan plan(
        {project_root, build_dir, standalone_mode, examples, build_all, build_args, "pulp dev"});
    if (!plan.ok()) {
        std::cerr << "pulp dev: " << plan.error() << "\n";
        return plan.exit_code();
    }
    int rc = plan.ensure_configured(allow_unsupported_sdk);
    if (rc != 0)
        return rc;

    rc = plan.select_and_build();
    if (rc != 0) {
        std::cerr << "Initial build failed.\n";
        // Continue anyway — the watch loop will retry
    }

    // Enter the watch loop
    return watch_loop(plan.watch_options(run_tests, test_filter, run_validate, launch_target,
                                         launch_args, hot_dsp));
}
