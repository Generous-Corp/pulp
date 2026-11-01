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
// The shippable configure takes over EVERY entry the developer configured
// (formats, SDK paths, compiler, sysroot, feature switches) by writing them to
// an initial-cache script the configure loads with `cmake -C`; only the
// options the release profile enforces (PULP_PROFILE_ENFORCED_OPTIONS in
// tools/cmake/PulpProfiles.cmake, read here from the generated
// build_profile_gen.h) and the dev-shared runtime switch are dropped and set
// OFF, so the drop list cannot drift from what PULP_PROFILE=release refuses.
// After the build, the set of plug-in artifacts
// under build-ship is compared with the dev build's, so a format that silently
// fell out of the configure is refused rather than shipped without it.
//
// Header-only + dependency-light on purpose, like ship_tracing_guard.hpp: it is
// unit-tested directly against fixture caches (test/test_ship_dev_build_guard.cpp).
#pragma once

#include "build_profile_gen.h"
#include "configure_defaults.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace pulp::cli {

// The build directory `pulp ship` configures and builds for a dev-build project.
inline constexpr std::string_view kShipBuildDirName = "build-ship";
// The initial-cache script written into that directory; its content is also
// the record of what the last shippable configure carried.
inline constexpr std::string_view kShipInitialCacheName = "pulp-ship-initial-cache.cmake";

inline std::filesystem::path ship_build_dir(const std::filesystem::path& root) {
    return root / std::string(kShipBuildDirName);
}

inline std::filesystem::path ship_initial_cache_path(const std::filesystem::path& ship_dir) {
    return ship_dir / std::string(kShipInitialCacheName);
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

// True when `build_dir` links plug-ins against the development-only shared
// Pulp runtime (PULP_DEV_SHARED_UI=ON). Unlike a PULP_DEV_BUILD dir it can
// never ship as it is, so --allow-dev-build does not keep it.
inline bool build_dir_is_dev_shared(const std::filesystem::path& build_dir) {
    const auto cache = read_cmake_cache(build_dir);
    if (!cache)
        return false;
    const auto value = cmake_cache_value(*cache, kDevSharedUiOption);
    return value && cmake_cache_flag_on(*value);
}

// One `NAME:TYPE=VALUE` line of a CMakeCache.txt.
struct CmakeCacheEntry {
    std::string name;
    std::string type;
    std::string value;
};

// Every cache entry in `cache`, in file order. Comment (`//`, `#`) and blank
// lines are skipped; a line without `:TYPE=` is not an entry.
inline std::vector<CmakeCacheEntry> cmake_cache_entries(std::string_view cache) {
    std::vector<CmakeCacheEntry> entries;
    size_t pos = 0;
    while (pos < cache.size()) {
        size_t end = cache.find('\n', pos);
        if (end == std::string_view::npos)
            end = cache.size();
        std::string_view line = cache.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty() || line.rfind("//", 0) == 0 || line[0] == '#')
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            continue;
        const std::string_view key = line.substr(0, eq);
        const size_t colon = key.rfind(':');
        if (colon == std::string_view::npos || colon == 0)
            continue;
        entries.push_back({std::string(key.substr(0, colon)), std::string(key.substr(colon + 1)),
                           std::string(line.substr(eq + 1))});
    }
    return entries;
}

// True when `name` is an option the release profile forces OFF.
constexpr bool profile_enforces(std::string_view name) {
    for (const auto option : kProfileEnforcedOptions)
        if (option == name)
            return true;
    return false;
}
// The guard's whole premise: the option it exists for is one the release
// profile owns, so the generated list must carry it.
static_assert(profile_enforces(kDevBuildCacheName),
              "PULP_PROFILE_ENFORCED_OPTIONS must include PULP_DEV_BUILD");

// Dev-only switches the shippable configure must not inherit beyond the
// release profile's enforced list: the dev-shared runtime is a link-layout
// choice the profile does not govern. Every dropped entry is set OFF explicitly
// in the initial cache.
inline constexpr std::string_view kShipBuildDroppedEntries[] = {
    kDevSharedUiOption,
};

// Entries that describe the dev build DIRECTORY rather than its configuration.
inline constexpr std::string_view kShipBuildDirectoryEntries[] = {
    "CMAKE_CACHEFILE_DIR",       "CMAKE_HOME_DIRECTORY",      "CMAKE_CACHE_MAJOR_VERSION",
    "CMAKE_CACHE_MINOR_VERSION", "CMAKE_CACHE_PATCH_VERSION",
};

inline bool ship_build_carries_entry(const CmakeCacheEntry& entry) {
    if (entry.type == "INTERNAL" || entry.type == "STATIC")
        return false;
    if (profile_enforces(entry.name))
        return false;
    for (const auto dropped : kShipBuildDroppedEntries)
        if (entry.name == dropped)
            return false;
    for (const auto directory : kShipBuildDirectoryEntries)
        if (entry.name == directory)
            return false;
    // Per-directory bookkeeping CMake writes for the dev tree.
    if (entry.name.rfind("CMAKE_CACHEFILE", 0) == 0)
        return false;
    return true;
}

