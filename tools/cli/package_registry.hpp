// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
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
    std::string include_dir;
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
    std::string integrity;
    std::string commit;
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

// Fetch registry from remote URL, cache locally with TTL
RegistryLoadResult load_remote_registry(const std::string& url,
                                         const fs::path& cache_dir,
                                         int ttl_hours = 24);

// Refresh the cached registry (ignore TTL)
RegistryLoadResult refresh_remote_registry(const std::string& url,
                                            const fs::path& cache_dir);

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
