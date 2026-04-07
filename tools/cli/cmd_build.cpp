// cmd_build.cpp — pulp build command

#include "cli_common.hpp"

#include <iostream>

int cmd_build(const std::vector<std::string>& args) {
    bool standalone_mode = false;
    auto project_root = resolve_active_project_root(&standalone_mode);
    if (project_root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto build_dir = project_root / "build";
    bool needs_configure = !fs::exists(build_dir / "CMakeCache.txt");

    // Check if CMakeLists.txt is newer than CMakeCache
    if (!needs_configure && fs::exists(build_dir / "CMakeCache.txt")) {
        auto cmake_time = fs::last_write_time(project_root / "CMakeLists.txt");
        auto cache_time = fs::last_write_time(build_dir / "CMakeCache.txt");
        if (cmake_time > cache_time) needs_configure = true;
    }

    // Extract flags before passing args through
    std::string js_engine;
    bool watch_mode = false;
    bool watch_test = false;
    bool watch_validate = false;
    std::string test_filter;
    std::vector<std::string> passthrough_args;
    for (auto& arg : args) {
        if (arg == "--watch" || arg == "-w") {
            watch_mode = true;
            continue;
        }
        if (arg == "--test" || arg == "-t") {
            watch_test = true;
            continue;
        }
        if (arg == "--validate") {
            watch_validate = true;
            continue;
        }
        if (arg.rfind("--test-filter=", 0) == 0) {
            test_filter = arg.substr(14);
            watch_test = true;
            continue;
        }
        if (arg.rfind("--js-engine=", 0) == 0) {
            js_engine = arg.substr(12);
            if (js_engine != "auto" && js_engine != "quickjs" && js_engine != "jsc" && js_engine != "v8") {
                std::cerr << "Error: --js-engine must be auto, quickjs, jsc, or v8\n";
                return 1;
            }
            needs_configure = true;  // Engine change requires reconfigure
        } else {
            passthrough_args.push_back(arg);
        }
    }

    if (needs_configure) {
        std::string configure_cmd = "cmake -B " + build_dir.string() + " -S " + project_root.string();
        append_windows_visual_studio_generator_args(configure_cmd);

        // Standalone projects need CMAKE_PREFIX_PATH to find the SDK
        if (standalone_mode) {
            auto version = read_sdk_version(project_root);
            auto sdk_dir = read_sdk_path_hint(project_root);
            auto checkout_hint = read_sdk_checkout_hint(project_root);
            auto config = sdk_dir / "lib" / "cmake" / "Pulp" / "PulpConfig.cmake";
            if (sdk_dir.empty() || !fs::exists(config)) {
                if (!checkout_hint.empty() && fs::exists(checkout_hint)) {
                    sdk_dir = ensure_checkout_sdk(checkout_hint, version);
                } else {
                    sdk_dir = ensure_sdk(version);
                }
            }
            if (sdk_dir.empty()) {
                std::cerr << "Error: could not obtain Pulp SDK v" << version << "\n";
                return 1;
            }
            configure_cmd += " -DCMAKE_PREFIX_PATH=" + sdk_dir.string();
        }

        // JS engine selection
        if (!js_engine.empty()) {
            configure_cmd += " -DPULP_JS_ENGINE=" + js_engine;
        }

        int rc = run_with_spinner(configure_cmd, "Configuring");
        if (rc != 0) return rc;
    }

    std::string build_cmd = "cmake --build " + build_dir.string();

    // Pass through extra args (e.g., --target, -j)
    for (auto& arg : passthrough_args) {
        build_cmd += " " + arg;
    }

    int rc = run_with_spinner(build_cmd, "Building");
    if (rc != 0 || !watch_mode) return rc;

    WatchOptions opts;
    opts.root = project_root;
    opts.build_dir = build_dir;
    opts.build_args = passthrough_args;
    opts.run_tests = watch_test;
    opts.test_filter = test_filter;
    opts.run_validate = watch_validate;
    return watch_loop(opts);
}