// The initial-cache script (`cmake -C`) that configures the shippable build
// from `dev_cache`, the dev build dir's CMakeCache.txt text:
// - every non-INTERNAL, non-STATIC entry the developer configured, verbatim
//   (formats, SDK paths, compiler, sysroot, feature switches, build type);
// - PULP_PROFILE pinned to `release`, whether the dev build recorded the
//   option (rewritten) or predates it (added): PulpProfiles.cmake would
//   otherwise default the sibling to `dev`, and its stamp would record a
//   build_profile that official provenance minting rejects;
// - every option the release profile enforces (PULP_DEV_BUILD, tests,
//   examples, audio probes, the exec bridge, ...) and PULP_DEV_SHARED_UI
//   dropped and set OFF explicitly, which is the point;
// - CMAKE_BUILD_TYPE pinned to Release when the dev build recorded none.
// The generator is INTERNAL and cannot come from an initial cache; see
// ship_build_configure_args(). Pure over the cache text so it can be tested
// without configuring anything.
inline std::string ship_build_initial_cache(std::string_view dev_cache) {
    std::string script =
        "# Written by `pulp ship`: the dev build's configuration, minus the dev-only switches.\n"
        "# Regenerated from build/CMakeCache.txt whenever it changes; do not edit.\n";
    bool has_build_type = false;
    bool has_profile = false;
    for (const auto& entry : cmake_cache_entries(dev_cache)) {
        if (!ship_build_carries_entry(entry))
            continue;
        std::string value = entry.value;
        if (entry.name == "PULP_PROFILE") {
            value = "release";
            has_profile = true;
        }
        if (entry.name == "CMAKE_BUILD_TYPE") {
            if (value.empty())
                value = std::string(kDefaultBuildType);
            has_build_type = true;
        }
        const std::string type = entry.type == "UNINITIALIZED" ? "STRING" : entry.type;
        script += "set(" + entry.name + " [==[" + value + "]==] CACHE " + type +
                  " \"carried from the dev build by pulp ship\")\n";
    }
    if (!has_build_type)
        script += "set(CMAKE_BUILD_TYPE [==[" + std::string(kDefaultBuildType) +
                  "]==] CACHE STRING \"pinned by pulp ship\")\n";
    if (!has_profile)
        script += "set(PULP_PROFILE [==[release]==] CACHE STRING \"pinned by pulp ship\")\n";
    for (const auto option : kProfileEnforcedOptions)
        script += "set(" + std::string(option) +
                  " OFF CACHE BOOL \"OFF under the release profile; set by pulp ship\")\n";
    for (const auto option : kShipBuildDroppedEntries)
        script += "set(" + std::string(option) +
                  " OFF CACHE BOOL \"the shippable build links plug-ins statically\")\n";
    return script;
}

// The `cmake -S <root> -B <build-ship>` arguments: the dev build's generator
// (CMake refuses to take one from an initial cache) and the initial-cache
// script written from ship_build_initial_cache().
inline std::vector<std::string>
ship_build_configure_args(std::string_view dev_cache, const std::filesystem::path& initial_cache) {
    std::vector<std::string> args;
    if (const auto generator = cmake_cache_value(dev_cache, "CMAKE_GENERATOR");
        generator && !generator->empty()) {
        args.emplace_back("-G");
        args.push_back(*generator);
    }
    args.emplace_back("-C");
    args.push_back(initial_cache.string());
    return args;
}

// True when `ship_dir` holds a completed configure: a cache that records its
// generator. A cache left behind by a failed configure has none.
inline bool ship_build_is_configured(const std::filesystem::path& ship_dir) {
    const auto cache = read_cmake_cache(ship_dir);
    if (!cache)
        return false;
    const auto generator = cmake_cache_value(*cache, "CMAKE_GENERATOR");
    return generator && !generator->empty();
}

// Whether the shippable build must be (re)configured before building:
// no completed configure yet, the dev cache changed after the ship cache was
// written, or the entries to carry differ from what the last configure carried.
inline bool ship_build_needs_configure(const std::filesystem::path& dev_dir,
                                       const std::filesystem::path& ship_dir,
                                       std::string_view initial_cache_text) {
    if (!ship_build_is_configured(ship_dir))
        return true;
    std::error_code ec;
    const auto dev_time = std::filesystem::last_write_time(dev_dir / "CMakeCache.txt", ec);
    if (ec)
        return true;
    const auto ship_time = std::filesystem::last_write_time(ship_dir / "CMakeCache.txt", ec);
    if (ec || dev_time > ship_time)
        return true;
    std::ifstream previous(ship_initial_cache_path(ship_dir), std::ios::binary);
    if (!previous)
        return true;
    std::ostringstream text;
    text << previous.rdbuf();
    return text.str() != initial_cache_text;
}

// The plug-in artifacts a build directory holds, as `<format dir>/<bundle>`:
// the bundles `pulp ship sign|package` would pick up from it.
inline std::set<std::string> plugin_artifact_set(const std::filesystem::path& build_dir) {
    std::set<std::string> artifacts;
    std::error_code ec;
    for (const char* dir_name : {"VST3", "CLAP", "AU", "AAX", "LV2", "Standalone"}) {
        const auto dir = build_dir / dir_name;
        if (!std::filesystem::is_directory(dir, ec) || ec)
            continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            const auto ext = entry.path().extension().string();
            if (ext == ".vst3" || ext == ".clap" || ext == ".component" || ext == ".aaxplugin" ||
                ext == ".lv2" || ext == ".app")
                artifacts.insert(std::string(dir_name) + "/" + entry.path().filename().string());
        }
    }
    return artifacts;
}

// Empty when `ship` holds every plug-in artifact `dev` holds; otherwise the
// reason the shippable build must not be shipped: the artifacts the dev build
// has that build-ship does not. Artifacts only build-ship has are fine (the
// dev build may have been built for a subset of targets).
inline std::string plugin_artifact_set_mismatch(const std::set<std::string>& dev,
                                                const std::set<std::string>& ship) {
    std::string missing;
    for (const auto& artifact : dev) {
        if (ship.count(artifact))
            continue;
        missing += (missing.empty() ? "" : ", ") + artifact;
    }
    if (missing.empty())
        return {};
    return "the shippable build lacks plug-in artifact(s) the dev build has: " + missing;
}

} // namespace pulp::cli
