#pragma once

// Generator, build-type, and examples defaults for the CLI's CMake configure.
//
// The decision is a pure function over values the caller gathers (cache text,
// ninja availability, PULP_BUILD_TYPE) so it can be tested without spawning
// CMake. The Rust CLI's `configure_default_args` applies the same rules; keep
// the two in step.

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <sstream>
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
//   the generator of an existing cache; migrate_stale_build_dir moves a
//   Makefiles cache aside first so the dir configures as fresh.
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

// Text of `build_dir`'s CMakeCache.txt, or nullopt when it has none.
inline std::optional<std::string> read_cmake_cache(const std::filesystem::path& build_dir) {
    std::ifstream in(build_dir / "CMakeCache.txt", std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// True when `build_dir`'s cache records PULP_BUILD_EXAMPLES=OFF.
inline bool build_dir_has_examples_off(const std::filesystem::path& build_dir) {
    const auto cache = read_cmake_cache(build_dir);
    return cache && cmake_cache_value(*cache, "PULP_BUILD_EXAMPLES") == "OFF";
}

// configure_default_args for a real build dir: reads its cache and
// PULP_BUILD_TYPE. The caller decides whether ninja is usable.
inline std::vector<std::string> configure_default_args_for(
    const std::filesystem::path& build_dir, bool source_checkout, bool examples,
    bool ninja_available) {
    ConfigureDefaults inputs;
    inputs.existing_cache = read_cmake_cache(build_dir);
    inputs.ninja_available = ninja_available;
    if (const char* bt = std::getenv("PULP_BUILD_TYPE")) inputs.build_type_env = std::string(bt);
    inputs.examples = examples;
    inputs.source_checkout = source_checkout;
    return configure_default_args(inputs);
}

// Opt-out that keeps an existing build dir's configuration as it is.
inline constexpr std::string_view kKeepBuildConfigEnv = "PULP_KEEP_BUILD_CONFIG";
// Where migrate_stale_build_dir moves an old cache and CMakeFiles.
inline constexpr std::string_view kPreMigrationDir = ".pulp-pre-migration";

inline bool equals_ignore_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Why an existing build dir is slower than the configuration a fresh `pulp
// build` pins, as short labels ("Unix Makefiles", "Debug", "examples ON").
// Empty when the dir already matches, or when the cache is not a real
// configure (no CMAKE_GENERATOR). The Rust CLI's `stale_build_config_reasons`
// applies the same rules.
// - Generator: a Makefiles generator when ninja is available. IDE generators
//   (Xcode, Visual Studio) are a deliberate choice and stay.
// - Build type (single-config generators): an empty or Debug build type that
//   PULP_BUILD_TYPE does not ask for.
// - Examples (source checkout only): examples ON without `--examples`.
inline std::vector<std::string> stale_build_config_reasons(const ConfigureDefaults& in) {
    std::vector<std::string> reasons;
    if (!in.existing_cache)
        return reasons;
    const auto generator = cmake_cache_value(*in.existing_cache, "CMAKE_GENERATOR");
    if (!generator)
        return reasons;
    if (in.ninja_available && generator->find("Makefiles") != std::string::npos) {
        reasons.push_back(*generator);
    }
    const bool multi_config = *generator == "Xcode" || generator->rfind("Visual Studio", 0) == 0 ||
                              (generator->size() >= 12 &&
                               generator->compare(generator->size() - 12, 12, "Multi-Config") == 0);
    if (!multi_config) {
        const std::string cached =
            cmake_cache_value(*in.existing_cache, "CMAKE_BUILD_TYPE").value_or("");
        std::string explicit_type;
        if (in.build_type_env) {
            const auto b = in.build_type_env->find_first_not_of(" \t");
            const auto e = in.build_type_env->find_last_not_of(" \t");
            if (b != std::string::npos)
                explicit_type = in.build_type_env->substr(b, e - b + 1);
        }
        const bool requested = !explicit_type.empty() && equals_ignore_case(explicit_type, cached);
        if ((cached.empty() || equals_ignore_case(cached, "Debug")) && !requested) {
            reasons.push_back(cached.empty() ? "no build type" : cached);
        }
    }
    if (in.source_checkout && !in.examples) {
        std::string examples =
            cmake_cache_value(*in.existing_cache, "PULP_BUILD_EXAMPLES").value_or("");
        for (auto& c : examples)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (examples == "ON" || examples == "1" || examples == "TRUE" || examples == "YES" ||
            examples == "Y") {
            reasons.emplace_back("examples ON");
        }
    }
    return reasons;
}

// Moves a slow build dir's CMakeCache.txt and CMakeFiles into kPreMigrationDir
// so the next configure starts fresh with the pinned defaults, and prints one
// line naming what changed. Returns whether it moved anything.
// PULP_KEEP_BUILD_CONFIG=1 keeps the old configuration. An existing dir never
// switches generator in place (CMake refuses), so a dir an older CLI created
// keeps Makefiles, Debug and examples forever unless its cache moves aside.
inline bool migrate_stale_build_dir(const std::filesystem::path& build_dir,
                                    const ConfigureDefaults& in, std::ostream& out) {
    const auto reasons = stale_build_config_reasons(in);
    if (reasons.empty())
        return false;
    if (const char* keep = std::getenv(std::string(kKeepBuildConfigEnv).c_str())) {
        const std::string_view v(keep);
        if (v.find_first_not_of(" \t") != std::string_view::npos && v != "0")
            return false;
    }
    namespace fs = std::filesystem;
    const fs::path aside = build_dir / kPreMigrationDir;
    std::error_code ec;
    fs::remove_all(aside, ec);
    fs::create_directories(aside, ec);
    if (ec)
        return false;
    for (const char* name : {"CMakeCache.txt", "CMakeFiles"}) {
        if (fs::exists(build_dir / name))
            fs::rename(build_dir / name, aside / name, ec);
        if (ec)
            return false;
    }
    std::string joined;
    for (const auto& r : reasons)
        joined += (joined.empty() ? "" : ", ") + r;
    out << "Reconfiguring " << build_dir.string() << " with the pulp defaults (was: " << joined
        << "); old cache moved to " << aside.string() << " (" << kKeepBuildConfigEnv
        << "=1 keeps a build dir as it is)\n";
    return true;
}

// migrate_stale_build_dir for a real build dir: reads its cache and
// PULP_BUILD_TYPE. The caller decides whether ninja is usable.
inline bool migrate_stale_build_dir_for(const std::filesystem::path& build_dir,
                                        bool source_checkout, bool examples, bool ninja_available,
                                        std::ostream& out) {
    ConfigureDefaults inputs;
    inputs.existing_cache = read_cmake_cache(build_dir);
    inputs.ninja_available = ninja_available;
    if (const char* bt = std::getenv("PULP_BUILD_TYPE"))
        inputs.build_type_env = std::string(bt);
    inputs.examples = examples;
    inputs.source_checkout = source_checkout;
    return migrate_stale_build_dir(build_dir, inputs, out);
}

}  // namespace pulp::cli
