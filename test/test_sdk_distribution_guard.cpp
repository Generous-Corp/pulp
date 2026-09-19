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

TEST_CASE("release tools reject a source build with Vellum D15", "[cli][sdk][ship]") {
    TempDir tmp;
    write_cache(tmp.path, "PULP_GPU_AUDIO_HAS_VELLUM_D15:INTERNAL=TRUE");
    std::string error;
    REQUIRE_FALSE(pulp::cli::sdk_allows_distribution(tmp.path, error));
    REQUIRE(error.find("development-only Vellum D15") != std::string::npos);
}

TEST_CASE("distribution artifact scan finds a nested Vellum D15 runtime", "[cli][sdk][ship]") {
    TempDir tmp;
    const auto bundle = tmp.path / "Stale.clap";
    const auto runtime = bundle / "Contents" / "MacOS" / "libvellum-gpu.dylib";
    fs::create_directories(runtime.parent_path());
    std::ofstream(runtime) << "development-only runtime\n";

    REQUIRE(pulp::cli::vellum_d15_artifact_offender(bundle) == runtime);
}

TEST_CASE("distribution artifact scan ignores an unrelated selected bundle", "[cli][sdk][ship]") {
    TempDir tmp;
    const auto bundle = tmp.path / "Clean.clap";
    fs::create_directories(bundle / "Contents" / "MacOS");
    std::ofstream(bundle / "Contents" / "MacOS" / "Clean") << "clean\n";

    REQUIRE(pulp::cli::vellum_d15_artifact_offender(bundle).empty());
}

TEST_CASE("distribution artifact scan fails closed on a symlink cycle", "[cli][sdk][ship]") {
    TempDir tmp;
    const auto bundle = tmp.path / "Cyclic.clap";
    fs::create_directories(bundle / "Contents");
    std::error_code ec;
    fs::create_directory_symlink(bundle, bundle / "Contents" / "loop", ec);
    if (ec) {
        SKIP("directory symlinks unavailable for this filesystem");
    }

    REQUIRE(pulp::cli::vellum_d15_artifact_offender(bundle) == bundle);
}
