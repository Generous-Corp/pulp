// fetchcontent_cache.hpp — Discovery + healing for the Pulp FetchContent
// shared-source cache (`~/Library/Caches/Pulp/fetchcontent-src/` on macOS,
// `~/.cache/pulp/fetchcontent-src/` on Linux, `%LOCALAPPDATA%/Pulp/...`
// on Windows). Powers `pulp doctor --caches[ --fix]` and the cache
// preflight inside `pulp build` / `pulp test`.
//
// Mirrors the pattern used by `version_diag` and
// `projects_registry`: pure-logic core with an injected `DiscoveryEnv`
// so unit tests don't need to touch the developer's real cache.
//
// This header deliberately has no dependency on cli_common.hpp so the
// unit-test binary can compile fetchcontent_cache.cpp + the test file
// without pulling pulp-cli's full runtime link surface in.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pulp::cli::fetchcontent_cache {

namespace fs = std::filesystem;

// Health classification for a single entry inside the FetchContent
// shared-source cache root. The status drives the human report and the
// `--fix` decision: only `Dangling` and `StaleCommit` are auto-healed,
// `RootOwned` is reported but never deleted (we don't sudo), and
// `Unknown` covers cases where lstat itself failed (rare, surfaced for
// diagnostics).
enum class CacheStatus {
    Healthy,        // entry exists and (if a symlink) target exists
    Dangling,       // symlink whose target is missing
    StaleCommit,    // entry directory contents predate the declared REF
    RootOwned,      // entry not user-writable; agent must escalate
    Unknown,        // lstat failed or unrepresentable state
};

const char* status_label(CacheStatus s);

// One cache directory entry.
struct CacheEntry {
    std::string name;                // entry basename, e.g. "threejs-077dd13c…"
    fs::path path;                   // absolute path inside the cache root
    CacheStatus status = CacheStatus::Healthy;
    bool is_symlink = false;
    fs::path resolved_target;        // target of a symlink (whether or not it exists)
    std::string declared_ref;        // hash from CMakeLists.txt, when matched
    std::string cached_ref;          // hash baked into the directory name
    std::string dep_name;            // canonical dep name (e.g. "threejs")
    std::string reason;              // short human reason, fits on one line
    std::string remediation;         // one-liner the user can copy/paste
    bool fixable = false;            // true iff `--fix` will act on this entry
};

// Result of the per-entry fix action.
enum class FixOutcome {
    Removed,        // we successfully unlinked or rmtree'd the entry
    Skipped,        // status not in the auto-fix set (RootOwned / Healthy)
    Failed,         // remove failed (permission, EBUSY, etc.)
    DryRun,         // would have removed, but dry_run=true was set
};

const char* fix_outcome_label(FixOutcome o);

struct FixResult {
    fs::path path;
    FixOutcome outcome = FixOutcome::Skipped;
    std::string error;               // populated when outcome == Failed
};

// Stat snapshot used by classification — abstracted so unit tests can
// inject deterministic state without touching the real filesystem.
struct StatInfo {
    bool exists = false;
    bool is_symlink = false;
    bool is_directory = false;
    bool is_user_writable = true;    // false implies likely root-owned
    fs::path symlink_target;         // populated when is_symlink && exists
};

// Mapping from canonical dep name (lowercase, e.g. "threejs") to the
// REF declared via `pulp_register_fetchcontent_source(<dep> REF <hash>)`
// in the parent project's CMakeLists.txt.
using DeclaredRefs = std::map<std::string, std::string>;

// Injected environment for discovery — see `discover_fetchcontent_cache`.
// The defaults populated by `make_real_env()` use the real filesystem;
// tests construct their own with mocked callables.
struct DiscoveryEnv {
    fs::path cache_root;
    DeclaredRefs declared_refs;

    // lstat-equivalent. Returns std::nullopt if the path doesn't exist
    // at all (so the caller can distinguish "missing entry" from
    // "exists but unreadable").
    std::function<std::optional<StatInfo>(const fs::path&)> lstat;

    // stat-equivalent: follows a symlink and reports the target's
    // existence. Used to classify symlink dangling-ness. Returns
    // std::nullopt if the path doesn't exist OR if the underlying
    // syscall fails for any reason.
    std::function<std::optional<StatInfo>(const fs::path&)> stat_follow;

    // Directory listing of `cache_root`. Returns absolute paths to each
    // direct child. Returns empty if cache_root itself doesn't exist.
    std::function<std::vector<fs::path>(const fs::path&)> list_dir;
};

