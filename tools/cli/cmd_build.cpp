// cmd_build.cpp — pulp build command

#include "cli_common.hpp"
#include "install_paths_mac.hpp"

#include <iostream>

namespace {

// Per CLAUDE.md "Plugin Install Policy":
//   "NEVER install a plugin to system folders without passing validation
//    first."
// This helper runs the validate step before promoting bundles into
// `~/Library/Audio/Plug-Ins/...`. It defers to `cmd_validate` for the
// actual validator orchestration so the policy stays in exactly one
// place — including the missing-tool advisory wording (#51 / #356).
//
// Returns the exit code from cmd_validate. Item 7.4b acceptance #3
// ("when auval is missing from PATH, install of an AU fails with the
// install hint") is implemented by passing --strict, which upgrades
// missing-tool advisories to hard failures so we never silently
// install behind a non-validated tool.
int validate_for_install() {
    std::vector<std::string> validate_args = {"--strict"};
    return cmd_validate(validate_args);
}

// Loud red banner emitted only when the user explicitly waives
// validation via `--skip-validation`. Visibility matters: this is the
// one path where the policy is intentionally bypassed, and the user
// needs to know they've turned it off if a DAW crash later traces back
// to it.
void print_skip_validation_banner() {
    std::cerr << "\n"
              << color::red() << color::bold()
              << "########################################################\n"
              << "#  WARNING: --skip-validation bypasses the install gate #\n"
              << "#  Per CLAUDE.md a plugin that crashes a DAW during    #\n"
              << "#  scan is worse than no plugin at all. Use only for   #\n"
              << "#  debugging adapter code, never for normal use.       #\n"
              << "########################################################\n"
              << color::reset() << "\n";
}

}  // namespace

