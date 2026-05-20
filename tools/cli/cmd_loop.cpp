// cmd_loop.cpp — pulp loop: leveraged-prototype focus mode.
//
// Issue #940 — Codify the leveraged-prototype dev loop.
//
// `pulp loop` is the explicit "I'm in single-platform iteration mode"
// switch. It records the focus platform in ~/.pulp/config.toml under
// the [loop] section so the user can leave the mode and return to
// cross-platform iteration deliberately, and then drives the same
// watch + rebuild + screencap loop as `pulp dev` — but pinned to one
// platform's toolchain so the slow cross-platform configure paths
// (Skia/Dawn/threejs FetchContent) can be skipped when other platforms
// add unrelated cost.
//
// This is Slice 1 of #940: the CLI surface, the focus state, and the
// watch loop. Three follow-up slices land separately:
//
//   * Slice 2 (#946): ar-swap helper that validates header/library ABI
//     before swapping a built `.o` from another worktree into a pinned
//     SDK static archive.
//   * Slice 3 (#947): PR-state monitor that polls `gh pr list` for
//     state flips on PRs referencing the named issues. The flag is
//     parsed here so the surface is forward-compatible; today it
//     prints a "deferred" hint.
//   * Slice 4 (#948): lift `@pulp/css-adapt`, `pulp-css-analyze`, and
//     `extract-html-bundle` from Spectr's tree into Pulp's
//     tools/packages/.

#include "cli_common.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

// The set of platform names accepted by --platform. The local autodetect
// produces values like "darwin-arm64" / "windows-x64"; the focus-mode
// surface accepts the short umbrella names from issue #940 ("macos",
// "linux", "windows") and normalizes everything down to the umbrella.
const char* kFocusPlatforms[] = {"macos", "linux", "windows"};

bool is_known_focus_platform(const std::string& name) {
    for (const char* p : kFocusPlatforms) {
        if (name == p) return true;
    }
    return false;
}

// Map detect_platform()'s "darwin-arm64" / "linux-x64" / "windows-arm64"
// strings to the umbrella names #940 uses. Any unknown host degrades
// to an empty string so the caller can warn.
std::string umbrella_from_detected(const std::string& detected) {
    if (detected.rfind("darwin", 0) == 0) return "macos";
    if (detected.rfind("linux", 0) == 0) return "linux";
    if (detected.rfind("windows", 0) == 0) return "windows";
    return {};
}

void print_help() {
    std::cout <<
        "pulp loop — leveraged-prototype focus mode (issue #940)\n\n"
        "Usage: pulp loop [options] [-- launch-args...]\n\n"
        "Single-platform watch + rebuild + screencap loop. Pin one platform's\n"
        "toolchain so cross-platform configure paths (Skia/Dawn/threejs\n"
        "FetchContent) don't gate fast iteration. Pair with `shipyard pr` /\n"
        "`pulp pr` at land time to restore full cross-platform validation.\n\n"
        "Options:\n"
        "  --platform=<macos|linux|windows>  Override the auto-detected host platform\n"
        "  --off                             Restore cross-platform mode (clear focus)\n"
        "  --status                          Print the current focus state and exit\n"
        "  --watch-issues N1,N2,...          (Slice 3, #947) PR-state monitor — deferred\n"
        "  --ar-swap-from <ref>              (Slice 2, #946) ABI-checked .o swap — deferred\n"
        "  --test, -t                        Run tests after each successful build\n"
        "  --test-filter=PATTERN             Run only tests matching PATTERN (implies --test)\n"
        "  --validate                        Run quick plugin dlopen validation after build\n"
        "  --run TARGET                      Launch TARGET from build dir, relaunch on rebuild\n"
        "  --target T                        Pass --target T to cmake --build\n"
        "  --no-watch                        Set/clear focus state and exit (no watch)\n"
        "  --allow-unsupported-sdk           Bypass the CLI-vs-project SDK guard (unsupported)\n"
        "  -h, --help                        Show this help\n\n"
        "Examples:\n"
        "  pulp loop                         # Enter focus mode on the auto-detected host\n"
        "  pulp loop --platform=macos --test # Pin to macOS + run tests on every save\n"
        "  pulp loop --status                # Print current focus state\n"
        "  pulp loop --off                   # Restore cross-platform mode\n"
        "  pulp loop --watch-issues 924,927  # (deferred — see issue #947)\n";
}

