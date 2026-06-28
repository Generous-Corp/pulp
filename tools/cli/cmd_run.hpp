// cmd_run.hpp — internal types for `pulp run`.
//
// Exposes a pure parse function so the new --headless / --screenshot /
// --frames / --watch flag plumbing (#914) can be unit-tested without
// shelling out to the CLI binary. The behavioural / shell-out coverage
// lives in test_cli_shellout.cpp.
#pragma once

#include <string>
#include <vector>

namespace pulp_cli {

/// Parsed result of a `pulp run` invocation. Ordering and forwarding
/// rules are encoded in `parse_run_options()` and `assemble_launch_args()`.
struct ParseRunResult {
    bool help = false;                ///< --help / -h was requested
    std::string error;                 ///< Non-empty on parse failure
    std::string target_name;           ///< Optional positional target

    // #914 flags
    bool headless = false;             ///< --headless or implied by --screenshot
    std::string screenshot_path;       ///< --screenshot <path>
    int frames = 1;                    ///< --frames <n>, default 1
    bool watch = false;                ///< --watch

    // Live Audio Inspector discoverability flags.
    bool audio_inspector = false;      ///< --audio-inspector (PULP_AUDIO_INSPECTOR=1)
    std::string audio_probe_json_path; ///< --audio-probe-json <path>; implies --headless
    std::string audio_scope_json_path; ///< --audio-scope-json <path>; implies --headless
    int audio_scope_window = 2048;      ///< --audio-scope-window <samples>
    std::string audio_scope_trigger = "rising-zero"; ///< --audio-scope-trigger <mode>
    int audio_scope_channel = 0;        ///< --audio-scope-channel <index>
    std::string audio_capture_wav_path; ///< --audio-capture-wav <file>; implies --headless
    int audio_capture_frames = 0;       ///< --audio-capture-frames <n>, 0 = ring default
    std::string audio_capture_rolling_path; ///< --audio-capture-rolling <file>; implies --headless
    int audio_capture_rolling_frames = 0;   ///< --audio-capture-rolling-frames <n>, 0 = ring default
    bool audio_capture_rolling_int24 = false; ///< --audio-capture-rolling-format int24 (default float)

    /// Args explicitly forwarded by the user with `-- ...`, plus any
    /// unknown flags (legacy permissive behaviour).
    std::vector<std::string> user_pass_through;
};

/// Parse `pulp run` arguments. Pure — no side effects, no I/O.
ParseRunResult parse_run_options(const std::vector<std::string>& args);

/// Build the argv that gets passed to the launched standalone binary.
/// Order: --headless (if set), --screenshot <path> (if set),
/// --frames <n> (if not default), --audio-inspector (if set),
/// --audio-probe-json <path> (if set), --audio-scope-* (if set), then
/// user_pass_through verbatim.
std::vector<std::string> assemble_launch_args(const ParseRunResult& opts);

}  // namespace pulp_cli