int cmd_build(const std::vector<std::string>& args) {
    pulp_debug("cmd_build: enter");
    bool standalone_mode = false;
    auto project_root = resolve_active_project_root(&standalone_mode);
    if (project_root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }
    pulp_debug("cmd_build: project root resolved");

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
    bool allow_unsupported_sdk = false;
    // Item 7.4b: install after build (with validation gate by default).
    bool install_mode = false;
    bool skip_validation = false;
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
        if (arg == "--install") {
            install_mode = true;
            continue;
        }
        if (arg == "--skip-validation") {
            skip_validation = true;
            continue;
        }
        if (arg.rfind("--test-filter=", 0) == 0) {
            test_filter = arg.substr(14);
            watch_test = true;
            continue;
        }
        if (arg == "--allow-unsupported-sdk") {
            allow_unsupported_sdk = true;
            continue;
        }
        if (arg == "--js-engine") {
            std::cerr << "Error: --js-engine requires a value\n";
            return 2;
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

    // `--skip-validation` is meaningless without `--install`; reject
    // the combo explicitly so users don't silently bypass a gate that
    // wouldn't have run anyway. The reverse is fine: `--install` alone
    // runs the validation gate (which is the normal mode).
    if (skip_validation && !install_mode) {
        std::cerr << "Error: --skip-validation only applies with --install.\n";
        return 2;
    }
    if (install_mode && watch_mode) {
        std::cerr << "Error: --install cannot be combined with --watch "
                     "(watch loops would re-install on every save).\n";
        return 2;
    }

    if (!enforce_project_cli_compatibility(project_root,
                                           "pulp build",
                                           allow_unsupported_sdk)) {
        return 1;
    }

    // FetchContent cache preflight (issue #744). Cheap (<1s on a
    // typical cache); fails fast with a clear remediation message
    // instead of letting `cmake configure` blow up 200 lines into the
    // log on a dangling symlink. Skipped only when the user has set
    // PULP_SKIP_CACHE_PREFLIGHT or when no cache root is derivable.
    // Only runs when we'd actually be invoking configure — incremental
    // rebuilds shouldn't pay the (small) cache-scan cost.
    if (needs_configure
        && !cache_preflight_check(project_root, "pulp build")) {
        return 1;
    }

    if (needs_configure) {
        std::string configure_cmd = "cmake -B " + build_dir.string() + " -S " + project_root.string();
        append_windows_visual_studio_generator_args(configure_cmd);

        // Standalone projects need CMAKE_PREFIX_PATH to find the SDK
        if (standalone_mode) {
            pulp_debug("cmd_build: resolve SDK (standalone)");
            auto sdk = resolve_standalone_sdk(project_root, true);
            if (!sdk.warning.empty()) {
                print_warn(sdk.warning);
            }
            if (sdk.resolved_sdk_dir.empty()) {
                std::cerr << "Error: could not obtain Pulp SDK v"
                          << sdk.requested_version << "\n";
                return 1;
            }
            configure_cmd += " -DCMAKE_PREFIX_PATH=" + shell_quote(sdk.resolved_sdk_dir);
            pulp_debug("cmd_build: SDK resolved");
        }

        // JS engine selection
        if (!js_engine.empty()) {
            configure_cmd += " -DPULP_JS_ENGINE=" + js_engine;
        }

        pulp_debug("cmd_build: run configure (cmake)");
        int rc = run_with_spinner(configure_cmd, "Configuring");
        if (rc != 0) return rc;
        pulp_debug("cmd_build: configure complete");
    }

    std::string build_cmd = "cmake --build " + build_dir.string();

    // Pass through extra args (e.g., --target, -j)
    for (auto& arg : passthrough_args) {
        build_cmd += " " + arg;
    }

    pulp_debug("cmd_build: run build (cmake --build)");
    int rc = run_with_spinner(build_cmd, "Building");
    if (rc != 0) return rc;

    // Item 7.4b: build → validate → install pipeline.
    //
    // The order matters: validation runs BEFORE the install copy so
    // a broken bundle never reaches `~/Library/Audio/Plug-Ins/`. Per
    // CLAUDE.md "Plugin Install Policy" this is the entire reason for
    // the flag — a plugin that crashes a DAW during scan is worse than
    // no plugin at all.
    if (install_mode) {
#ifndef __APPLE__
        std::cerr << "Error: `pulp build --install` is currently only "
                     "implemented for macOS plugin formats "
                     "(AU/VST3/CLAP).\n";
        return 1;
#else
        if (!skip_validation) {
            int vrc = validate_for_install();
            if (vrc != 0) {
                std::cerr << "\n" << color::red()
                          << "pulp build --install: validation failed — "
                             "refusing to install."
                          << color::reset() << "\n"
                          << "Re-run `pulp validate` to see the failure "
                             "detail, fix the bundle, then retry.\n"
                          << "(Debug-only escape hatch: `pulp build "
                             "--install --skip-validation`.)\n";
                return vrc;
            }
        } else {
            print_skip_validation_banner();
        }

        namespace ip = pulp::cli::install_paths_mac;
        auto env = ip::make_default_env();
        if (env.home_dir.empty()) {
            std::cerr << "Error: $HOME is not set; cannot resolve "
                         "~/Library/Audio/Plug-Ins/...\n";
            return 1;
        }

        auto bundles = ip::discover_bundles(build_dir);
        if (bundles.empty()) {
            std::cerr << "No plugin bundles found under " << build_dir
                      << " — nothing to install.\n";
            return 1;
        }

        int installed = 0, failed = 0;
        for (const auto& entry : bundles) {
            std::cout << "Installing "
                      << entry.bundle_path.filename().string() << " ("
                      << ip::validator_for_kind(entry.kind) << " kind)...\n";
            auto r = ip::install_bundle(env, entry.bundle_path, entry.kind);
            if (r.success) {
                std::cout << "  " << (r.replaced_existing ? "updated " : "installed ")
                          << r.destination.string() << "\n";
                ++installed;
            } else {
                std::cerr << "  " << color::red() << "FAILED" << color::reset()
                          << ": " << r.error << "\n";
                ++failed;
            }
        }
        std::cout << "\nInstalled " << installed << "/"
                  << bundles.size() << " bundles";
        if (failed > 0) std::cout << " (" << failed << " failed)";
        std::cout << ".\n";
        return failed == 0 ? 0 : 1;
#endif
    }

    if (!watch_mode) return rc;

    WatchOptions opts;
    opts.root = project_root;
    opts.build_dir = build_dir;
    opts.build_args = passthrough_args;
    opts.run_tests = watch_test;
    opts.test_filter = test_filter;
    opts.run_validate = watch_validate;
    return watch_loop(opts);
}
