// Shared timeout policy for tests that shell out to the built `pulp` binary.
//
// WHY THIS EXISTS
// Every CLI shell-out suite spawns the CLI and needs a timeout. Left to each
// file, that number drifts: it gets picked to fit whatever the suite did on the
// author's machine, then a suite grows slower work (a codesign pass, a
// grandchild spawn) and the number stays put. The result is a suite that fails
// on a loaded CI runner as a bare `timed_out` with no indication of which test
// or how long it actually took. One shared knob, one policy comment.
//
// WHAT THIS NUMBER IS
// A HANG GUARD, not a performance budget. It exists to stop a wedged child from
// hanging the suite forever — nothing else. It is deliberately far looser than
// any test's expected runtime, and a test getting anywhere near it is a bug
// (a hang), not a slow machine.
//
// HOW THE NUMBER IS CHOSEN
// Two constraints, from below and above.
//
// From below: it must never fire because a machine is busy. The slowest suite
// here spawns codesign once per discovered bundle and runs ~2s unloaded, so this
// default leaves roughly 30x headroom. A tight value converts "this machine is
// loaded" into an opaque `timed_out` inside the child — the failure mode that
// reddens the required gate while telling nobody which subprocess or how long.
// That is exactly how a codesign-heavy suite sitting at 10s used to redden
// `macos` on PRs that could not have caused it.
//
// From above: it must stay clear of the outer `ctest --timeout` (120s in
// build.yml) rather than meeting it. Equal guards race, so neither reliably
// binds and the failure you get depends on scheduling. Staying under the outer
// guard means a genuine hang always fails at this layer, deterministically, with
// a known message — and slowness still never reaches either guard.
//
// WHY IT IS NOT ADAPTIVE
// A self-calibrating or load-scaled timeout makes failures unreproducible (the
// threshold differs per run) and masks real performance regressions (the budget
// grows to fit whatever the code now costs). A fixed generous default plus an
// explicit env override is the design. Do not make this adaptive.
//
// OVERRIDE
// `PULP_TEST_SHELLOUT_TIMEOUT_MS` overrides the default, for bringing up a
// genuinely slower host or for shortening the guard while debugging a hang
// locally. Invalid or non-positive values fall back to the default.

#pragma once

#include <cstdlib>
#include <string>

namespace pulp_test_cli {

// The fixed hang-guard default, in milliseconds. Generous by intent — see the
// header comment before changing it. Lowering it to "make the suite fail faster"
// reintroduces the drift this file removes; raising it past the outer
// `ctest --timeout` makes the two guards race.
inline constexpr int kDefaultShelloutTimeoutMs = 60000;

// The effective shell-out timeout: `PULP_TEST_SHELLOUT_TIMEOUT_MS` when it
// parses to a positive integer, otherwise `kDefaultShelloutTimeoutMs`.
inline int shellout_timeout_ms() {
    const char* env = std::getenv("PULP_TEST_SHELLOUT_TIMEOUT_MS");
    if (env == nullptr || *env == '\0') return kDefaultShelloutTimeoutMs;
    try {
        std::size_t consumed = 0;
        const std::string text(env);
        const int value = std::stoi(text, &consumed);
        // Reject trailing garbage ("30s") and non-positive values rather than
        // silently accepting a partial parse.
        if (consumed != text.size() || value <= 0) return kDefaultShelloutTimeoutMs;
        return value;
    } catch (...) {
        return kDefaultShelloutTimeoutMs;
    }
}

}  // namespace pulp_test_cli
