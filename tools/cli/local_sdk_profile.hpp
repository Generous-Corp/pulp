// local_sdk_profile.hpp — immutable checkout-backed SDK profile primitives.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace pulp::cli::local_sdk {

namespace fs = std::filesystem;

inline constexpr int kForgeProfileRevision = 2;
inline constexpr const char* kForgeProfileName = "forge-dev";
inline constexpr const char* kProvenanceSchema = "pulp.sdk-provenance.v1";

struct Identity {
    std::string sdk_version;
    std::string source_git_sha;
    std::string platform;
    std::string cmake_version;
    std::string generator;
    std::string compiler;
    std::string macos_sdk;
    std::string deployment_target;
    std::string skia_identity;
    std::string vst3_git_sha;
    std::string ausdk_git_sha;
};

bool operator==(const Identity& lhs, const Identity& rhs);

struct Paths {
    fs::path install_prefix;
    fs::path build_dir;
    std::string input_fingerprint;
};

struct Validation {
    bool ok = false;
    std::vector<std::string> errors;
};

struct InstallRequest {
    bool ok = false;
    bool from_local = false;
    bool print_path = false;
    bool version_explicit = false;
    std::string version;
    std::string profile;
    std::string error;
};

enum class ArchiveSliceAction {
    Keep,
    ThinToArm64,
    Reject,
};

InstallRequest parse_install_arguments(const std::vector<std::string>& args,
                                       const std::string& default_version);
ArchiveSliceAction archive_slice_action(const std::string& lipo_architectures);
std::string input_fingerprint(const Identity& identity);
Paths profile_paths(const fs::path& pulp_home, const Identity& identity);
std::vector<std::string> configure_arguments(const fs::path& source, const fs::path& build,
                                             const fs::path& install_prefix,
                                             const Identity& identity);
std::map<std::string, std::string> parse_cmake_cache(const fs::path& cache_path);
Validation validate_staged_install(const fs::path& prefix, const fs::path& build_dir,
                                   const Identity& expected, const fs::path& expected_skia_dir);
Validation validate_published_install(const fs::path& prefix, const Identity& expected);
std::string serialize_provenance(const Identity& identity, const std::string& fingerprint);
bool write_file_atomically(const fs::path& destination, const std::string& contents,
                           std::string& error);

} // namespace pulp::cli::local_sdk
