// SPDX-License-Identifier: MIT
#pragma once

#include "package_registry_trust.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pulp::cli::pkg {

namespace fs = std::filesystem;

// ── Platform Targets ──

struct PlatformTarget {
    std::string platform;  // "macOS", "Windows", "Linux", "iOS", "WASM"
    std::string arch;      // "arm64", "x64", "wasm32"

    std::string to_string() const { return platform + "-" + arch; }
    bool operator==(const PlatformTarget& o) const {
        return platform == o.platform && arch == o.arch;
    }
    static std::optional<PlatformTarget> parse(const std::string& s);
};

std::vector<PlatformTarget> default_targets();
bool is_valid_target(const PlatformTarget& t);

// ── License Policy ──

enum class LicenseVerdict { allowed, review_required, rejected };

LicenseVerdict check_license(const std::string& spdx_id);
const char* license_verdict_label(LicenseVerdict v);
std::string license_tier(const std::string& spdx_id);
std::string license_explanation(const std::string& spdx_id);

// ── Package Descriptor ──

struct FetchInfo {
    std::string method;          // "FetchContent", "header-only", "vendored"
    std::string git_repository;
    std::string git_tag;
};

struct CMakeInfo {
    std::vector<std::string> targets;
    bool header_only = false;
    bool add_subdirectory = true;
    std::string include_dir;
    std::vector<std::string> sources;
};

struct PlatformSupport {
    std::vector<std::string> architectures;
    std::string notes;
};

struct VerificationInfo {
    std::string last_verified;
    std::string verified_version;
    std::map<std::string, std::string> build_status;
};

struct PackageDescriptor {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    std::string license;
    std::string category;
    std::string url;
    FetchInfo fetch;
    CMakeInfo cmake;
    std::map<std::string, PlatformSupport> platforms;
    bool rt_safe = false;
    std::vector<std::string> tags;
    std::vector<std::string> provides;
    std::map<std::string, std::string> overlaps_with_builtin;
    std::string unique_value;
    std::vector<std::string> alternatives;
    VerificationInfo verification;
};

// ── Registry ──

struct Registry {
    int version = 0;
    std::map<std::string, PackageDescriptor> packages;
};

struct RegistryLoadResult {
    Registry registry;
    std::string error;
};

RegistryLoadResult load_registry(const fs::path& registry_path);

// ── Lock File ──

struct LockedPackage {
    std::string version;
    std::string resolved;
    // "sha256-<hex>" of `git ls-tree -r -z --full-tree <commit>`; empty
    // only in locks written before packages were pinned.
    std::string integrity;
    // Full 40-hex commit (older locks may hold the registry tag).
    std::string commit;
    // SPDX license recorded at add time.
    std::string license;
    // "accept-license:<SPDX>" or "commercial" when an override was used.
    std::string license_acceptance;
};

struct LockFile {
    int version = 1;
    std::map<std::string, LockedPackage> packages;
};

LockFile load_lock_file(const fs::path& path);
bool save_lock_file(const fs::path& path, const LockFile& lock);

// ── Target Config ──

std::vector<PlatformTarget> read_project_targets(const fs::path& project_root);
bool write_project_targets(const fs::path& project_root,
                           const std::vector<PlatformTarget>& targets);

// ── Remote Registry ──

// How a remote registry fetch treats the Ed25519 signature published beside
// registry.json (see package_registry_trust.hpp). With a pinned signer key the
// default refuses an unsigned or unverifiable registry; with no key pinned the
// registry is used unsigned and the result carries a notice saying so.
struct RegistryTrustPolicy {
    bool allow_unsigned = false;
    std::string pinned_public_key_hex{kPinnedRegistrySignerPublicKeyHex};
};

// A remote load: the registry plus what the signature check established.
struct RemoteRegistryLoadResult : RegistryLoadResult {
    // Set when the registry loaded without a verified signature under a
    // policy that allows it; callers print it so the gap is never silent.
    std::string warning;
    // True when the bytes behind `registry` carried a signature that verified
    // against the pinned key (now, or when they were cached).
    bool verified = false;
};

// Fetch `url` into `dest`; true on success. The default shells out to curl
// (PowerShell on Windows); tests inject a fake.
using RegistryDownloader = std::function<bool(const std::string& url, const fs::path& dest)>;
RegistryDownloader default_registry_downloader();

// Fetch registry from remote URL and cache it locally with a TTL. A cached
// copy is reused only while it is fresh and its cached registry.json.sig
// still verifies against the pinned key (or the policy allows unsigned
// registries). When the download fails, a stale cache is used under the same
// rule.
RemoteRegistryLoadResult load_remote_registry(const std::string& url, const fs::path& cache_dir,
                                              int ttl_hours = 24,
                                              const RegistryTrustPolicy& policy = {},
                                              const RegistryDownloader& downloader = {});

// Refresh the cached registry (ignore TTL)
RemoteRegistryLoadResult refresh_remote_registry(const std::string& url, const fs::path& cache_dir,
                                                 const RegistryTrustPolicy& policy = {},
                                                 const RegistryDownloader& downloader = {});

// Default remote registry URL
inline const char* default_remote_registry_url() {
    return "https://raw.githubusercontent.com/danielraffel/pulp-packages/main/registry.json";
}

// Default cache directory
fs::path default_cache_dir();

// ── Semver ──

struct SemVer {
    int major = 0, minor = 0, patch = 0;
    std::string pre;  // pre-release label

    bool operator<(const SemVer& o) const;
    bool operator==(const SemVer& o) const;
    bool compatible_with(const SemVer& constraint) const;  // ^constraint
    static std::optional<SemVer> parse(const std::string& s);
};

// ── Quality Score ──

struct QualityScore {
    int total = 0;        // 0-100
    int license = 0;      // 0-25
    int platforms = 0;    // 0-25
    int verification = 0; // 0-25
    int maintenance = 0;  // 0-25
    std::string tier;     // "official", "community", "experimental"
};

QualityScore compute_quality(const PackageDescriptor& pkg);

// ── Queries ──

std::vector<PlatformTarget> unsupported_targets(
    const PackageDescriptor& pkg,
    const std::vector<PlatformTarget>& targets);

std::vector<const PackageDescriptor*> search(const Registry& reg,
                                              const std::string& query);

}  // namespace pulp::cli::pkg
