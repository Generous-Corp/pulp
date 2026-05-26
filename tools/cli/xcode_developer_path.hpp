#pragma once

// Helpers for validating `xcode-select -p` output.
//
// Extracted so the predicate is unit-testable without spawning a real
// `xcode-select` process. `pulp ship auv3-xcodeproj` calls this to refuse
// running when only the command-line tools are selected (the result is a
// .xcodeproj that the real Xcode.app cannot open).
//
// We must NOT hard-match the literal substring `Xcode.app` because
// beta-channel users run `Xcode-beta.app`, `Xcode_15.app`, custom-named
// app bundles, etc. The previous hard match blocked all of them and
// blocked `pulp ship auv3-xcodeproj` for beta users entirely. See
// #2969 / Codex comment 3305628892.

#include <string>

namespace pulp::cli {

/// Return true iff `xcselect_path` looks like a full-Xcode developer dir
/// (something like `<...>/Xcode*.app/Contents/Developer`). Rejects the
/// command-line-tools selection (`/Library/Developer/CommandLineTools`)
/// because that path does not contain an `.app/` bundle component.
inline bool looks_like_full_xcode_developer_dir(const std::string& path) noexcept {
    // Must contain `.app/Contents/Developer` (or close — we accept the
    // exact substring; trailing slash is preserved by xcode-select -p,
    // which doesn't emit one).
    const std::string needle = ".app/Contents/Developer";
    auto pos = path.find(needle);
    if (pos == std::string::npos) return false;
    // The substring before `.app` must end with at least one non-slash
    // character (no `/.app/...` at the start, no empty bundle name).
    return pos > 0 && path[pos - 1] != '/';
}

}  // namespace pulp::cli
