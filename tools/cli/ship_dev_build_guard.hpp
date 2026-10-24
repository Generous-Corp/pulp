// Ship-time handling of a PULP_DEV_BUILD build directory.
//
// A build configured with PULP_DEV_BUILD=ON records development-only paths:
// the absolute on-disk UI_SCRIPT path (and theme / asset-root paths) that a
// hot-reloading editor reads so an edit shows without a rebuild. A shipped
// artifact must carry only the embedded editor, so `pulp ship` does not ship
// such a build directory. It does not refuse either: the `pulp` CLI turns the
// option on for every developer-loop configure, so a plain `pulp build` dir is
// a dev build and refusing would break create -> build -> ship. Instead it
// configures a sibling `build-ship` directory from the dev cache with
// PULP_DEV_BUILD=OFF, builds it, and ships from there. `--allow-dev-build`
// ships the dev directory as it is.
//
// Header-only + dependency-light on purpose, like ship_tracing_guard.hpp: it is
// unit-tested directly against fixture caches (test/test_ship_dev_build_guard.cpp).
#pragma once

#include "configure_defaults.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::cli {

// The build directory `pulp ship` configures and builds for a dev-build project.
inline constexpr std::string_view kShipBuildDirName = "build-ship";

inline std::filesystem::path ship_build_dir(const std::filesystem::path& root) {
    return root / std::string(kShipBuildDirName);
}

// CMake's truthy spellings for a BOOL cache entry.
inline bool cmake_cache_flag_on(std::string_view value) {
    std::string upper;
    upper.reserve(value.size());
    for (char c : value)
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return upper == "ON" || upper == "TRUE" || upper == "YES" || upper == "Y" || upper == "1";
}

// The recorded PULP_DEV_BUILD value of `build_dir`, or nullopt when the dir has
// no cache or the cache never recorded the option.
inline std::optional<std::string> dev_build_cache_value(const std::filesystem::path& build_dir) {
    const auto cache = read_cmake_cache(build_dir);
    if (!cache)
        return std::nullopt;
    return cmake_cache_value(*cache, kDevBuildCacheName);
}

// True when `build_dir` was configured as a dev build. A missing cache or an
// unrecorded option is not a dev build (nothing to redirect).
inline bool build_dir_is_dev_build(const std::filesystem::path& build_dir) {
    const auto value = dev_build_cache_value(build_dir);
    return value && cmake_cache_flag_on(*value);
}

// Cache entries the shippable configure takes over from the dev build's cache,
// so both directories build the same product against the same SDK, toolchain,
// generator and feature switches. Entries the dev cache does not record are
// not passed.
inline constexpr std::string_view kShipBuildCarriedCacheEntries[] = {
    "CMAKE_PREFIX_PATH",
    "Pulp_DIR",
    "CMAKE_TOOLCHAIN_FILE",
    "CMAKE_OSX_ARCHITECTURES",
    "CMAKE_OSX_DEPLOYMENT_TARGET",
    "PULP_JS_ENGINE",
    "PULP_MACOS_ARCH",
    "PULP_BUILD_EXAMPLES",
    "PULP_REQUIRE_CHECKOUT_DEPENDENCIES",
    "PULP_ALLOW_DEBUG_SDK",
};

// The `cmake -S <root> -B <build-ship>` arguments that configure the shippable
// build from `dev_cache` (the dev build dir's CMakeCache.txt text):
// - the dev build's generator and build type (Release when it has none);
// - the carried entries above, verbatim;
// - PULP_BUILD_TESTS=OFF when the dev cache records the option (a source
//   checkout), since a shipping build has no use for the test tree;
// - PULP_DEV_BUILD=OFF, which is the point.
// Pure over the cache text so it can be tested without configuring anything.
inline std::vector<std::string> ship_build_configure_args(std::string_view dev_cache) {
    std::vector<std::string> args;
    if (const auto generator = cmake_cache_value(dev_cache, "CMAKE_GENERATOR");
        generator && !generator->empty()) {
        args.emplace_back("-G");
        args.push_back(*generator);
    }
    auto build_type = cmake_cache_value(dev_cache, "CMAKE_BUILD_TYPE").value_or("");
    if (build_type.empty())
        build_type = std::string(kDefaultBuildType);
    args.push_back("-DCMAKE_BUILD_TYPE=" + build_type);
    for (const auto name : kShipBuildCarriedCacheEntries) {
        if (const auto value = cmake_cache_value(dev_cache, name); value && !value->empty())
            args.push_back("-D" + std::string(name) + "=" + *value);
    }
    if (cmake_cache_value(dev_cache, "PULP_BUILD_TESTS").has_value())
        args.emplace_back("-DPULP_BUILD_TESTS=OFF");
    args.emplace_back(std::string("-D") + std::string(kDevBuildCacheName) + "=OFF");
    return args;
}

} // namespace pulp::cli
