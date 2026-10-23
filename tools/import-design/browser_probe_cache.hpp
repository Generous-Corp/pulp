#pragma once

// Remembered browser identities, so an import does not launch a second
// Chromium only to rediscover what it learned on the previous run.
//
// Probing a browser runs `--version` and then a headless CDP capability probe,
// which costs seconds per import. Its answer is a function of the files it
// executed. The cache key is the identity of every one of them: the browser
// executable (canonical path, size, modification time), its bundle's
// Info.plist when it has one, the capture script that ran the probe, and the
// Node runtime that ran the script, plus a schema version. Any change to any
// of them is a different key, so a replaced or updated browser is probed
// again. Only compatible answers are stored; a failure is always re-probed.

#include <filesystem>
#include <optional>
#include <string>

namespace pulp::import_design::browser_capture {

struct CachedBrowserIdentity {
    std::string product;
    std::string version;
    int major_version = 0;
    std::string protocol_version;
    std::string build_hash;
};

// nullopt when any input cannot be identified, which disables caching for the
// probe rather than keying it on partial information.
[[nodiscard]] std::optional<std::string>
browser_probe_cache_key(const std::filesystem::path& browser_executable,
                        const std::filesystem::path& capture_script,
                        const std::filesystem::path& node_executable);

[[nodiscard]] std::optional<CachedBrowserIdentity>
load_cached_browser_identity(const std::filesystem::path& cache_directory, const std::string& key);

// Best effort: a cache that cannot be written only costs the next import a
// probe, so a failure here never fails the import.
void store_cached_browser_identity(const std::filesystem::path& cache_directory,
                                   const std::string& key, const CachedBrowserIdentity& identity);

// PULP_HOME/cache/browser-probe, or empty when PULP_DESIGN_BROWSER_PROBE_CACHE
// is "0" or no Pulp home can be resolved.
[[nodiscard]] std::filesystem::path default_browser_probe_cache_directory();

} // namespace pulp::import_design::browser_capture
