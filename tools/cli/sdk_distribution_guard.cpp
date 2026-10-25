#include "sdk_distribution_guard.hpp"

#include "configure_defaults.hpp"
#include "ship_tracing_guard.hpp"

#include <fstream>
#include <sstream>
#include <string_view>

namespace pulp::cli {

std::filesystem::path vellum_d15_artifact_offender(const std::filesystem::path& artifact,
                                                   bool follow_directory_symlinks) {
    constexpr auto runtime_name = "libvellum-gpu.dylib";
    std::error_code ec;
    if (artifact.empty())
        return {};
    const bool artifact_exists = std::filesystem::exists(artifact, ec);
    if (ec)
        return artifact;
    if (!artifact_exists)
        return {};
    if (artifact.filename() == runtime_name)
        return artifact;
    if (!std::filesystem::is_directory(artifact, ec))
        return {};
    // A filesystem error means the selected artifact could not be inspected.
    // Fail closed by returning the artifact itself as the offending path.
    if (ec)
        return artifact;

    const auto options = follow_directory_symlinks
                             ? std::filesystem::directory_options::follow_directory_symlink
                             : std::filesystem::directory_options::none;
    std::filesystem::recursive_directory_iterator it(artifact, options, ec), end;
    if (ec)
        return artifact;
    while (!ec && it != end) {
        if (it->path().filename() == runtime_name)
            return it->path();
        if (it.depth() > 64)
            return artifact;
        it.increment(ec);
    }
    if (ec)
        return artifact;
    return {};
}

bool sdk_allows_distribution(const std::filesystem::path& build_dir, std::string& error) {
    std::ifstream cache(build_dir / "CMakeCache.txt");
    if (!cache)
        return true;

    bool development_sdk = false;
    bool has_vellum_d15 = false;
    std::string line;
    while (std::getline(cache, line)) {
        development_sdk = development_sdk ||
                          line == "PULP_SDK_DISTRIBUTION_ELIGIBLE:INTERNAL=FALSE" ||
                          line == "PULP_SDK_DISTRIBUTION_ELIGIBLE:BOOL=FALSE" ||
                          line == "PULP_SDK_DEVELOPMENT:INTERNAL=TRUE" ||
                          line == "PULP_SDK_DEVELOPMENT:BOOL=TRUE";
        has_vellum_d15 = has_vellum_d15 || line == "PULP_GPU_AUDIO_HAS_VELLUM_D15:INTERNAL=TRUE" ||
                         line == "PULP_GPU_AUDIO_HAS_VELLUM_D15:BOOL=TRUE";
    }
    if (has_vellum_d15) {
        error = "the configured build contains the development-only Vellum D15 GPU-audio "
                "provider. Packaging, notarizing, sharing, and releasing it are disabled "
                "until the production archive provenance contract is implemented";
        return false;
    }
    if (development_sdk) {
        error = "the configured Pulp SDK is development-only "
                "(distribution_eligible=false). Reconfigure against a released SDK before "
                "packaging, notarizing, sharing, or releasing artifacts";
        return false;
    }
    return true;
}

// The install name of the shared runtime is @rpath/libpulp-dev-shared-ui.<version>.dylib
// (tools/cmake/PulpDevSharedUi.cmake), so these bytes are in the runtime itself
// and in the load commands of every module linked against it.
static constexpr std::string_view kDevSharedRuntimePrefix = "libpulp-dev-shared-ui.";

static bool is_dev_shared_offender(const std::filesystem::path& file) {
    if (file.filename().string().rfind(kDevSharedRuntimePrefix, 0) == 0)
        return true;
    return file_contains_bytes(file, kDevSharedRuntimePrefix);
}

std::filesystem::path dev_shared_artifact_offender(const std::filesystem::path& artifact,
                                                   bool follow_directory_symlinks) {
    std::error_code ec;
    if (artifact.empty())
        return {};
    const bool artifact_exists = std::filesystem::exists(artifact, ec);
    if (ec)
        return artifact;
    if (!artifact_exists)
        return {};
    if (!std::filesystem::is_directory(artifact, ec)) {
        if (ec)
            return artifact;
        return is_dev_shared_offender(artifact) ? artifact : std::filesystem::path{};
    }

    const auto options = follow_directory_symlinks
                             ? std::filesystem::directory_options::follow_directory_symlink
                             : std::filesystem::directory_options::none;
    std::filesystem::recursive_directory_iterator it(artifact, options, ec), end;
    if (ec)
        return artifact;
    while (!ec && it != end) {
        if (it.depth() > 64)
            return artifact;
        if (it->is_regular_file(ec) && !ec && is_dev_shared_offender(it->path()))
            return it->path();
        if (ec)
            return artifact;
        it.increment(ec);
    }
    if (ec)
        return artifact;
    return {};
}

bool is_local_signing_identity(std::string_view identity) {
    // The certificate common names are "Apple Development: <name> (<team>)"
    // and "Mac Developer: ...". Matching through the colon keeps an identity
    // that merely starts with those words from counting as local.
    return identity == "-" || identity.rfind("Apple Development:", 0) == 0 ||
           identity.rfind("Mac Developer:", 0) == 0;
}

bool dev_shared_build_allows_distribution(const std::filesystem::path& build_dir,
                                          std::string& error) {
    std::ifstream cache(build_dir / "CMakeCache.txt");
    if (!cache)
        return true;
    std::ostringstream text;
    text << cache.rdbuf();
    const auto value = cmake_cache_value(text.str(), kDevSharedUiOption);
    if (!value || !cmake_value_on(*value))
        return true;
    error = "the configured build links plug-ins against the development-only shared Pulp "
            "runtime (PULP_DEV_SHARED_UI=ON), which loads only from this build tree. Only a "
            "static build can be signed for distribution, packaged, notarized, shared, or "
            "released";
    return false;
}

} // namespace pulp::cli