// Construct a DiscoveryEnv that talks to the real filesystem. The
// caller supplies the cache root and the declared refs. Used by the
// CLI; unit tests build their own env with mocked callables.
DiscoveryEnv make_real_env(fs::path cache_root, DeclaredRefs refs);

// Default cache root — mirrors `pulp_default_fetchcontent_cache_root`
// in tools/cmake/PulpFetchContent.cmake. Returns empty if no usable
// directory could be derived (e.g. HOME unset on Unix, both
// LOCALAPPDATA and USERPROFILE unset on Windows).
//
// Honours these overrides, in order:
//   1. PULP_SHARED_FETCHCONTENT_SOURCE_DIR env var (highest priority,
//      same name CMake honours).
//   2. Platform default (~/Library/Caches/Pulp/fetchcontent-src on
//      macOS, $XDG_CACHE_HOME/pulp/fetchcontent-src or
//      ~/.cache/pulp/fetchcontent-src on Linux, %LOCALAPPDATA%/Pulp/...
//      on Windows).
fs::path default_cache_root();

// Parse a CMakeLists.txt file (or any text containing
// `pulp_register_fetchcontent_source(...)` calls) and return the
// declared `name -> REF` map. Names are lowercased to match the
// directory-naming convention. Calls without a REF arg are skipped
// (they don't drive the cache key).
DeclaredRefs parse_declared_refs_from_text(const std::string& text);

// Convenience overload: read the file and parse it. Returns an empty
// map if the file doesn't exist. Used by the CLI; tests use the
// text overload above with inline fixtures.
DeclaredRefs parse_declared_refs_from_file(const fs::path& cmake_file);

// Walk the cache root, classify every direct child entry, and return
// the resulting list. Empty cache root returns an empty vector
// (healthy: nothing to scan). Entries are sorted by name for stable
// reporting.
std::vector<CacheEntry> discover_fetchcontent_cache(const DiscoveryEnv& env);

// Aggregate health: true iff no entry is in a `✗` state (Dangling,
// StaleCommit, or RootOwned). Used to decide the `pulp doctor --caches`
// exit code (any non-Healthy entry warrants reporting + a non-zero exit
// from the diagnostic command).
bool any_unhealthy(const std::vector<CacheEntry>& entries);

// Subset of `any_unhealthy` used by the `pulp build` / `pulp test`
// preflight gate. StaleCommit entries are EXCLUDED from this predicate:
// CMake's override path is keyed on the *current* sanitized ref, so
// leftover `<dep>-<oldref>` directories are normally harmless — configure
// either ignores them or refetches. Hard-failing the build on a stale
// pin would block every developer with an older cache after any
// dependency bump, even though the actual build would succeed. Stale
// entries are still surfaced by `pulp doctor --caches` and cleaned up
// by `--fix`; they just don't gate the build.
//
// This predicate reports true only for states that genuinely break
// configure/build (Dangling symlinks, RootOwned directories CMake can't
// touch, Unknown/lstat-failed entries).
bool blocks_preflight(const std::vector<CacheEntry>& entries);

// Render the human-readable cache report to the given stream. Returns
// 0 if all entries are healthy, 1 otherwise — callers can use this
// directly as the doctor exit code.
int render_report(const std::vector<CacheEntry>& entries,
                  const fs::path& cache_root,
                  std::ostream& out);

// Render the same report in a one-line-per-entry preflight format used
// by `pulp build` / `pulp test`. Only emits output when at least one
// `✗` entry exists; on a healthy cache the function is silent. Returns
// 0 on healthy, 1 on any `✗`.
int render_preflight(const std::vector<CacheEntry>& entries,
                     const fs::path& cache_root,
                     std::ostream& out);

// Render the cache report as a stable JSON object, mirroring the shape
// used by `pulp doctor --versions --json`. Always returns 0 — JSON is
// a pure data surface and never drives an exit code.
int render_report_json(const std::vector<CacheEntry>& entries,
                       const fs::path& cache_root,
                       std::ostream& out);

// Apply fixes to the entries that are marked `fixable`. With
// `dry_run=true`, returns DryRun results without touching the
// filesystem (used by `pulp doctor --caches --fix --dry-run`). Returns
// one FixResult per entry the function considered (i.e. one per
// `entries[i]`), in the same order.
std::vector<FixResult> apply_fixes(const std::vector<CacheEntry>& entries,
                                   bool dry_run);

}  // namespace pulp::cli::fetchcontent_cache
