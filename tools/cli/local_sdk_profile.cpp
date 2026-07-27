// local_sdk_profile.cpp — pure policy and validation for forge-dev SDK installs.

#include "local_sdk_profile.hpp"

#include "json_writer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace pulp::cli::local_sdk {
namespace {

std::string trim_copy(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void require(bool condition, std::string message, Validation& out) {
    if (!condition)
        out.errors.push_back(std::move(message));
}

bool cache_true(const std::map<std::string, std::string>& cache, const std::string& key) {
    auto it = cache.find(key);
    if (it == cache.end())
        return false;
    auto value = it->second;
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value == "ON" || value == "TRUE" || value == "1";
}

std::string normalized_identity(const Identity& i) {
    std::ostringstream out;
    out << "profile=forge-dev\n"
        << "revision=" << kForgeProfileRevision << "\n"
        << "sdk_version=" << i.sdk_version << "\n"
        << "source_git_sha=" << i.source_git_sha << "\n"
        << "platform=" << i.platform << "\n"
        << "cmake_version=" << i.cmake_version << "\n"
        << "generator=" << i.generator << "\n"
        << "compiler=" << i.compiler << "\n"
        << "macos_sdk=" << i.macos_sdk << "\n"
        << "deployment_target=" << i.deployment_target << "\n"
        << "skia=" << i.skia_identity << "\n"
        << "vst3=" << i.vst3_git_sha << "\n"
        << "ausdk=" << i.ausdk_git_sha << "\n";
    return out.str();
}

// This is an address, not a security checksum. FNV-1a keeps the standalone
// policy module dependency-free; the full unhashed inputs remain in provenance.
std::string fnv1a_64_hex(const std::string& text) {
    std::uint64_t value = 14695981039346656037ull;
    for (unsigned char byte : text) {
        value ^= byte;
        value *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

Validation validate_common(const fs::path& prefix, const Identity& expected) {
    Validation out;
    const auto config = prefix / "lib" / "cmake" / "Pulp" / "PulpConfig.cmake";
    const auto targets = prefix / "lib" / "cmake" / "Pulp" / "PulpTargets.cmake";
    const auto build_type = trim_copy(read_file(prefix / "sdk_build_type.txt"));
    const auto version = trim_copy(read_file(prefix / "version.txt"));
    const auto build_info = read_file(prefix / "include" / "pulp" / "runtime" / "build_info.hpp");

    require(fs::is_regular_file(config), "PulpConfig.cmake is missing", out);
    require(fs::is_regular_file(targets), "PulpTargets.cmake is missing", out);
    require(build_type == "Release", "SDK build type is not Release", out);
    require(version == expected.sdk_version, "SDK version marker does not match", out);
    require(build_info.find("kBuildType   = \"Release\"") != std::string::npos,
            "installed build_info.hpp is not Release", out);
    require(build_info.find("kGitDirty                = false") != std::string::npos,
            "installed build_info.hpp reports a dirty source tree", out);
    require(!expected.source_git_sha.empty() &&
                build_info.find(expected.source_git_sha.substr(0, 7)) != std::string::npos,
            "installed build_info.hpp does not match the source commit", out);

    const auto targets_text = read_file(targets);
    require(targets_text.find("Pulp::clap") != std::string::npos,
            "installed SDK does not expose CLAP", out);
    require(targets_text.find("Pulp::vst3-sdk") != std::string::npos,
            "installed SDK does not expose VST3", out);
    require(targets_text.find("Pulp::ausdk") != std::string::npos,
            "installed SDK does not expose Audio Unit", out);
    require(targets_text.find("Pulp::render") != std::string::npos,
            "installed SDK does not expose the GPU runtime", out);
    require(targets_text.find("Pulp::standalone") != std::string::npos,
            "installed SDK does not expose the standalone application runtime", out);
    require(targets_text.find("Pulp::format") != std::string::npos,
            "installed SDK does not expose the plugin format runtime", out);

    out.ok = out.errors.empty();
    return out;
}

} // namespace

bool operator==(const Identity& lhs, const Identity& rhs) {
    return lhs.sdk_version == rhs.sdk_version && lhs.source_git_sha == rhs.source_git_sha &&
           lhs.platform == rhs.platform && lhs.cmake_version == rhs.cmake_version &&
           lhs.generator == rhs.generator && lhs.compiler == rhs.compiler &&
           lhs.macos_sdk == rhs.macos_sdk &&
           lhs.deployment_target == rhs.deployment_target &&
           lhs.skia_identity == rhs.skia_identity && lhs.vst3_git_sha == rhs.vst3_git_sha &&
           lhs.ausdk_git_sha == rhs.ausdk_git_sha;
}

InstallRequest parse_install_arguments(const std::vector<std::string>& args,
                                       const std::string& default_version) {
    InstallRequest out;
    out.version = default_version;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--local") {
            out.from_local = true;
        } else if (arg == "--print-path") {
            out.print_path = true;
        } else if (arg == "--profile" || arg == "--version") {
            if (i + 1 >= args.size() || (!args[i + 1].empty() && args[i + 1].front() == '-')) {
                out.error = arg + " requires a value";
                return out;
            }
            if (arg == "--profile")
                out.profile = args[++i];
            else {
                out.version = args[++i];
                out.version_explicit = true;
            }
        } else if (!arg.empty() && arg.front() == '-') {
            out.error = "unknown flag: " + arg;
            return out;
        } else {
            out.error = "unknown argument: " + arg;
            return out;
        }
    }
    if (!out.profile.empty() && out.profile != kForgeProfileName) {
        out.error = "unknown local SDK profile: " + out.profile;
        return out;
    }
    if ((!out.profile.empty() || out.print_path) && !out.from_local) {
        out.error = "--profile and --print-path require --local";
        return out;
    }
    if (out.print_path && out.profile != kForgeProfileName) {
        out.error = "--print-path requires --profile forge-dev";
        return out;
    }
    if (out.version_explicit && out.profile == kForgeProfileName) {
        out.error = "--version cannot be combined with --profile forge-dev; "
                    "the version comes from the selected checkout";
        return out;
    }
    out.ok = true;
    return out;
}

std::string input_fingerprint(const Identity& identity) {
    return fnv1a_64_hex(normalized_identity(identity)).substr(0, 12);
}

Paths profile_paths(const fs::path& pulp_home, const Identity& identity) {
    Paths out;
    out.input_fingerprint = input_fingerprint(identity);
    out.install_prefix = pulp_home / "sdk-dev" / "forge-v1" / identity.platform /
                         identity.source_git_sha / out.input_fingerprint;
    out.build_dir = pulp_home / "sdk-build-dev" / "forge-v1" / identity.platform /
                    identity.source_git_sha / out.input_fingerprint;
    return out;
}

std::vector<std::string> configure_arguments(const fs::path& source, const fs::path& build,
                                             const fs::path& install_prefix,
                                             const Identity& identity) {
    return {"cmake",
            "-S",
            source.string(),
            "-B",
            build.string(),
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_C_COMPILER=/usr/bin/clang",
            "-DCMAKE_CXX_COMPILER=/usr/bin/clang++",
            "-DCMAKE_INSTALL_PREFIX=" + install_prefix.string(),
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=" + identity.deployment_target,
            "-DSKIA_DIR=" + (source / "external" / "skia-build").string(),
            "-DPULP_MACOS_ARCH=arm64",
            "-DPULP_ENABLE_GPU=ON",
            "-DPULP_REQUIRE_GPU_FOR_SDK=ON",
            "-DPULP_ENABLE_DESIGN_IMPORT=ON",
            "-DPULP_ENABLE_AUDIO_PROBES=OFF",
            "-DPULP_ENABLE_INSPECTOR=OFF",
            "-DPULP_BUILD_TESTS=OFF",
            "-DPULP_BUILD_EXAMPLES=OFF"};
}

std::map<std::string, std::string> parse_cmake_cache(const fs::path& cache_path) {
    std::map<std::string, std::string> result;
    std::ifstream in(cache_path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line.front() == '#' || line.rfind("//", 0) == 0)
            continue;
        const auto colon = line.find(':');
        const auto equals = line.find('=', colon == std::string::npos ? 0 : colon);
        if (colon == std::string::npos || equals == std::string::npos)
            continue;
        result[line.substr(0, colon)] = trim_copy(line.substr(equals + 1));
    }
    return result;
}

