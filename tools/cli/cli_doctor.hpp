// cli_doctor.hpp — `pulp doctor` check API, shared by doctor + create.
//
// The declarations matching tools/cli/cli_doctor_helpers.cpp. Included by
// cli_common.hpp, so command files that already include that header need no
// change; include this directly when only the doctor probes are needed.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ── Doctor (shared by doctor + create) ──────────────────────────────────────

struct DoctorCheck {
    std::string name;
    bool passed;
    std::string detail;
    std::string fix;
    // Optional checks report remediation advice but don't contribute
    // to the overall doctor exit code when they fail. Used for e.g.
    // the Google Android CLI accelerator, which is a speedup,
    // not a requirement.
    bool optional = false;
    // Release-only checks describe a capability needed to PUBLISH, not to
    // develop. `pulp doctor` still gates on them — a missing RELEASE_BOT_TOKEN
    // once cost a 22-hour release drought, so that signal must stay loud — but
    // commands that merely build or scaffold must not inherit it. Without this
    // distinction `pulp create` refused to scaffold a project on any machine
    // lacking a release credential, including every contributor's, since a repo
    // secret is not something they can set.
    bool release_only = false;
    // Checkout-health checks describe the state of an existing git
    // checkout rather than a capability needed to build or scaffold.
    // `pulp doctor` gates on them — a write-dead checkout must be a
    // loud signal — but `pulp create` must not inherit it: a stale lock
    // in some unrelated sibling worktree has no bearing on whether a
    // new project can be scaffolded, and gating there would repeat the
    // release_only mistake above.
    bool checkout_health = false;
};

// Whether a failing check should stop `pulp create` from scaffolding.
// The decision is a meaningful intermediate in its own right, so it is
// reachable and testable here rather than living inside the command's
// loop: `optional`, `release_only`, and `checkout_health` rows all
// surface their advice without gating, and only a plain failing check
// blocks. `pulp doctor` gates on all of them except `optional`.
inline bool doctor_check_gates_project_creation(const DoctorCheck& check) {
    if (check.passed) return false;
    return !check.optional && !check.release_only && !check.checkout_health;
}

// `only_filter`: case-insensitive substring. When non-empty,
// individual probes whose name doesn't match are SKIPPED — no process
// spawn, no file IO — so `pulp doctor --only git` runs only git probes
// instead of running everything and filtering the output.
std::vector<DoctorCheck> run_doctor_checks(const fs::path& active_root, bool standalone_mode,
                                           const std::string& only_filter = {});

// `pulp doctor android` — Android NDK / SDK / emulator checks plus
// optional Google "Android CLI" detection. Passes the host platform
// implicitly via #ifdef in the implementation.
std::vector<DoctorCheck> run_doctor_android_checks(const std::string& only_filter = {});

// `pulp doctor ios` — Xcode + iOS Simulator checks. macOS-only;
// returns a single explanatory entry on other hosts so users still
// see a useful message.
std::vector<DoctorCheck> run_doctor_ios_checks(const std::string& only_filter = {});

// Helper: case-insensitive substring match used by all three run_doctor_*
// functions to short-circuit probes. Empty filter = always-run.
bool doctor_check_matches_only_filter(const std::string& only_filter,
                                      const std::string& check_name);
