// cli_watch.hpp — File-watch / dev-loop options and entry points.
//
// The declarations for the watch/rebuild loop (implemented in cli_common.cpp).
// Used by `dev`, `build --watch`, and `loop`. Included by cli_common.hpp, so
// command files that already include that header need no change; include this
// directly when only the watch loop is needed.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct WatchOptions {
    fs::path root;
    fs::path build_dir;
    std::vector<std::string> build_args;
    bool run_tests = false;          // run ctest after successful build
    std::string test_filter;          // -R filter for ctest (empty = all)
    bool run_validate = false;        // run quick dlopen validation after build
    std::string launch_target;        // binary to launch/relaunch (empty = none)
    std::vector<std::string> launch_args;  // args for launched binary
    bool hot_dsp = false;             // keep the launched app alive across rebuilds;
                                      // its ReloadableShell watcher hot-swaps the
                                      // rebuilt logic instead of a process restart
    int build_jobs = 0;                // optional host-governed build/test cap
    std::string build_qos;             // optional host role QoS hint
    bool build_watchdog = false;       // true when a lease-backed build should be CPU-watched
};

// Watch source files and rebuild on changes. Optionally runs tests,
// validation, and manages a launched subprocess.
int watch_loop(const WatchOptions& opts);

// Legacy API — calls watch_loop with build-only options
int watch_and_rebuild(const fs::path& root, const fs::path& build_dir,
                      const std::vector<std::string>& build_args);
