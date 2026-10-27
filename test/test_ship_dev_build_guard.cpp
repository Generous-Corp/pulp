// Unit tests for the ship-time PULP_DEV_BUILD guard (tools/cli/ship_dev_build_guard.hpp).
//
// The guard refuses to sign/package a build dir whose CMakeCache.txt records
// PULP_DEV_BUILD=ON, the configuration that compiles the on-disk ui.js hot-reload
// path into the artifact. Fixture caches stand in for real configures.

#include "tools/cli/ship_dev_build_guard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
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

TEST_CASE("the shippable initial cache carries every configured entry except the dev-only switches",
          "[ship][dev-build]") {
    using pulp::cli::ship_build_initial_cache;
    const std::string cache = "# This is the CMakeCache file.\n"
                              "// Build type\n"
                              "CMAKE_BUILD_TYPE:STRING=Release\n"
                              "CMAKE_CXX_COMPILER:FILEPATH=/opt/llvm/bin/clang++\n"
                              "CMAKE_OSX_SYSROOT:PATH=/Applications/Xcode.app/SDKs/MacOSX.sdk\n"
                              "CMAKE_PREFIX_PATH:PATH=/opt/pulp-sdk\n"
                              "PULP_ENABLE_AAX:BOOL=ON\n"
                              "PULP_AAX_SDK_DIR:PATH=/Users/dev/aax_sdk\n"
                              "PULP_ENABLE_ARA:BOOL=ON\n"
                              "PULP_ENABLE_GPU:BOOL=ON\n"
                              "PULP_PROFILE:STRING=forge-dev\n"
                              "PULP_BUILD_TESTS:BOOL=ON\n"
                              "PULP_BUILD_EXAMPLES:BOOL=ON\n"
                              "PULP_DEV_BUILD:BOOL=ON\n"
                              "PULP_DEV_BRIDGE_EXEC:BOOL=ON\n"
                              "PULP_DEV_SHARED_UI:BOOL=ON\n"
                              "PULP_ENABLE_AUDIO_PROBES:BOOL=ON\n"
                              "PULP_TRACING:BOOL=ON\n"
                              "SOME_FLAG:UNINITIALIZED=1\n"
                              "CMAKE_GENERATOR:INTERNAL=Ninja\n"
                              "CMAKE_CACHEFILE_DIR:INTERNAL=/proj/build\n"
                              "CMAKE_HOME_DIRECTORY:INTERNAL=/proj\n"
                              "PULP_HAS_SKIA:INTERNAL=TRUE\n"
                              "CMAKE_PLATFORM_INFO_INITIALIZED:STATIC=1\n";
    const auto script = ship_build_initial_cache(cache);
    auto has = [&](const std::string& line) { return script.find(line) != std::string::npos; };
    // Everything the developer configured comes along, verbatim.
    CHECK(has("set(PULP_ENABLE_ARA [==[ON]==] CACHE BOOL"));
    CHECK(has("set(PULP_ENABLE_AAX [==[ON]==] CACHE BOOL"));
    CHECK(has("set(PULP_AAX_SDK_DIR [==[/Users/dev/aax_sdk]==] CACHE PATH"));
    CHECK(has("set(CMAKE_CXX_COMPILER [==[/opt/llvm/bin/clang++]==] CACHE FILEPATH"));
    CHECK(has("set(CMAKE_OSX_SYSROOT [==[/Applications/Xcode.app/SDKs/MacOSX.sdk]==] CACHE PATH"));
    CHECK(has("set(PULP_ENABLE_GPU [==[ON]==] CACHE BOOL"));
    CHECK(has("set(CMAKE_BUILD_TYPE [==[Release]==] CACHE STRING"));
    CHECK(has("set(SOME_FLAG [==[1]==] CACHE STRING"));
    // The profile is forced to release; every option it enforces, and the
    // dev-shared runtime, is dropped (never carried verbatim) and set OFF.
    CHECK(has("set(PULP_PROFILE [==[release]==] CACHE STRING"));
    CHECK_FALSE(has("forge-dev"));
    for (const char* option : {"PULP_BUILD_TESTS", "PULP_BUILD_EXAMPLES", "PULP_DEV_BUILD",
                               "PULP_DEV_BRIDGE_EXEC", "PULP_ENABLE_AUDIO_PROBES", "PULP_TRACING",
                               "PULP_DEV_SHARED_UI"}) {
        INFO(option);
        CHECK(has("set(" + std::string(option) + " OFF CACHE BOOL"));
        CHECK_FALSE(has(std::string(option) + " [==["));
    }
    // Internal and static bookkeeping never crosses over.
    CHECK_FALSE(has("CMAKE_GENERATOR"));
    CHECK_FALSE(has("CMAKE_CACHEFILE_DIR"));
    CHECK_FALSE(has("CMAKE_HOME_DIRECTORY"));
    CHECK_FALSE(has("PULP_HAS_SKIA"));
    CHECK_FALSE(has("CMAKE_PLATFORM_INFO_INITIALIZED"));

    SECTION("a cache without a build type pins Release") {
        const auto bare = ship_build_initial_cache("CMAKE_GENERATOR:INTERNAL=Xcode\n");
        CHECK(bare.find("set(CMAKE_BUILD_TYPE [==[Release]==] CACHE STRING") != std::string::npos);
        CHECK(bare.find("set(PULP_DEV_BUILD OFF CACHE BOOL") != std::string::npos);
    }
    SECTION("the configure arguments are the generator and the initial cache") {
        REQUIRE(pulp::cli::ship_build_configure_args(cache, "/proj/build-ship/init.cmake") ==
                std::vector<std::string>{"-G", "Ninja", "-C", "/proj/build-ship/init.cmake"});
        REQUIRE(pulp::cli::ship_build_configure_args("", "/x/init.cmake") ==
                std::vector<std::string>{"-C", "/x/init.cmake"});
    }
    SECTION("the ship build dir sits beside build/") {
        REQUIRE(pulp::cli::ship_build_dir("/proj") == fs::path("/proj") / "build-ship");
    }
}