Validation validate_staged_install(const fs::path& prefix, const fs::path& build_dir,
                                   const Identity& expected, const fs::path& expected_skia_dir) {
    auto out = validate_common(prefix, expected);
    const auto cache = parse_cmake_cache(build_dir / "CMakeCache.txt");
    require(cache.find("CMAKE_BUILD_TYPE") != cache.end() &&
                cache.at("CMAKE_BUILD_TYPE") == "Release",
            "configured build type is not Release", out);
    require(cache.find("CMAKE_OSX_ARCHITECTURES") != cache.end() &&
                cache.at("CMAKE_OSX_ARCHITECTURES") == "arm64",
            "configured SDK architecture is not arm64", out);
    require(cache.find("CMAKE_OSX_DEPLOYMENT_TARGET") != cache.end() &&
                cache.at("CMAKE_OSX_DEPLOYMENT_TARGET") == expected.deployment_target,
            "configured macOS deployment target does not match the immutable profile", out);
    require(cache.find("SKIA_DIR") != cache.end() &&
                fs::path(cache.at("SKIA_DIR")).lexically_normal() ==
                    expected_skia_dir.lexically_normal(),
            "configured Skia directory does not match the immutable profile", out);
    require(cache_true(cache, "PULP_ENABLE_GPU"), "GPU support was not enabled", out);
    require(cache_true(cache, "PULP_HAS_SKIA"), "Skia was not resolved", out);
    require(cache_true(cache, "PULP_ENABLE_DESIGN_IMPORT"), "design import was not enabled", out);
    require(cache_true(cache, "PULP_HAS_VST3"), "VST3 SDK was not resolved", out);
    require(cache_true(cache, "PULP_HAS_AUSDK"), "AudioUnitSDK was not resolved", out);
    require(cache_true(cache, "PULP_HAS_CLAP"), "CLAP was not resolved", out);
    require(!cache_true(cache, "PULP_ENABLE_AUDIO_PROBES"),
            "audio probes must stay disabled in the SDK", out);
    require(!cache_true(cache, "PULP_ENABLE_INSPECTOR"),
            "the in-plugin inspector must stay disabled in the SDK", out);
    out.ok = out.errors.empty();
    return out;
}

