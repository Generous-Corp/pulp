#include "support/unique_temp_dir.hpp"
#include "tools/cli/sdk_distribution_guard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path = pulp::test::make_unique_temp_dir("pulp-sdk-distribution");
    TempDir() = default;
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

namespace {

void write_dev_shared_module(const fs::path& file) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << "\xcf\xfa\xed\xfe" << "fixture LC_LOAD_DYLIB "
        << "@rpath/libpulp-dev-shared-ui.1.11.0.dylib" << '\0';
}

} // namespace

TEST_CASE("release tools reject a PULP_DEV_SHARED_UI build cache", "[cli][sdk][ship][dev-shared]") {
    for (const char* value : {"ON", "on", "TRUE", "1", "YES"}) {
        DYNAMIC_SECTION("PULP_DEV_SHARED_UI:BOOL=" << value) {
            TempDir tmp;
            write_cache(tmp.path, std::string("PULP_DEV_SHARED_UI:BOOL=") + value);
            std::string error;
            REQUIRE_FALSE(pulp::cli::dev_shared_build_allows_distribution(tmp.path, error));
            REQUIRE(error.find("PULP_DEV_SHARED_UI=ON") != std::string::npos);
        }
    }
}

TEST_CASE("release tools accept a build cache with PULP_DEV_SHARED_UI off or absent",
          "[cli][sdk][ship][dev-shared]") {
    TempDir tmp;
    std::string error;
    REQUIRE(pulp::cli::dev_shared_build_allows_distribution(tmp.path, error));
    write_cache(tmp.path, "PULP_DEV_SHARED_UI:BOOL=OFF");
    REQUIRE(pulp::cli::dev_shared_build_allows_distribution(tmp.path, error));
    write_cache(tmp.path, "CMAKE_BUILD_TYPE:STRING=Release");
    REQUIRE(pulp::cli::dev_shared_build_allows_distribution(tmp.path, error));
    REQUIRE(error.empty());
}

TEST_CASE("dev-shared artifact scan finds a module that loads the shared runtime",
          "[cli][sdk][ship][dev-shared]") {
    TempDir tmp;
    const auto bundle = tmp.path / "FastGain.vst3";
    const auto module = bundle / "Contents" / "MacOS" / "FastGain";
    write_dev_shared_module(module);
    std::ofstream(bundle / "Contents" / "Info.plist") << "<plist/>\n";

    REQUIRE(pulp::cli::dev_shared_artifact_offender(bundle) == module);
    REQUIRE(pulp::cli::dev_shared_artifact_offender(module) == module);
}

TEST_CASE("dev-shared artifact scan finds a copied runtime by name",
          "[cli][sdk][ship][dev-shared]") {
    TempDir tmp;
    const auto app = tmp.path / "FastGain.app";
    const auto runtime = app / "Contents" / "Frameworks" / "libpulp-dev-shared-ui.1.11.0.dylib";
    fs::create_directories(runtime.parent_path());
    std::ofstream(runtime) << "runtime\n";

    REQUIRE(pulp::cli::dev_shared_artifact_offender(app) == runtime);
}

TEST_CASE("dev-shared artifact scan ignores a statically linked bundle",
          "[cli][sdk][ship][dev-shared]") {
    TempDir tmp;
    const auto bundle = tmp.path / "FastGain.clap";
    fs::create_directories(bundle / "Contents" / "MacOS");
    std::ofstream(bundle / "Contents" / "MacOS" / "FastGain", std::ios::binary)
        << "\xcf\xfa\xed\xfe" << "@rpath/libwgpu_native.dylib" << '\0';

    REQUIRE(pulp::cli::dev_shared_artifact_offender(bundle).empty());
    REQUIRE(pulp::cli::dev_shared_artifact_offender(tmp.path / "missing.clap").empty());
}

