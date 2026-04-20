// projects_registry.hpp — `~/.pulp/projects.json` project registry
//
// Issue #499 / #552 (Slice 1b): authoritative registry of known Pulp
// projects, populated by `pulp create` on successful scaffold and by
// explicit `pulp projects add/remove` commands. Lives alongside
// version_diag.{hpp,cpp} and is deliberately link-free of cli_common so
// the unit test binary can exercise it standalone.
//
// Design decision (2026-04-21): "hybrid" model. The JSON file is the
// authoritative list; `pulp doctor --versions --scan-parents` is an
// opt-in ancestor walk for projects that were never registered (for
// instance after `git clone` of someone else's example). Ancestor-scan
// results are surfaced inline but NEVER auto-added to the registry.
//
// Stale-entry policy: an entry whose `path` no longer exists on disk
// is flagged in the report but kept. Only explicit `pulp projects
// remove <path>` deletes it. This mirrors the design doc's "never
// silently mutate the user's registry" rule.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace pulp::cli::projects_registry {

namespace fs = std::filesystem;

// One registered project. `path` is the absolute project root. `name`
// is a display hint (usually the directory basename, or the CMake
// project name). `registered_at` is an ISO-8601 UTC timestamp — used
// only for diagnostics.
struct Project {
    fs::path path;
    std::string name;
    std::string registered_at;  // "2026-04-21T14:30:00Z"
};

// Resolve the registry file location. Defaults to
// $PULP_HOME/projects.json (or ~/.pulp/projects.json). Takes an
// optional override for unit tests.
fs::path registry_path(const fs::path& override_home = {});

// Read the registry. Missing or malformed files yield an empty list —
// this is a diagnostic surface, not a critical database, so we never
// throw on corruption. Returns entries in insertion order.
std::vector<Project> read_registry(const fs::path& registry_json);

// Atomically rewrite the registry from the supplied list. Writes to a
// sibling .tmp file then renames. Creates parent directories as
// needed. Returns true on success, false on any filesystem error.
bool write_registry(const fs::path& registry_json,
                    const std::vector<Project>& projects);

// Add a project to the registry. Absolute-paths are canonicalised
// against fs::current_path() if the input is relative. If a project
// with the same canonical path is already present, its `name` and
// `registered_at` are refreshed in place instead of duplicating.
// Returns the resulting list (same behaviour as read + upsert + write).
std::vector<Project> add_project(const fs::path& registry_json,
                                 const fs::path& project_path,
                                 const std::string& project_name);

// Remove a project by path (canonicalised). Returns true if an entry
// was removed, false if no matching entry existed or the write failed.
bool remove_project(const fs::path& registry_json,
                    const fs::path& project_path);

// ISO-8601 UTC timestamp ("YYYY-MM-DDTHH:MM:SSZ"). Wall-clock only —
// the registry is a diagnostic surface, not an audit log.
std::string now_iso8601_utc();

// Walk `start` and its ancestors (up to filesystem root or `stop`,
// whichever comes first) looking for directories that contain a
// `CMakeLists.txt` invoking one of the `pulp_add_*` macros. Returns
// the matches deepest-first (closest ancestor to `start` first). Used
// by `pulp doctor --versions --scan-parents`.
//
// The scan is cheap: it `std::ifstream`-reads each CMakeLists.txt and
// greps for a small regex. It is NOT recursive into sibling subtrees —
// only the direct chain from `start` up to the root. That keeps the
// invariant "no silent disk scans" from the locked-in design decision.
std::vector<fs::path> scan_parent_pulp_projects(const fs::path& start);

}  // namespace pulp::cli::projects_registry
