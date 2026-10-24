// Unit tests for the ship-time PULP_DEV_BUILD guard (tools/cli/ship_dev_build_guard.hpp).
//
// The guard refuses to sign/package a build dir whose CMakeCache.txt records
// PULP_DEV_BUILD=ON, the configuration that compiles the on-disk ui.js hot-reload
// path into the artifact. Fixture caches stand in for real configures.

#include "tools/cli/ship_dev_build_guard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path unique_dir(const char* leaf) {
    auto d = fs::temp_directory_path() / "pulp-ship-dev-build-guard-test" / leaf;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

void write_cache(const fs::path& build_dir, const std::string& text) {
    std::ofstream f(build_dir / "CMakeCache.txt", std::ios::binary);
    f << text;
}

} // namespace

TEST_CASE("a cache recording PULP_DEV_BUILD=ON is a dev build", "[ship][dev-build]") {
    auto build = unique_dir("on");
    write_cache(build, "CMAKE_BUILD_TYPE:STRING=Release\nPULP_DEV_BUILD:BOOL=ON\n");
    REQUIRE(pulp::cli::dev_build_cache_value(build) == "ON");
    REQUIRE(pulp::cli::build_dir_is_dev_build(build));
}

TEST_CASE("a cache recording PULP_DEV_BUILD=OFF ships", "[ship][dev-build]") {
    auto build = unique_dir("off");
    write_cache(build, "PULP_DEV_BUILD:BOOL=OFF\nCMAKE_BUILD_TYPE:STRING=Release\n");
    REQUIRE(pulp::cli::dev_build_cache_value(build) == "OFF");
    REQUIRE_FALSE(pulp::cli::build_dir_is_dev_build(build));
}

TEST_CASE("an unrecorded option or a missing cache is not a dev build", "[ship][dev-build]") {
    auto build = unique_dir("unrecorded");
    write_cache(build, "CMAKE_BUILD_TYPE:STRING=Release\n");
    REQUIRE_FALSE(pulp::cli::dev_build_cache_value(build).has_value());
    REQUIRE_FALSE(pulp::cli::build_dir_is_dev_build(build));
    REQUIRE_FALSE(pulp::cli::build_dir_is_dev_build(build / "missing"));
}

TEST_CASE("every CMake truthy spelling counts as on", "[ship][dev-build]") {
    for (const char* on : {"ON", "on", "TRUE", "True", "YES", "Y", "1"})
        REQUIRE(pulp::cli::cmake_cache_flag_on(on));
    for (const char* off : {"OFF", "FALSE", "NO", "N", "0", "", "IGNORE", "NOTFOUND"})
        REQUIRE_FALSE(pulp::cli::cmake_cache_flag_on(off));
}

TEST_CASE("the shippable configure mirrors the dev cache with PULP_DEV_BUILD=OFF",
          "[ship][dev-build]") {
    using pulp::cli::ship_build_configure_args;
    SECTION("an SDK consumer's cache: generator, build type, SDK location, switches") {
        const std::string cache = "CMAKE_GENERATOR:INTERNAL=Ninja\n"
                                  "CMAKE_BUILD_TYPE:STRING=Release\n"
                                  "CMAKE_PREFIX_PATH:PATH=/opt/pulp-sdk\n"
                                  "Pulp_DIR:PATH=/opt/pulp-sdk/lib/cmake/Pulp\n"
                                  "PULP_JS_ENGINE:STRING=quickjs\n"
                                  "PULP_MACOS_ARCH:STRING=universal\n"
                                  "PULP_DEV_BUILD:BOOL=ON\n"
                                  "UNRELATED:STRING=x\n";
        const std::vector<std::string> expected{"-G",
                                                "Ninja",
                                                "-DCMAKE_BUILD_TYPE=Release",
                                                "-DCMAKE_PREFIX_PATH=/opt/pulp-sdk",
                                                "-DPulp_DIR=/opt/pulp-sdk/lib/cmake/Pulp",
                                                "-DPULP_JS_ENGINE=quickjs",
                                                "-DPULP_MACOS_ARCH=universal",
                                                "-DPULP_DEV_BUILD=OFF"};
        REQUIRE(ship_build_configure_args(cache) == expected);
    }
    SECTION("a source checkout's cache drops the test tree") {
        const std::string cache = "CMAKE_GENERATOR:INTERNAL=Ninja\n"
                                  "CMAKE_BUILD_TYPE:STRING=Release\n"
                                  "PULP_BUILD_TESTS:BOOL=ON\n"
                                  "PULP_BUILD_EXAMPLES:BOOL=OFF\n"
                                  "PULP_REQUIRE_CHECKOUT_DEPENDENCIES:BOOL=ON\n"
                                  "PULP_DEV_BUILD:BOOL=ON\n";
        const std::vector<std::string> expected{"-G",
                                                "Ninja",
                                                "-DCMAKE_BUILD_TYPE=Release",
                                                "-DPULP_BUILD_EXAMPLES=OFF",
                                                "-DPULP_REQUIRE_CHECKOUT_DEPENDENCIES=ON",
                                                "-DPULP_BUILD_TESTS=OFF",
                                                "-DPULP_DEV_BUILD=OFF"};
        REQUIRE(ship_build_configure_args(cache) == expected);
    }
    SECTION("an empty or build-type-less cache still pins Release and OFF") {
        REQUIRE(ship_build_configure_args("") ==
                std::vector<std::string>{"-DCMAKE_BUILD_TYPE=Release", "-DPULP_DEV_BUILD=OFF"});
        REQUIRE(ship_build_configure_args(
                    "CMAKE_GENERATOR:INTERNAL=Xcode\nCMAKE_BUILD_TYPE:STRING=\n") ==
                std::vector<std::string>{"-G", "Xcode", "-DCMAKE_BUILD_TYPE=Release",
                                         "-DPULP_DEV_BUILD=OFF"});
    }
    SECTION("the ship build dir sits beside build/") {
        REQUIRE(pulp::cli::ship_build_dir("/proj") == fs::path("/proj") / "build-ship");
    }
}
