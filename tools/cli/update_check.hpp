// update_check.hpp — Release-discovery Slice 2 (#547 / parent #499).
//
// Pure-logic core for the 24h "is there a newer Pulp CLI release?" cache.
// Anonymous GitHub Releases API + 24h cache in ~/.pulp/update-cache.json.
// No auth, no fallback — the 60 req/hour anonymous rate limit is fine
// because the cache interval is 24h by default.
//
// This header is deliberately decoupled from cli_common.hpp so the unit
// tests can link update_check.cpp standalone (same pattern as
// version_diag — see #499 Slice 1).
//
// Public surface:
//   - CacheEntry / parse_cache_json / serialize_cache_json  (JSON I/O)
//   - is_cache_stale                                        (age check)
//   - compose_banner                                        (prompt-mode text)
//   - write_toml_key_in_section                             (~/.pulp/config.toml writer)
//   - Fetcher (abstract) + GitHubReleasesFetcher (real HTTP via curl/pwsh)
//   - refresh_cache                                         (Fetcher orchestrator)
//
// Network calls are isolated to GitHubReleasesFetcher. Tests inject a
// FakeFetcher and never touch the network. See test/test_cli_update_check.cpp.

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace pulp::cli::update_check {

namespace fs = std::filesystem;

// ── Schema ──────────────────────────────────────────────────────────────────

// Cache schema version. Bump when adding required fields; the reader
// tolerates higher versions by ignoring unknown fields rather than
// failing, and tolerates lower versions by treating missing fields as
// defaults.
inline constexpr int kCacheSchemaVersion = 1;

struct CacheEntry {
    int schema = kCacheSchemaVersion;

    // Seconds since Unix epoch at which the last check completed. A
    // value of 0 means "never checked".
    std::int64_t last_check_epoch_sec = 0;

    // Normalized semver triple, e.g. "0.27.0". Empty if the last check
    // failed and we have no prior known latest.
    std::string latest_version;

    // HTML URL of the release (user-facing link in the banner).
    std::string release_notes_url;

    // Tracks which version we've already shown a banner for, so `prompt`
    // mode doesn't pester the user on every invocation. If this matches
    // latest_version, the banner is suppressed.
    std::string banner_shown_for_version;
};

// ── JSON I/O (minimal, handcrafted) ─────────────────────────────────────────

// Deserialize the cache-file JSON. Returns CacheEntry{} on parse error
// (fresh cache), not an exception. The shape we accept:
//
//   {
//     "schema": 1,
//     "last_check_epoch_sec": 1713638400,
//     "latest_version": "0.28.0",
//     "release_notes_url": "https://github.com/.../releases/tag/v0.28.0",
//     "banner_shown_for_version": "0.27.0"
//   }
//
// Unknown fields are ignored. Missing fields default per CacheEntry.
CacheEntry parse_cache_json(const std::string& json);

// Serialize to a human-readable, stable JSON shape. Writes are never
// streamed — callers buffer then fs::rename to make the swap atomic on
// POSIX (Windows writes directly; a torn write would just force a
// re-fetch on the next invocation, not corrupt behaviour).
std::string serialize_cache_json(const CacheEntry& entry);

// Best-effort read of ~/.pulp/update-cache.json. Returns nullopt only
// when the file does not exist; a malformed file returns CacheEntry{}.
std::optional<CacheEntry> read_cache_file(const fs::path& cache_path);

// Atomically write the cache file. Ensures parent dir exists.
// Returns true on success.
bool write_cache_file(const fs::path& cache_path, const CacheEntry& entry);

// ── Age Check ───────────────────────────────────────────────────────────────

// True if the cache's last_check_epoch_sec is older than interval_hours
// behind now. Also true when last_check_epoch_sec == 0 (never checked)
// or negative clock skew (now < last_check). interval_hours <= 0 means
// "never stale" — treat check as disabled.
bool is_cache_stale(const CacheEntry& cache,
                    std::int64_t now_epoch_sec,
                    int interval_hours);

// Convenience: epoch-seconds from std::chrono now.
std::int64_t now_epoch_sec();

// ── Semver comparison (tolerant) ────────────────────────────────────────────

// Parse a version string into (M, N, P). Accepts optional leading 'v'.
// Non-semver strings yield {0,0,0} and false. Extra dotted components
// and pre-release suffixes are tolerated and ignored for comparison.
struct SemverTriple {
    int major = 0;
    int minor = 0;
    int patch = 0;
    bool ok = false;
};
SemverTriple parse_semver(const std::string& s);

// -1, 0, +1. Returns 0 if either side is non-ok.
int compare_semver(const SemverTriple& a, const SemverTriple& b);

// True if `latest` is a strictly newer release than `installed`.
// Safe on unparseable inputs (returns false).
bool is_newer(const std::string& installed, const std::string& latest);

// ── Banner ──────────────────────────────────────────────────────────────────

// Exact single-line banner emitted in `prompt` mode. Printed to stderr
// (not stdout) so it never corrupts piped/scripted output. The caller
// decides the stream; this function only composes the string.
//
// Shape (see design Section A and #547):
//   Pulp vX.Y.Z available (you have vA.B.C). Run `pulp upgrade` or
//   `pulp config set update.mode manual` to silence.
std::string compose_banner(const std::string& installed_version,
                           const std::string& latest_version);

// ── TOML Key Writer ─────────────────────────────────────────────────────────

// Rewrite or insert `key = "value"` under `[section]` in the given TOML
// source. Preserves all other content verbatim. If the section is
// missing, it is appended at the end with a leading blank line. If the
// key is missing inside an existing section, it's appended at the end
// of that section. If the key is present, its value is replaced
// in-place.
//
// This is deliberately minimal — it only needs to round-trip the
// ~/.pulp/config.toml surfaces we own. It does NOT support arrays,
// tables-of-tables, or multi-line strings.
std::string write_toml_key_in_section(const std::string& source,
                                      const std::string& section,
                                      const std::string& key,
                                      const std::string& value);

// Convenience reader — same parser as cli_common::read_user_config_value
// (commented-code-tolerant: strips first-`#` per line, requires
// [section] boundary, supports "key = \"value\"" and bare "key = value").
// Extracted here so both the unit test and cli_common can share logic.
std::string read_toml_key_in_section(const std::string& source,
                                     const std::string& section,
                                     const std::string& key);

// ── Fetcher ─────────────────────────────────────────────────────────────────

// Result of a single GitHub releases/latest query. `ok=false` means the
// fetch failed or returned something we couldn't parse. Callers should
// leave the existing cache untouched on failure rather than clobbering
// a known-good latest_version.
struct FetchResult {
    bool ok = false;
    std::string latest_version;       // normalized without leading 'v'
    std::string release_notes_url;    // html_url of the release
    std::string error;                // set when !ok
};

// Abstract fetcher so tests never hit the network.
struct Fetcher {
    virtual ~Fetcher() = default;
    virtual FetchResult fetch_latest_release(const std::string& owner_repo) = 0;
};

// Real fetcher — shells out to curl on macOS/Linux and
// Invoke-WebRequest on Windows. Parses tag_name + html_url from the
// JSON body with a regex scan (same strategy as version_diag's
// plugin.json reader — no JSON dep).
struct GitHubReleasesFetcher : Fetcher {
    FetchResult fetch_latest_release(const std::string& owner_repo) override;
};

// Refresh the cache using the given fetcher. Updates last_check_epoch_sec
// regardless of fetch outcome so a 24h network outage doesn't mean we
// hammer the API on every invocation. Returns the updated CacheEntry.
// The previous cache is passed in and carried forward on fetch failure.
CacheEntry refresh_cache(Fetcher& fetcher,
                         const CacheEntry& previous,
                         const std::string& owner_repo,
                         std::int64_t now_epoch_sec);

}  // namespace pulp::cli::update_check