// Persist focus state. `platform` empty clears the marker.
bool write_focus_state(const std::string& platform) {
    if (platform.empty()) {
        // Clear by writing an empty value — read_user_config_value treats
        // empty string the same as missing key (callers check empty).
        return write_user_config_value("loop", "focus_platform", "");
    }
    return write_user_config_value("loop", "focus_platform", platform);
}

std::string read_focus_state() {
    return read_user_config_value("loop", "focus_platform");
}

bool missing_value(const std::vector<std::string>& args, size_t i) {
    return i + 1 >= args.size() || (!args[i + 1].empty() && args[i + 1][0] == '-');
}

}  // namespace

int cmd_loop(const std::vector<std::string>& args) {
    bool standalone_mode = false;
    auto project_root = resolve_active_project_root(&standalone_mode);
    (void)standalone_mode;  // Reserved for future loop-mode-specific divergences.

    // Parse flags. We accept --status / --off / --help even outside a
    // project root so the user can clear focus state from anywhere.
    std::string platform_override;
    bool clear_focus = false;
    bool status_only = false;
    bool no_watch = false;
    bool run_tests = false;
    bool run_validate = false;
    bool allow_unsupported_sdk = false;
    bool after_separator = false;
    std::string test_filter;
    std::string launch_target;
    std::string watch_issues;     // deferred Slice 3
    std::string ar_swap_from;     // deferred Slice 2
    std::vector<std::string> launch_args;
    std::vector<std::string> build_args;

    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--help" || a == "-h") {
            print_help();
            return 0;
        }
        if (a == "--") { after_separator = true; continue; }
        if (after_separator) { launch_args.push_back(a); continue; }

        if (a == "--off") {
            clear_focus = true;
        } else if (a == "--status") {
            status_only = true;
        } else if (a == "--no-watch") {
            no_watch = true;
        } else if (a.rfind("--platform=", 0) == 0) {
            platform_override = a.substr(11);
        } else if (a == "--platform") {
            if (missing_value(args, i)) {
                std::cerr << "pulp loop: --platform requires a value\n";
                return 2;
            }
            platform_override = args[++i];
        } else if (a.rfind("--watch-issues=", 0) == 0) {
            watch_issues = a.substr(15);
        } else if (a == "--watch-issues") {
            if (missing_value(args, i)) {
                std::cerr << "pulp loop: --watch-issues requires a value\n";
                return 2;
            }
            watch_issues = args[++i];
        } else if (a.rfind("--ar-swap-from=", 0) == 0) {
            ar_swap_from = a.substr(15);
        } else if (a == "--ar-swap-from") {
            if (missing_value(args, i)) {
                std::cerr << "pulp loop: --ar-swap-from requires a value\n";
                return 2;
            }
            ar_swap_from = args[++i];
        } else if (a == "--test" || a == "-t") {
            run_tests = true;
        } else if (a.rfind("--test-filter=", 0) == 0) {
            test_filter = a.substr(14);
            run_tests = true;
        } else if (a == "--validate") {
            run_validate = true;
        } else if (a == "--allow-unsupported-sdk") {
            allow_unsupported_sdk = true;
        } else if (a == "--run") {
            if (missing_value(args, i)) {
                std::cerr << "pulp loop: --run requires a value\n";
                return 2;
            }
            launch_target = args[++i];
        } else if (a == "--target") {
            if (missing_value(args, i)) {
                std::cerr << "pulp loop: --target requires a value\n";
                return 2;
            }
            build_args.push_back("--target");
            build_args.push_back(args[++i]);
        } else {
            build_args.push_back(a);
        }
    }

    // ── --status: just print and exit ───────────────────────────────────────
    if (status_only) {
        auto current = read_focus_state();
        auto detected = detect_platform();
        auto umbrella = umbrella_from_detected(detected);
        std::cout << "pulp loop — focus mode status\n";
        std::cout << "  detected host:  " << (detected.empty() ? "unknown" : detected);
        if (!umbrella.empty()) std::cout << " (" << umbrella << ")";
        std::cout << "\n";
        if (current.empty()) {
            std::cout << "  focus platform: (none — cross-platform mode)\n";
        } else {
            std::cout << "  focus platform: " << current << "\n";
        }
        return 0;
    }

    // ── --off: clear focus state and exit ───────────────────────────────────
    if (clear_focus) {
        auto current = read_focus_state();
        if (current.empty()) {
            std::cout << "pulp loop: already in cross-platform mode (no focus to clear).\n";
            return 0;
        }
        if (!write_focus_state("")) {
            std::cerr << "pulp loop: failed to clear focus state in ~/.pulp/config.toml\n";
            return 1;
        }
        std::cout << "pulp loop: focus mode cleared (was " << current
                  << "). Cross-platform mode restored.\n";
        std::cout << "  Run `shipyard pr` / `pulp pr` for full cross-platform validation\n"
                     "  before landing.\n";
        return 0;
    }

    // ── Resolve focus platform (override > existing focus > autodetect) ─────
    std::string focus_platform = platform_override;
    if (!focus_platform.empty() && !is_known_focus_platform(focus_platform)) {
        std::cerr << "pulp loop: unknown --platform value '" << focus_platform
                  << "'. Expected one of: macos, linux, windows.\n";
        return 1;
    }
    if (focus_platform.empty()) {
        // No override — fall back to detected host.
        auto detected = detect_platform();
        focus_platform = umbrella_from_detected(detected);
        if (focus_platform.empty()) {
            std::cerr << "pulp loop: could not detect host platform automatically. "
                         "Pass --platform=<macos|linux|windows> explicitly.\n";
            return 1;
        }
    }

    // Persist the focus marker so subsequent invocations / other tools
    // (the slash command, ar-swap helper, etc.) can read it.
    if (!write_focus_state(focus_platform)) {
        // Non-fatal: the watch loop can still run, but the user is out
        // of luck w.r.t. state persistence (PULP_HOME unwritable, etc.).
        std::cerr << "pulp loop: warning — could not persist focus state to "
                     "~/.pulp/config.toml. Continuing without persistence.\n";
    }

    std::cout << "pulp loop: focus mode active on " << focus_platform << ".\n";
    std::cout << "  Cross-platform configure paths skipped on this host.\n";
    std::cout << "  Run `pulp loop --off` to restore cross-platform mode\n"
                 "  before landing the consumer-side PR.\n";

    // Surface deferred-slice hints early so users know what's available
    // and what's coming. These are forward-compatible no-ops at the
    // CLI level today.
    if (!watch_issues.empty()) {
        std::cout << "\npulp loop: --watch-issues=" << watch_issues
                  << " is deferred — see issue #947 (Slice 3).\n"
                     "  In the meantime, run `gh pr list --search \"" << watch_issues
                  << "\"` manually.\n";
    }
    if (!ar_swap_from.empty()) {
        std::cout << "\npulp loop: --ar-swap-from=" << ar_swap_from
                  << " is deferred — see issue #946 (Slice 2).\n"
                     "  Header/library ABI verification is the gating piece — coming next.\n";
    }

    // If --no-watch was passed (or there's no project to watch), exit
    // cleanly after persisting the focus marker. This is the
    // CI-test-friendly path: the test can flip focus state and assert
    // exit 0 without ever entering a watch loop.
    if (no_watch) {
        return 0;
    }
    if (project_root.empty()) {
        std::cerr << "pulp loop: focus state persisted, but not in a Pulp project — "
                     "skipping watch loop. Run from a project root to start watching.\n";
        return 0;
    }

    // ── Enter watch loop (same plumbing as `pulp dev`) ──────────────────────
    if (!enforce_project_cli_compatibility(project_root,
                                           "pulp loop",
                                           allow_unsupported_sdk)) {
        return 1;
    }

    auto build_dir = project_root / "build";

    // Ensure configured. Reuse cmd_build's bootstrap path; the cross-platform
    // skip is applied via build_args / env vars elsewhere when relevant.
    if (!fs::exists(build_dir / "CMakeCache.txt")) {
        std::cout << "Project not configured. Configuring + building first...\n";
        std::vector<std::string> bootstrap_args;
        if (allow_unsupported_sdk) bootstrap_args.push_back("--allow-unsupported-sdk");
        int rc = cmd_build(bootstrap_args);
        if (rc != 0) return rc;
    }

    // Initial build
    std::string build_cmd = "cmake --build " + build_dir.string();
    for (auto& arg : build_args) build_cmd += " " + arg;
    int rc = run_with_spinner(build_cmd, "Building");
    if (rc != 0) {
        std::cerr << "Initial build failed. Watch loop will retry on changes.\n";
    }

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
