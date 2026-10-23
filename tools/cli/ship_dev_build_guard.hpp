// Ship-time PULP_DEV_BUILD guard.
//
// A build configured with PULP_DEV_BUILD=ON compiles development-only paths
// in: the absolute on-disk UI_SCRIPT path (and theme / asset-root paths) that a
// hot-reloading editor reads so an edit shows without a rebuild. A shipped
// artifact must carry only the embedded script bytes, so `pulp ship` reads the
// build dir's CMakeCache.txt and refuses to sign or package a dev build.
//
// Header-only + dependency-light on purpose, like ship_tracing_guard.hpp: it is
// unit-tested directly against fixture caches (test/test_ship_dev_build_guard.cpp).
#pragma once

#include "configure_defaults.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::cli {

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
// unrecorded option is not a dev build (nothing to refuse).
inline bool build_dir_is_dev_build(const std::filesystem::path& build_dir) {
    const auto value = dev_build_cache_value(build_dir);
    return value && cmake_cache_flag_on(*value);
}

} // namespace pulp::cli