// The drop list is the release profile's own enforced list, generated from
// PulpProfiles.cmake, so a cache entry for ANY enforced option is dropped and
// the shippable configure cannot carry one the profile would refuse.
TEST_CASE("the dropped entries are the release profile's enforced options", "[ship][dev-build]") {
    using pulp::cli::kProfileEnforcedOptions;
    using pulp::cli::ship_build_initial_cache;
    REQUIRE(pulp::cli::profile_enforces("PULP_DEV_BUILD"));
    REQUIRE(pulp::cli::profile_enforces("PULP_BUILD_EXAMPLES"));
    REQUIRE(pulp::cli::profile_enforces("PULP_DEV_BRIDGE_EXEC"));
    REQUIRE_FALSE(pulp::cli::profile_enforces("PULP_ENABLE_GPU"));
    REQUIRE_FALSE(pulp::cli::profile_enforces("PULP_DEV_SHARED_UI"));
    for (const auto option : kProfileEnforcedOptions) {
        INFO(option);
        const auto name = std::string(option);
        const auto script = ship_build_initial_cache(name + ":BOOL=ON\nPULP_ENABLE_GPU:BOOL=ON\n");
        CHECK(script.find("set(" + name + " [==[") == std::string::npos);
        CHECK(script.find("set(" + name + " OFF CACHE BOOL") != std::string::npos);
        CHECK(script.find("set(PULP_ENABLE_GPU [==[ON]==] CACHE BOOL") != std::string::npos);
    }
}

