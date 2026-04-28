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

    /// Args explicitly forwarded by the user with `-- ...`, plus any
    /// unknown flags (legacy permissive behaviour).
    std::vector<std::string> user_pass_through;
};

/// Parse `pulp run` arguments. Pure — no side effects, no I/O.
ParseRunResult parse_run_options(const std::vector<std::string>& args);

/// Build the argv that gets passed to the launched standalone binary.
/// Order: --headless (if set), --screenshot <path> (if set),
/// --frames <n> (if not default), then user_pass_through verbatim.
std::vector<std::string> assemble_launch_args(const ParseRunResult& opts);

}  // namespace pulp_cli
