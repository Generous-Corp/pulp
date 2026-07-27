#include "tools/cli/sdk_distribution_guard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path = fs::temp_directory_path() /
                    ("pulp-sdk-distribution-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() { fs::create_directories(path); }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_cache(const fs::path& build, const std::string& line) {
    fs::create_directories(build);
    std::ofstream(build / "CMakeCache.txt") << line << "\n";
}

} // namespace

TEST_CASE("release tools reject a development Pulp SDK", "[cli][sdk][ship]") {
    TempDir tmp;
    write_cache(tmp.path, "PULP_SDK_DISTRIBUTION_ELIGIBLE:INTERNAL=FALSE");
    std::string error;
    REQUIRE_FALSE(pulp::cli::sdk_allows_distribution(tmp.path, error));
    REQUIRE(error.find("development-only") != std::string::npos);
}

TEST_CASE("release tools retain compatibility with released SDK caches", "[cli][sdk][ship]") {
    TempDir tmp;
    write_cache(tmp.path, "PULP_SDK_DISTRIBUTION_ELIGIBLE:INTERNAL=TRUE");
    std::string error;
    REQUIRE(pulp::cli::sdk_allows_distribution(tmp.path, error));
    REQUIRE(error.empty());
}