TEST_CASE("the shippable build reconfigures when its cache is missing, incomplete or stale",
          "[ship][dev-build]") {
    using pulp::cli::ship_build_needs_configure;
    const auto root = unique_dir("needs-configure");
    const auto dev = root / "build";
    const auto ship = root / "build-ship";
    fs::create_directories(dev);
    fs::create_directories(ship);
    write_cache(dev, "CMAKE_GENERATOR:INTERNAL=Ninja\nPULP_DEV_BUILD:BOOL=ON\n");
    const std::string carried = "set(X [==[1]==] CACHE BOOL \"\")\n";

    SECTION("no ship cache") {
        REQUIRE(ship_build_needs_configure(dev, ship, carried));
    }
    SECTION("a cache a failed configure left behind has no generator") {
        write_cache(ship, "CMAKE_BUILD_TYPE:STRING=Release\n");
        REQUIRE_FALSE(pulp::cli::ship_build_is_configured(ship));
        REQUIRE(ship_build_needs_configure(dev, ship, carried));
    }
    SECTION("a completed configure with the same carried entries is current") {
        write_cache(ship, "CMAKE_GENERATOR:INTERNAL=Ninja\nPULP_DEV_BUILD:BOOL=OFF\n");
        std::ofstream(pulp::cli::ship_initial_cache_path(ship), std::ios::binary) << carried;
        // The ship cache must be at least as new as the dev cache.
        fs::last_write_time(ship / "CMakeCache.txt",
                            fs::last_write_time(dev / "CMakeCache.txt") + std::chrono::seconds(2));
        REQUIRE_FALSE(ship_build_needs_configure(dev, ship, carried));
        SECTION("until the carried entries differ") {
            REQUIRE(ship_build_needs_configure(dev, ship,
                                               carried + "set(Y [==[2]==] CACHE BOOL \"\")\n"));
        }
        SECTION("or the dev cache is newer") {
            fs::last_write_time(dev / "CMakeCache.txt",
                                fs::last_write_time(ship / "CMakeCache.txt") +
                                    std::chrono::seconds(2));
            REQUIRE(ship_build_needs_configure(dev, ship, carried));
        }
    }
    fs::remove_all(root);
}

TEST_CASE("the shippable build is refused when it lacks a plug-in artifact the dev build has",
          "[ship][dev-build]") {
    const auto root = unique_dir("artifact-set");
    const auto dev = root / "build";
    const auto ship = root / "build-ship";
    for (const char* rel :
         {"build/VST3/Synth.vst3", "build/AAX/Synth.aaxplugin", "build/CLAP/Synth.clap",
          "build/Standalone/Synth.app", "build-ship/VST3/Synth.vst3", "build-ship/CLAP/Synth.clap",
          "build-ship/Standalone/Synth.app", "build-ship/LV2/Synth.lv2"})
        fs::create_directories(root / rel);
    const auto dev_set = pulp::cli::plugin_artifact_set(dev);
    const auto ship_set = pulp::cli::plugin_artifact_set(ship);
    REQUIRE(dev_set == std::set<std::string>{"AAX/Synth.aaxplugin", "CLAP/Synth.clap",
                                             "Standalone/Synth.app", "VST3/Synth.vst3"});
    const auto mismatch = pulp::cli::plugin_artifact_set_mismatch(dev_set, ship_set);
    REQUIRE(mismatch.find("AAX/Synth.aaxplugin") != std::string::npos);
    REQUIRE(mismatch.find("LV2") == std::string::npos);
    // Extra artifacts in build-ship are not a mismatch; the dev build may have
    // been built for a subset of targets.
    REQUIRE(pulp::cli::plugin_artifact_set_mismatch(ship_set, ship_set).empty());
    REQUIRE(pulp::cli::plugin_artifact_set_mismatch({}, ship_set).empty());
    fs::remove_all(root);
}

TEST_CASE("a dev-shared cache is recognized and never reaches the shippable build",
          "[ship][dev-build][dev-shared]") {
    auto dir = unique_dir("dev-shared");
    CHECK_FALSE(pulp::cli::build_dir_is_dev_shared(dir));
    write_cache(dir, "PULP_DEV_SHARED_UI:BOOL=OFF\n");
    CHECK_FALSE(pulp::cli::build_dir_is_dev_shared(dir));
    write_cache(dir, "PULP_DEV_SHARED_UI:BOOL=ON\nPULP_ENABLE_ARA:BOOL=ON\n");
    CHECK(pulp::cli::build_dir_is_dev_shared(dir));
    // A dev-shared dir is not a PULP_DEV_BUILD dir: --allow-dev-build is
    // about the latter only.
    CHECK_FALSE(pulp::cli::build_dir_is_dev_build(dir));

    const auto script = pulp::cli::ship_build_initial_cache(
        "PULP_DEV_SHARED_UI:BOOL=ON\nPULP_ENABLE_ARA:BOOL=ON\n");
    auto has = [&](const std::string& line) { return script.find(line) != std::string::npos; };
    CHECK(has("set(PULP_DEV_SHARED_UI OFF CACHE BOOL"));
    CHECK_FALSE(has("PULP_DEV_SHARED_UI [==["));
    CHECK(has("set(PULP_ENABLE_ARA [==[ON]==] CACHE BOOL"));
}
