#include "../tools/cli/configure_defaults.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using pulp::cli::ConfigureDefaults;
using pulp::cli::cmake_cache_value;
using pulp::cli::configure_default_args;

namespace {
bool has(const std::vector<std::string>& args, const std::string& value) {
    return std::find(args.begin(), args.end(), value) != args.end();
}
}  // namespace

TEST_CASE("fresh source configure pins Ninja, Release, and examples off",
          "[cli][configure-defaults]") {
    ConfigureDefaults in;
    in.ninja_available = true;
    in.source_checkout = true;
    REQUIRE(configure_default_args(in)
            == std::vector<std::string>{"-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                                        "-DPULP_BUILD_EXAMPLES=OFF"});
}

TEST_CASE("fresh configure without ninja leaves the generator to CMake",
          "[cli][configure-defaults]") {
    ConfigureDefaults in;
    in.source_checkout = true;
    const auto args = configure_default_args(in);
    REQUIRE_FALSE(has(args, "-G"));
    REQUIRE(has(args, "-DCMAKE_BUILD_TYPE=Release"));
}

TEST_CASE("PULP_BUILD_TYPE overrides the Release default; empty means unset",
          "[cli][configure-defaults]") {
    ConfigureDefaults in;
    in.build_type_env = "Debug";
    REQUIRE(has(configure_default_args(in), "-DCMAKE_BUILD_TYPE=Debug"));
    REQUIRE_FALSE(has(configure_default_args(in), "-DCMAKE_BUILD_TYPE=Release"));
    in.build_type_env = "";
    REQUIRE(has(configure_default_args(in), "-DCMAKE_BUILD_TYPE=Release"));
}

TEST_CASE("an existing cache keeps its generator, build type, and examples",
          "[cli][configure-defaults]") {
    ConfigureDefaults in;
    in.existing_cache = "CMAKE_GENERATOR:INTERNAL=Unix Makefiles\n"
                        "CMAKE_BUILD_TYPE:STRING=Debug\n"
                        "PULP_BUILD_EXAMPLES:BOOL=ON\n";
    in.ninja_available = true;
    in.source_checkout = true;
    REQUIRE(configure_default_args(in).empty());
}

TEST_CASE("an existing cache with an empty build type is filled with Release",
          "[cli][configure-defaults]") {
    ConfigureDefaults in;
    in.existing_cache = "CMAKE_GENERATOR:INTERNAL=Unix Makefiles\r\nCMAKE_BUILD_TYPE:STRING=\r\n";
    in.ninja_available = true;
    in.source_checkout = true;
    REQUIRE(configure_default_args(in) == std::vector<std::string>{"-DCMAKE_BUILD_TYPE=Release"});
}

TEST_CASE("--examples opts in; standalone projects never get the option",
          "[cli][configure-defaults]") {
    ConfigureDefaults in;
    in.existing_cache = "PULP_BUILD_EXAMPLES:BOOL=OFF\nCMAKE_BUILD_TYPE:STRING=Release\n";
    in.examples = true;
    in.source_checkout = true;
    REQUIRE(configure_default_args(in) == std::vector<std::string>{"-DPULP_BUILD_EXAMPLES=ON"});

    ConfigureDefaults standalone;
    standalone.ninja_available = true;
    standalone.examples = true;
    for (const auto& arg : configure_default_args(standalone)) {
        REQUIRE(arg.rfind("-DPULP_BUILD_EXAMPLES", 0) != 0);
    }
}

TEST_CASE("cmake_cache_value reads typed entries", "[cli][configure-defaults]") {
    const std::string cache = "// comment\nCMAKE_BUILD_TYPE:STRING=Release\nFOO:BOOL=ON";
    REQUIRE(cmake_cache_value(cache, "CMAKE_BUILD_TYPE") == "Release");
    REQUIRE(cmake_cache_value(cache, "FOO") == "ON");
    REQUIRE_FALSE(cmake_cache_value(cache, "MISSING").has_value());
    REQUIRE_FALSE(cmake_cache_value(cache, "CMAKE_BUILD").has_value());
}