Validation validate_published_install(const fs::path& prefix, const Identity& expected) {
    auto out = validate_common(prefix, expected);
    const auto provenance = read_file(prefix / "sdk-provenance.json");
    require(provenance == serialize_provenance(expected, input_fingerprint(expected)),
            "SDK provenance does not exactly match the requested immutable profile", out);
    out.ok = out.errors.empty();
    return out;
}

std::string serialize_provenance(const Identity& i, const std::string& fingerprint) {
    using pulp::cli::json_string;
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": " << json_string(kProvenanceSchema) << ",\n"
        << "  \"kind\": \"development\",\n"
        << "  \"profile\": " << json_string(kForgeProfileName) << ",\n"
        << "  \"profile_revision\": " << kForgeProfileRevision << ",\n"
        << "  \"distribution_eligible\": false,\n"
        << "  \"sdk_version\": " << json_string(i.sdk_version) << ",\n"
        << "  \"source_git_sha\": " << json_string(i.source_git_sha) << ",\n"
        << "  \"source_git_dirty\": false,\n"
        << "  \"platform\": " << json_string(i.platform) << ",\n"
        << "  \"architectures\": [\"arm64\"],\n"
        << "  \"build_type\": \"Release\",\n"
        << "  \"cmake_version\": " << json_string(i.cmake_version) << ",\n"
        << "  \"generator\": " << json_string(i.generator) << ",\n"
        << "  \"compiler\": " << json_string(i.compiler) << ",\n"
        << "  \"macos_sdk\": " << json_string(i.macos_sdk) << ",\n"
        << "  \"deployment_target\": " << json_string(i.deployment_target) << ",\n"
        << "  \"features\": {\"gpu\": true, \"design_import\": true, "
           "\"audio_probes\": false, \"inspector\": false},\n"
        << "  \"formats\": {\"AU\": true, \"VST3\": true, \"CLAP\": true, "
           "\"Standalone\": true},\n"
        << "  \"dependencies\": {\"skia\": " << json_string(i.skia_identity)
        << ", \"vst3_git_sha\": " << json_string(i.vst3_git_sha)
        << ", \"ausdk_git_sha\": " << json_string(i.ausdk_git_sha) << "},\n"
        << "  \"input_fingerprint\": " << json_string(fingerprint) << "\n"
        << "}\n";
    return out.str();
}

bool write_file_atomically(const fs::path& destination, const std::string& contents,
                           std::string& error) {
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) {
        error = "could not create provenance directory: " + ec.message();
        return false;
    }
    auto temporary = destination;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out || !(out << contents)) {
            error = "could not write temporary provenance file";
            fs::remove(temporary, ec);
            return false;
        }
    }
    fs::rename(temporary, destination, ec);
    if (ec) {
        error = "could not publish provenance file: " + ec.message();
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}

} // namespace pulp::cli::local_sdk
