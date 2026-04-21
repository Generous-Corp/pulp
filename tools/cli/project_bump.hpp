// project_bump.hpp — Release-discovery Slice 7 (#564 / parent #499).
//
// Pure-logic core for `pulp project bump` and `pulp project undo`.
// The CLI frontend lives in `cmd_project.cpp`; this module is the
// testable surface — CMakeLists pin parsing, rewrite, undo-file
// round-trip, and the helpers that decide whether a pin is safe to
// rewrite (branch pins / SHA pins are skipped by design).
//
// Decoupling discipline:
//   - No `cli_common.hpp` include — unit tests link this TU
//     standalone alongside projects_registry.cpp / update_check.cpp
//     (same pattern as Slices 1/2/3/5).
//   - All time enters via caller-supplied ISO timestamps or file
//     names. Never call `std::chrono::system_clock::now()` inside
//     this module beyond the `now_iso8601_utc()` helper exposed to
//     command code; tests pass explicit stamps through.
//
// The bump pin rewrite supports three shapes found in the wild:
//
//   1. FetchContent_Declare(pulp
//          GIT_REPOSITORY https://github.com/danielraffel/pulp.git
//          GIT_TAG v0.23.0)            # ← patched here
//
//   2. pulp_add_project(PulpSynth VERSION 0.23.0 ...)   # helper-style
//
//   3. project(MySynth VERSION 0.23.0 ...)              # template-style
//
// We only rewrite the **first** occurrence of each shape — the
// canonical scaffold never writes more than one. Multiple pins in
// the same file produce a warning in the CLI (Slice 7.1 surface).
//
// Safety rails owned by this module:
//   - refuse_dynamic_pin() — branch names, SHAs, missing pins.
//   - parse_semver_triple()/compare() — rejects invalid --to values
//     and flags downgrades (caller decides whether to honor
//     --allow-downgrade).
//   - undo-file JSON serialize/parse round-trips bump batches so
//     `pulp project undo` can revert them.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pulp::cli::project_bump {

namespace fs = std::filesystem;

// ── Pin site ────────────────────────────────────────────────────────────────

enum class PinKind {
    FetchContentGitTag,   // GIT_TAG v0.23.0 inside FetchContent_Declare(pulp ...)
    PulpAddProject,       // pulp_add_project(NAME VERSION 0.23.0 ...)
    ProjectVersion,       // project(NAME VERSION 0.23.0 ...)
    Unknown,              // nothing identifiable — project will be skipped
};

struct PinSite {
    PinKind kind = PinKind::Unknown;
    std::string current_pin;     // raw text as it appears in CMakeLists (may include leading 'v')
    // Byte offsets into the source text; span covers the pin literal
    // only (not the surrounding call). Used by rewrite_pin() to stay
    // deterministic.
    std::size_t start = 0;
    std::size_t end = 0;
};

// Scan a CMakeLists.txt source string for the first recognizable pin
// site. Scanning order (first-hit wins) is:
//   1. `FetchContent_Declare(pulp ... GIT_TAG <pin>)` — most common
//      external project shape.
//   2. `pulp_add_project(NAME VERSION <pin> ...)` — Pulp helper.
//   3. `project(NAME VERSION <pin> ...)` — template scaffold.
//
// Returns `PinKind::Unknown` with empty fields when nothing is found.
PinSite find_pin_site(const std::string& cmake_source);

// True when the found pin cannot be safely rewritten:
//   - empty pin text
//   - pin is a git branch name (anything that doesn't parse as semver
//     after dropping a leading 'v')
//   - pin is a 40-char-ish SHA (hex-only, 7-40 chars)
//
// Tests cover each branch.
bool refuse_dynamic_pin(const PinSite& site);

// Rewrite the pin literal in `cmake_source` to `new_pin`, preserving
// the rest of the file byte-for-byte. The `site` must have been
// obtained from `find_pin_site(cmake_source)` against the same input.
// Returns the rewritten source on success; nullopt when `site.kind ==
// Unknown` or the byte span no longer matches.
//
// `new_pin_style` preserves the caller's leading-'v' preference: pass
// true when the original pin carried a leading 'v' (FetchContent
// GIT_TAG commonly uses `v0.23.0`), false for the bare semver shape.
std::optional<std::string> rewrite_pin(const std::string& cmake_source,
                                       const PinSite& site,
                                       const std::string& new_pin,
                                       bool new_pin_style_has_v);

