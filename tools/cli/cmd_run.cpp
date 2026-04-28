// cmd_run.cpp — pulp run command
//
// Launches a project's standalone binary. In addition to the basic launch
// path, supports headless / screenshot / frames / watch flags so any Pulp
// plugin's standalone can be auto-validated in CI without a real window.
//
// Issue #914: surface --headless / --screenshot / --frames / --watch.
// The standalone-side wiring (HeadlessHost, validation_harness,
// pulp-screenshot) is already in place — this file plumbs the flags
// through to the launched binary via both forwarded args AND env vars,
// so binaries that read either source pick up the headless mode.

#include "cli_common.hpp"
#include "cmd_run.hpp"

#include <cstdlib>
#include <iostream>

namespace {

constexpr const char* kHeadlessEnv   = "PULP_HEADLESS";
constexpr const char* kScreenshotEnv = "PULP_SCREENSHOT";
constexpr const char* kFramesEnv     = "PULP_FRAMES";

void print_help() {
    std::cout
        << "pulp run — launch a standalone Pulp application\n\n"
           "Usage: pulp run [target] [--headless] [--screenshot <file>] [--frames <n>] [--watch] [-- args...]\n\n"
           "If no target is specified, finds the first standalone binary in the\n"
           "active project build. Arguments after `--` are passed to the launched\n"
           "application.\n\n"
           "Options:\n"
           "  --headless              Run without a window; render offscreen.\n"
           "                          (Forwarded as --headless and PULP_HEADLESS=1.)\n"
           "  --screenshot <file>     Save a PNG screenshot to <file> after rendering.\n"
           "                          (Forwarded as --screenshot <file> and\n"
           "                          PULP_SCREENSHOT=<file>. Implies --headless.)\n"
           "  --frames <n>            Number of frames to render before screenshot.\n"
           "                          Default 1. (Forwarded as --frames <n> and\n"
           "                          PULP_FRAMES=<n>.)\n"
           "  --watch                 Re-launch the binary on source changes.\n"
           "                          Composes with --headless / --screenshot.\n"
           "  -h, --help              Show this help and exit.\n\n"
           "Examples:\n"
           "  pulp run                                # launch first standalone\n"
           "  pulp run pulp-gain                       # launch a specific target\n"
           "  pulp run --headless --screenshot ui.png  # CI-friendly headless render\n"
           "  pulp run --watch                         # re-launch on file change\n";
}

// Set or clear an env var portably. value="" clears it.
void set_env(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) {
        ::unsetenv(name);
    } else {
        ::setenv(name, value.c_str(), 1);
    }
#endif
}

}  // namespace

// Parser is defined in cmd_run_parse.cpp so it stays free of cli_common.

