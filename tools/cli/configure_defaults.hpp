#pragma once

// Generator, build-type, and examples defaults for the CLI's CMake configure.
//
// The decision is a pure function over values the caller gathers (cache text,
// ninja availability, PULP_BUILD_TYPE) so it can be tested without spawning
// CMake. The Rust CLI's `configure_default_args` applies the same rules; keep
// the two in step.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::cli {

// Build type a configure pins when PULP_BUILD_TYPE is unset. An unset
// CMAKE_BUILD_TYPE under a single-config generator compiles with no
// optimization and no NDEBUG.
inline constexpr std::string_view kDefaultBuildType = "Release";

struct ConfigureDefaults {
    // Text of the build dir's CMakeCache.txt; nullopt for a fresh build dir.
    std::optional<std::string> existing_cache;
    bool ninja_available = false;
    // Value of PULP_BUILD_TYPE, if set.
    std::optional<std::string> build_type_env;
    // `--examples` was passed.
    bool examples = false;
    // The project is the Pulp source checkout (not a standalone product).
    bool source_checkout = false;
};

// Value of `NAME:TYPE=value` in a CMakeCache.txt, if present.
inline std::optional<std::string> cmake_cache_value(std::string_view cache,
                                                    std::string_view name) {
    size_t pos = 0;
    while (pos <= cache.size()) {
        size_t end = cache.find('\n', pos);
        if (end == std::string_view::npos) end = cache.size();
        std::string_view line = cache.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const size_t eq = line.find('=');
        if (eq != std::string_view::npos) {
            std::string_view key = line.substr(0, eq);
            const size_t colon = key.find(':');
            if (colon != std::string_view::npos && key.substr(0, colon) == name) {
                std::string_view value = line.substr(eq + 1);
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                    value.remove_suffix(1);
                }
                return std::string(value);
            }
        }
        if (end == cache.size()) break;
        pos = end + 1;
    }
    return std::nullopt;
}

// - Generator: `-G Ninja` only for a fresh build dir. CMake refuses to switch
//   the generator of an existing cache, so an existing dir keeps its own.
// - Build type: an explicit PULP_BUILD_TYPE always applies; otherwise
//   kDefaultBuildType fills a fresh dir or a cache whose build type is empty,
//   and an existing non-empty build type is left alone.
// - Examples (source checkout only): `--examples` turns them on; a fresh dev
//   configure turns them off; an existing cache keeps its setting.
inline std::vector<std::string> configure_default_args(const ConfigureDefaults& in) {
    std::vector<std::string> args;
    const bool fresh = !in.existing_cache.has_value();
    if (fresh && in.ninja_available) {
        args.emplace_back("-G");
        args.emplace_back("Ninja");
    }
    std::optional<std::string> explicit_type;
    if (in.build_type_env && in.build_type_env->find_first_not_of(" \t") != std::string::npos) {
        explicit_type = *in.build_type_env;
    }
    std::optional<std::string> cached_type;
    if (in.existing_cache) {
        cached_type = cmake_cache_value(*in.existing_cache, "CMAKE_BUILD_TYPE");
        if (cached_type && cached_type->empty()) cached_type.reset();
    }
    if (explicit_type) {
        args.push_back("-DCMAKE_BUILD_TYPE=" + *explicit_type);
    } else if (!cached_type) {
        args.push_back("-DCMAKE_BUILD_TYPE=" + std::string(kDefaultBuildType));
    }
    if (in.source_checkout) {
        if (in.examples) {
            args.emplace_back("-DPULP_BUILD_EXAMPLES=ON");
        } else if (fresh) {
            args.emplace_back("-DPULP_BUILD_EXAMPLES=OFF");
        }
    }
    return args;
}

}  // namespace pulp::cli
