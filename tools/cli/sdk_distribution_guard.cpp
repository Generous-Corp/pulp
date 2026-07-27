#include "sdk_distribution_guard.hpp"

#include <fstream>

namespace pulp::cli {

bool sdk_allows_distribution(const std::filesystem::path& build_dir, std::string& error) {
    std::ifstream cache(build_dir / "CMakeCache.txt");
    if (!cache)
        return true;

    std::string line;
    while (std::getline(cache, line)) {
        if (line == "PULP_SDK_DISTRIBUTION_ELIGIBLE:INTERNAL=FALSE" ||
            line == "PULP_SDK_DISTRIBUTION_ELIGIBLE:BOOL=FALSE" ||
            line == "PULP_SDK_DEVELOPMENT:INTERNAL=TRUE" ||
            line == "PULP_SDK_DEVELOPMENT:BOOL=TRUE") {
            error =
                "the configured Pulp SDK is development-only "
                "(distribution_eligible=false). Reconfigure against a released SDK before "
                "packaging, notarizing, sharing, or releasing artifacts";
            return false;
        }
    }
    return true;
}

} // namespace pulp::cli
