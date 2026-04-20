// version_diag.hpp — Version diagnostics for `pulp doctor --versions`
//
// Issue #499 (Slice 1): Surface CLI/SDK/Plugin versions + detect skew
// between the installed CLI and a project's `cli_min_version`. Pure
// logic behind a thin I/O-free core plus render helpers — the core is
// unit-testable without shelling out.
//
// This header deliberately has no dependency on cli_common.hpp so the
// unit-test binary (test_cli_version_diag) can compile just
// version_diag.cpp + the test file without pulling pulp-cli's full
// runtime link surface in.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pulp::cli::version_diag {

namespace fs = std::filesystem;

// Parsed semver triple (major.minor.patch). Only numeric triples are
// treated as comparable; anything with a suffix (e.g. "0.24.0-dev") is
// kept verbatim in the raw string and flagged non-comparable so skew
// detection skips it silently — per the design doc's "untagged builds
// are silently skipped" rule.
struct Semver {
    int major = 0;
    int minor = 0;
    int patch = 0;
    bool comparable = false;   // true iff parsed as pure M.N.P
    std::string raw;           // verbatim string for display
};

Semver parse_semver(const std::string& s);

// -1 if a < b, 0 if a == b, +1 if a > b. Only call when both .comparable.
int compare_semver(const Semver& a, const Semver& b);

// Parse ".claude-plugin/plugin.json" -> version field. Returns empty
// Semver{ .raw = "" } if not found. Uses a tiny regex scan rather than
// full JSON parsing so we don't pull a new dependency in for a
// diagnostic. The plugin manifest is a stable, well-known shape.
Semver read_plugin_version(const fs::path& plugin_json_path);

// Locate the plugin.json the diagnostic should report on, in order:
//   1. explicit override path (if non-empty and exists)
//   2. inside the current repo checkout: <repo>/.claude-plugin/plugin.json
//   3. user-global Claude Code plugin install (best-effort):
//        ~/.claude/plugins/pulp/plugin.json
//        ~/.claude-plugin/pulp/plugin.json
// Returns the first path that exists, or empty if none do. The exact
// installed-plugin path is an open question in the design doc — this
// helper is deliberately forgiving.
fs::path locate_plugin_json(const fs::path& active_repo_root,
                            const fs::path& override_path = {});

// Read `cli_min_version` from a project's pulp.toml. Returns empty
// Semver if absent or unparseable.
Semver read_project_cli_min_version(const fs::path& project_root);

enum class SkewSeverity { Info, Warn };

struct SkewFinding {
    SkewSeverity severity = SkewSeverity::Info;
    std::string message;
};

// A single registered project's version snapshot, used for per-project
// skew lines in `pulp doctor --versions`. Populated by cmd_doctor from
// `~/.pulp/projects.json` (or the `--scan-parents` ancestor walk) and
// fed into VersionReport::analyze() so each project contributes its
// own skew findings. See issue #552 (Slice 1b).
struct ProjectEntry {
    fs::path path;               // canonical project root
    std::string name;            // display name (directory basename or custom)
    Semver sdk;                  // parsed from CMakeLists.txt / pulp.toml
    Semver cli_min;              // parsed from pulp.toml cli_min_version
    bool missing_on_disk = false;  // `path` doesn't exist — warn but keep
    bool scanned = false;        // discovered via --scan-parents, not registered
};

// Inputs to the skew analyzer. All fields optional — the analyzer
// skips checks silently when data is missing (forward-compatible
// behaviour: never hard-fail on missing or untagged versions).
struct VersionReport {
    Semver cli;                  // PULP_SDK_VERSION of the running binary
    Semver plugin;               // .claude-plugin/plugin.json "version"
    Semver project_sdk;          // project's own CMake/pulp.toml sdk_version
    Semver project_cli_min;      // project's pulp.toml cli_min_version
    fs::path project_root;       // for report lines
    fs::path plugin_json_path;   // for report lines

    // Other registered projects (from ~/.pulp/projects.json) plus any
    // ancestor projects surfaced by `--scan-parents`. See issue #552.
    std::vector<ProjectEntry> projects;

    // Analyse and return user-visible findings. Rules (Slice 1 + 1b):
    //   - project_cli_min set AND project_cli_min > cli  -> Warn "upgrade CLI"
    //   - project_sdk set AND project_sdk > cli          -> Warn "CLI behind project SDK"
    //   - for each projects[] entry: same rules, message names the project
    //   - missing-on-disk entries                        -> Warn "path missing"
    //   - otherwise                                      -> Info "compatible"
    std::vector<SkewFinding> analyze() const;
};

// Render the human-readable report body to stdout (Section B layout
// from the design doc). Returns 0 on no-warn, 1 if any Warn-level
// finding is emitted — callers can use this for a non-zero exit code.
int render_report(const VersionReport& report);

// Render the same report as a single JSON object to stdout. The shape
// is stable enough for scripts to parse (see docs/reference/cli.md).
// Always returns 0 — the JSON lane is a pure data surface and mirrors
// the human lane's advisory-only posture. Issue #552.
int render_report_json(const VersionReport& report);

}  // namespace pulp::cli::version_diag