// Extract the pure-semver form from a pin literal (strips leading
// 'v'). Returns empty when the pin doesn't look like semver at all.
std::string normalize_pin(const std::string& raw_pin);

// True when `raw_pin` begins with a lowercase 'v' followed by semver.
// Used to preserve the caller's formatting preference across a
// rewrite.
bool pin_has_v_prefix(const std::string& raw_pin);

// Parse a semver string into (M, N, P). Rejects pre-release / build
// suffixes so `--to 0.28.0-rc1` doesn't smuggle in. Returns {0,0,0}
// with ok=false on failure.
struct SemverTriple {
    int major = 0;
    int minor = 0;
    int patch = 0;
    bool ok = false;
};

SemverTriple parse_semver_strict(const std::string& s);

// -1 / 0 / +1. Returns 0 when either side is non-ok.
int compare_semver(const SemverTriple& a, const SemverTriple& b);

// Convenience: true iff `to` is a strictly older version than `from`.
// Used by the downgrade gate. Safe on non-ok inputs (returns false).
bool is_downgrade(const std::string& from, const std::string& to);

// ── Undo batch ──────────────────────────────────────────────────────────────
//
// Every successful `pulp project bump` writes an undo file at
// `<pulp_home>/bump-undo-<timestamp>.json` describing the OLD pin for
// each project in the batch. `pulp project undo` reads the most
// recent file (or a user-specified timestamp) and restores the pins.
//
// Schema (single JSON object, stable shape — tests assert the keys):
//
//   {
//     "timestamp": "2026-04-21T14:30:00Z",
//     "target_version": "0.32.0",
//     "entries": [
//       {
//         "project_path": "/Users/me/code/PulpSynth",
//         "project_name": "PulpSynth",
//         "old_pin":      "0.23.0",
//         "old_pin_style_has_v": true,
//         "pin_kind":     "FetchContentGitTag",
//         "status":       "bumped"
//       },
//       {
//         "project_path": "/Users/me/code/Broken",
//         "project_name": "Broken",
//         "old_pin":      "",
//         "status":       "failed",
//         "failure_reason": "build verification failed"
//       }
//     ]
//   }
//
// The `status` field is one of: "bumped" | "dry_run" | "skipped" |
// "failed". Only `bumped` entries are candidates for undo; the rest
// are carried so the report surfaced to the user matches what the
// undo file shows.

struct UndoEntry {
    fs::path project_path;
    std::string project_name;
    std::string old_pin;             // raw text including optional leading 'v'
    bool old_pin_style_has_v = false;
    PinKind pin_kind = PinKind::Unknown;
    std::string status;              // bumped | dry_run | skipped | failed
    std::string failure_reason;      // only set when status == "failed"
};

struct UndoBatch {
    std::string timestamp;           // ISO-8601 UTC
    std::string target_version;      // version bumped TO (for report text)
    std::vector<UndoEntry> entries;
};

// Serialize / parse — deliberately minimal JSON by hand (same
// philosophy as projects_registry.cpp / update_check.cpp). Unknown
// fields tolerated; malformed input returns nullopt / empty.
std::string serialize_undo_batch(const UndoBatch& batch);
std::optional<UndoBatch> parse_undo_batch(const std::string& json);

bool write_undo_batch(const fs::path& path, const UndoBatch& batch);
std::optional<UndoBatch> read_undo_batch(const fs::path& path);

// List the bump-undo-*.json files under `pulp_home`, newest first
// (sorted by filename — the timestamp component is lexicographically
// monotonic for ISO-8601 Z-suffixed stamps). Missing dir → empty.
std::vector<fs::path> list_undo_batches(const fs::path& pulp_home);

// Compute the undo-batch file path for a given timestamp.
fs::path undo_batch_path(const fs::path& pulp_home, const std::string& timestamp);

// ISO-8601 UTC timestamp (same shape as projects_registry). Re-exposed
// here so callers don't have to link projects_registry just for this.
std::string now_iso8601_utc();

// Pin-kind <-> string for undo-file serialization.
const char* pin_kind_name(PinKind k);
PinKind parse_pin_kind(const std::string& name);

}  // namespace pulp::cli::project_bump
