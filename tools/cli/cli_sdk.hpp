// cli_sdk.hpp — SDK resolution, project-config, and version-banner helpers.
//
// The declarations matching tools/cli/cli_sdk.cpp. Included by cli_common.hpp,
// so command files that already include that header need no change; include
// this directly when SDK/config access is all you need.
#pragma once

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// ── SDK / Config ────────────────────────────────────────────────────────────

fs::path pulp_home();
fs::path sdk_cache_path(const std::string& version);
fs::path local_sdk_cache_path(const std::string& version);
std::string detect_platform();

// SDK tarball filename helpers live in tools/cli/sdk_cache_paths.hpp so
// the matching unit test can link them without the cli_common dep chain.
fs::path ensure_sdk(const std::string& version);
fs::path ensure_checkout_sdk(const fs::path& repo_root, const std::string& version);
int ensure_checkout_dependencies(const fs::path& repo_root);
std::string read_pulp_toml_value(const fs::path& project_root, const std::string& key);
// `read_sdk_version` returns the EFFECTIVE SDK version a build should use.
// If pulp.toml pins an explicit version, that wins. If pulp.toml writes
// `sdk_version = "latest"` (floating mode, the `pulp create` default),
// this returns the newest installed SDK under
// ~/.pulp/sdk/<x.y.z>/, falling back to PULP_SDK_VERSION when none
// are installed. Use `read_raw_sdk_version` for the unresolved value.
std::string read_sdk_version(const fs::path& project_root);
std::string read_raw_sdk_version(const fs::path& project_root);
// True when pulp.toml's sdk_version field is the floating marker
// "latest", or absent entirely. Pinned projects (an explicit x.y.z)
// return false. Used by `pulp upgrade` to skip
// pinned-project SDK auto-update and by `pulp doctor --versions`
// to render the pin status.
bool is_floating_sdk(const fs::path& project_root);
// Return the newest installed SDK version under ~/.pulp/sdk/ — i.e.
// the version `"latest"` resolves to. Empty if no SDKs are installed.
std::string newest_installed_sdk();
// Cached query of "what's the newest SDK available on GitHub Releases?".
// Returns the version string (without the leading `v`) or empty if the cache is
// missing/stale and a fresh query failed. Cached at
// ~/.pulp/cache/latest_release.txt with a 24h TTL; refreshes opportunistically
// when called and the cache is stale, but never blocks the caller for more than
// ~2s (curl timeout).
std::string latest_available_sdk_version();
// Print a one-line banner to stdout if `latest_available_sdk_version()`
// returns a version newer than `installed`. No-op when the cache
// returns nothing, when versions tie, or when installed > available
// (the user is on a pre-release / development build).
void maybe_print_newer_sdk_banner(const std::string& installed);
fs::path read_sdk_path_hint(const fs::path& project_root);
fs::path read_sdk_checkout_hint(const fs::path& project_root);
struct StandaloneSdkResolution {
    std::string requested_version;
    fs::path sdk_path_hint;
    fs::path sdk_checkout_hint;
    fs::path resolved_sdk_dir;
    std::string sdk_path_version;
    bool sdk_path_config_ready = false;
    bool sdk_path_version_known = false;
    bool sdk_path_version_matches = false;
    bool sdk_path_custom_unverifiable = false;
    bool used_sdk_path_hint = false;
    std::string warning;
};
StandaloneSdkResolution resolve_standalone_sdk(const fs::path& project_root,
                                               bool materialize);
bool enforce_project_cli_compatibility(const fs::path& project_root,
                                       const std::string& command_name,
                                       bool allow_unsupported_sdk);

// FetchContent cache preflight. Discovers the shared
// FetchContent source cache for the active project and renders a
// compact remediation message to stderr if any entry is in a `[!!]`
// state. Returns true when the caller may proceed, false when the
// caller should abort with a non-zero exit code.
//
// Honours `PULP_SKIP_CACHE_PREFLIGHT=1` as an opt-out — useful for CI
// environments that can't auto-heal and want to fail at configure
// time anyway. Honours an empty `project_root` (returns true; no
// CMakeLists to read).
bool cache_preflight_check(const fs::path& project_root,
                           const std::string& command_name);
std::string read_user_config_value(const std::string& section, const std::string& key);

struct PrWorkflowSelection {
    std::string workflow;  // shipyard | github | manual
    std::string source;    // cli | env:PULP_PR_WORKFLOW | config:pr.workflow | default
    std::string error;     // non-empty when the selected value is invalid
};

bool is_valid_pr_workflow(const std::string& workflow);
std::string normalize_pr_workflow(std::string workflow);
PrWorkflowSelection resolve_pr_workflow(const std::string& cli_override = {});
std::string read_pinned_shipyard_version(const fs::path& root);
std::string capture_shipyard_version(const std::string& shipyard_bin);

// Write/update `key = "value"` under `[section]` in ~/.pulp/config.toml.
// Creates the file if missing. Preserves all other content verbatim.
// Used by `pulp config set` and by the banner-suppression bookkeeping
// inside cmd_upgrade.
bool write_user_config_value(const std::string& section,
                             const std::string& key,
                             const std::string& value);
std::string read_project_cmake_version(const fs::path& project_root);
