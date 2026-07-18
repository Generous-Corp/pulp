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
};

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
