#include "sdk_distribution_guard.hpp"

#include <fstream>

namespace pulp::cli {

std::filesystem::path vellum_d15_artifact_offender(const std::filesystem::path& artifact) {
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

    const auto options = std::filesystem::directory_options::follow_directory_symlink;
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

} // namespace pulp::cli