int cmd_run(const std::vector<std::string>& args) {
    using namespace pulp_cli;

    auto opts = parse_run_options(args);
    if (opts.help) {
        print_help();
        return 0;
    }
    if (!opts.error.empty()) {
        std::cerr << "Error: " << opts.error << "\n";
        return 2;
    }

    bool standalone_mode = false;
    auto root = resolve_active_project_root(&standalone_mode);
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto build_dir = root / "build";
    if (!fs::exists(build_dir / "CMakeCache.txt")) {
        std::cerr << "Error: project not built yet. Run `pulp build` first.\n";
        return 1;
    }

    // If --headless was requested but no screenshot path given, pick a
    // sensible default so the simplest "pulp run --headless" still does
    // something useful in CI.
    if (opts.headless && opts.screenshot_path.empty()) {
        std::string base = opts.target_name.empty() ? "pulp-run" : opts.target_name;
        opts.screenshot_path = (build_dir / (base + ".png")).string();
    }

    // Find standalone binary
    fs::path binary;
    auto app_search_root = standalone_mode ? (build_dir / "bin") : (build_dir / "examples");

#ifdef __APPLE__
    auto find_app_bundle = [&](const fs::path& search_dir) -> fs::path {
        if (!fs::exists(search_dir)) return {};
        for (auto& entry : fs::directory_iterator(search_dir)) {
            if (!entry.is_directory()) continue;
            auto name = entry.path().filename().string();
            if (name.size() > 4 && name.substr(name.size() - 4) == ".app") {
                auto macos_dir = entry.path() / "Contents" / "MacOS";
                if (fs::exists(macos_dir)) {
                    for (auto& exec_entry : fs::directory_iterator(macos_dir)) {
                        if (!exec_entry.is_regular_file()) continue;
                        auto st = fs::status(exec_entry.path());
                        if ((st.permissions() & fs::perms::owner_exec) != fs::perms::none) {
                            return exec_entry.path();
                        }
                    }
                }
            }
        }
        return {};
    };
#endif

    if (!opts.target_name.empty()) {
        if (standalone_mode) {
            if (fs::exists(app_search_root)) {
                for (auto& file : fs::directory_iterator(app_search_root)) {
                    if (!file.is_regular_file()) continue;
                    auto fname = file.path().filename().string();
                    if (fname.find("-test") != std::string::npos) continue;
                    if (fname == opts.target_name || file.path().stem().string() == opts.target_name) {
                        auto st = fs::status(file.path());
                        if ((st.permissions() & fs::perms::owner_exec) != fs::perms::none) {
                            binary = file.path();
                            break;
                        }
                    }
                }
            }
        } else if (fs::exists(app_search_root)) {
            for (auto& dir_entry : fs::directory_iterator(app_search_root)) {
                if (!dir_entry.is_directory()) continue;
                for (auto& file : fs::directory_iterator(dir_entry.path())) {
                    if (!file.is_regular_file()) continue;
                    auto fname = file.path().filename().string();
                    if (fname.find("-test") != std::string::npos) continue;
                    if (fname == opts.target_name || file.path().stem().string() == opts.target_name) {
                        auto st = fs::status(file.path());
                        if ((st.permissions() & fs::perms::owner_exec) != fs::perms::none) {
                            binary = file.path();
                            break;
                        }
                    }
                }
                if (!binary.empty()) break;
            }
        }
        if (binary.empty()) {
            std::cerr << "Error: could not find standalone binary '" << opts.target_name
                      << "' in " << app_search_root.string() << "\n";
            std::cerr << "  Run `pulp build` to build, then try again.\n";
            return 1;
        }
    } else {
        if (standalone_mode) {
            if (fs::exists(app_search_root)) {
                for (auto& file : fs::directory_iterator(app_search_root)) {
                    if (!file.is_regular_file()) continue;
                    auto fname = file.path().filename().string();
                    if (fname.find("-test") != std::string::npos) continue;
                    if (fname.find("cmake") != std::string::npos) continue;
                    if (fname.find(".") != std::string::npos) continue;
                    auto st = fs::status(file.path());
                    if ((st.permissions() & fs::perms::owner_exec) != fs::perms::none) {
                        binary = file.path();
                        break;
                    }
                }
            }
        } else if (fs::exists(app_search_root)) {
            for (auto& dir_entry : fs::directory_iterator(app_search_root)) {
                if (!dir_entry.is_directory()) continue;
                for (auto& file : fs::directory_iterator(dir_entry.path())) {
                    if (!file.is_regular_file()) continue;
                    auto fname = file.path().filename().string();
                    if (fname.find("-test") != std::string::npos) continue;
                    if (fname.find("cmake") != std::string::npos) continue;
                    if (fname.find(".") != std::string::npos) continue;
                    auto st = fs::status(file.path());
                    if ((st.permissions() & fs::perms::owner_exec) != fs::perms::none) {
                        binary = file.path();
                        break;
                    }
                }
                if (!binary.empty()) break;
            }
        }
#ifdef __APPLE__
        if (binary.empty()) {
            binary = find_app_bundle(build_dir);
        }
        if (binary.empty()) {
            binary = find_app_bundle(app_search_root);
        }
#endif
        if (binary.empty()) {
            std::cerr << "Error: no standalone binary found in " << app_search_root.string() << "\n";
            std::cerr << "  Create one with: pulp create MyApp --type app\n";
            std::cerr << "  Or build an existing project: pulp build\n";
            return 1;
        }
    }

    // Set env vars so binaries that prefer env over argv (or that can't
    // be modified to parse new flags) still pick up the headless mode.
    if (opts.headless) set_env(kHeadlessEnv, "1");
    if (!opts.screenshot_path.empty()) set_env(kScreenshotEnv, opts.screenshot_path);
    if (opts.frames != 1) set_env(kFramesEnv, std::to_string(opts.frames));

    auto launch_args = assemble_launch_args(opts);

    if (opts.watch) {
        WatchOptions wopts;
        wopts.root = root;
        wopts.build_dir = build_dir;
        wopts.launch_target = binary.string();
        wopts.launch_args = launch_args;
        std::cout << "Launching " << binary.filename().string()
                  << " (watch mode)...\n";
        return watch_loop(wopts);
    }

    std::cout << "Launching " << binary.filename().string();
    if (opts.headless) std::cout << " (headless)";
    if (!opts.screenshot_path.empty())
        std::cout << " → " << opts.screenshot_path;
    std::cout << "\n";

    std::string cmd = shell_quote(binary);
    for (auto& arg : launch_args) {
        cmd += " " + shell_quote(arg);
    }

    return run(cmd);
}