TEST_CASE("dev-shared artifact scan fails closed on a symlink cycle",
          "[cli][sdk][ship][dev-shared]") {
    TempDir tmp;
    const auto bundle = tmp.path / "Cyclic.vst3";
    fs::create_directories(bundle / "Contents");
    std::error_code ec;
    fs::create_directory_symlink(bundle, bundle / "Contents" / "loop", ec);
    if (ec) {
        SKIP("directory symlinks unavailable for this filesystem");
    }

    REQUIRE(pulp::cli::dev_shared_artifact_offender(bundle) == bundle);
}

TEST_CASE("only ad hoc and Apple Development identities sign a dev-shared build",
          "[cli][sdk][ship][dev-shared]") {
    REQUIRE(pulp::cli::is_local_signing_identity("-"));
    REQUIRE(pulp::cli::is_local_signing_identity("Apple Development: Dev Person (ABCDE12345)"));
    REQUIRE(pulp::cli::is_local_signing_identity("Mac Developer: Dev Person (ABCDE12345)"));
    REQUIRE_FALSE(pulp::cli::is_local_signing_identity("Developer ID Application: Co (TEAMID)"));
    REQUIRE_FALSE(pulp::cli::is_local_signing_identity("3rd Party Mac Developer Application: Co"));
    REQUIRE_FALSE(pulp::cli::is_local_signing_identity(""));
    // Lookalikes that only start with the same words are not local identities.
    REQUIRE_FALSE(pulp::cli::is_local_signing_identity("Apple Developmental Labs (TEAMID)"));
    REQUIRE_FALSE(pulp::cli::is_local_signing_identity("Apple Development"));
    REQUIRE_FALSE(pulp::cli::is_local_signing_identity("Mac Developers Inc: Release"));
}

TEST_CASE("a container's expanded size is read from zipinfo and PackageInfo",
          "[cli][sdk][ship][dev-shared]") {
    using pulp::cli::pkg_payload_install_bytes;
    using pulp::cli::zipinfo_uncompressed_bytes;
    REQUIRE(zipinfo_uncompressed_bytes(
                "3 files, 1048576 bytes uncompressed, 2048 bytes compressed:  99.8%\n") ==
            1048576u);
    REQUIRE(zipinfo_uncompressed_bytes("1 file, 0 bytes uncompressed, 0 bytes compressed:  0.0%") ==
            0u);
    REQUIRE_FALSE(zipinfo_uncompressed_bytes("Empty zipfile.\n").has_value());
    REQUIRE_FALSE(zipinfo_uncompressed_bytes("").has_value());

    REQUIRE(pkg_payload_install_bytes(
                R"(<pkg-info identifier="com.x"><payload numberOfFiles="3" installKBytes="4096"/>)"
                R"(</pkg-info>)") == 4096u * 1024);
    // The attribute is read from the payload element, not from elsewhere.
    REQUIRE_FALSE(pkg_payload_install_bytes(R"(<pkg-info installKBytes="9"><scripts/></pkg-info>)")
                      .has_value());
    REQUIRE_FALSE(
        pkg_payload_install_bytes(R"(<payload numberOfFiles="1"/><bundle installKBytes="9"/>)")
            .has_value());
    REQUIRE_FALSE(pkg_payload_install_bytes(R"(<payload installKBytes=""/>)").has_value());
}

TEST_CASE("a container is only extracted when it fits with room to spare",
          "[cli][sdk][ship][dev-shared]") {
    constexpr std::uintmax_t mib = 1024 * 1024;
    REQUIRE(pulp::cli::container_space_refusal(10 * mib, 1024 * mib).empty());
    REQUIRE(pulp::cli::container_space_refusal(0, 256 * mib).empty());
    // Fits, but would leave less than the reserve.
    const auto tight = pulp::cli::container_space_refusal(800 * mib, 1000 * mib);
    REQUIRE(tight.find("838860800 bytes") != std::string::npos);
    REQUIRE(tight.find("1048576000 bytes free") != std::string::npos);
    REQUIRE_FALSE(pulp::cli::container_space_refusal(2000 * mib, 1000 * mib).empty());
}
