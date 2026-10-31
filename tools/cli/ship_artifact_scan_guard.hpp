// Ship-time release-artifact scan.
//
// Every "a shipped binary must not contain X" rule lives in one list,
// tools/cmake/release_artifact_forbidden.json, applied by
// tools/cmake/PulpReleaseArtifactScan.cmake. The build runs that scanner as
// ctests over the artifact classes it builds, which proves the rule for a
// fixture, not for the bytes a developer signs. `pulp ship` therefore runs the
// same scanner, with the same list and the same gates, over exactly the
// binaries it is about to sign, package or report on.
//
// This header holds the pure parts: deriving the active gates from the build
// directory's CMakeCache.txt (the mirror of pulp_release_artifact_active_gates
// in PulpReleaseArtifactScan.cmake) and from the installed SDK's provenance,
// locating the scanner beside the build's source tree or inside its SDK,
// telling which files under an artifact are native binaries, and rendering the
// `cmake -P` argv. It is header-only and stdlib-only on purpose, like
// ship_tracing_guard.hpp, so test/test_ship_artifact_scan_guard.cpp proves it
// against fixtures without a built plug-in. cmd_ship.cpp runs the command and
// turns its verdict into a refusal.
#pragma once

#include "configure_defaults.hpp"
#include "ship_dev_build_guard.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace pulp::cli {

inline constexpr std::string_view kReleaseScanScriptName = "PulpReleaseArtifactScan.cmake";
inline constexpr std::string_view kReleaseScanListName = "release_artifact_forbidden.json";
inline constexpr std::string_view kSdkProvenanceName = "sdk-provenance.json";

// Whether this host can produce evidence for the scan. The scanner needs a
// POSIX shell for its runner and nm for symbol markers; on Windows the runner
// would go through cmd.exe and an MSVC host has no nm, so the scan is skipped
// there with a line saying so rather than refusing every signing.
inline constexpr bool release_scan_supported() {
#if defined(_WIN32)
    return false;
#else
    return true;
#endif
}

// The gate ids PulpReleaseArtifactScan.cmake understands, as the ship scan
// derives them. `control-stripped` is decided per artifact by the
// control-shipping manifest and is never derived here.
inline constexpr std::string_view kGateJsOff = "js-off";
inline constexpr std::string_view kGateTracingOff = "tracing-off";
inline constexpr std::string_view kGateReloadShip = "reload-ship";
inline constexpr std::string_view kGateDevEnvOff = "dev-env-off";
inline constexpr std::string_view kGateBridgeExecOff = "bridge-exec-off";

namespace detail {

inline void add_gate(std::vector<std::string>& gates, std::string_view gate) {
    for (const auto& existing : gates)
        if (existing == gate)
            return;
    gates.emplace_back(gate);
}

// The scalar values of a provenance marker, keyed by where they sit: a
// top-level key as "key", a key of the top-level "features" object as
// "features.key". Nothing deeper is read, so an object such as gpu_audio or
// integrity cannot shadow a top-level key whatever order the producer wrote
// the keys in. Quotes are stripped from string values; objects and arrays
// are not recorded. The marker is produced by Pulp's own tooling, so this
// walks braces and strings rather than carrying a JSON parser.
inline std::map<std::string, std::string> provenance_scalars(std::string_view text) {
    std::map<std::string, std::string> out;
    std::vector<std::string> path; // object keys from the root to the current value
    int depth = 0;                 // 1 inside the root object
    bool in_string = false;
    std::string token;       // the string literal being read
    std::string pending_key; // the key awaiting its value at this depth
    bool expecting_value = false;
    bool string_is_value = false;
    const auto record = [&](const std::string& value) {
        if (!expecting_value)
            return;
        if (depth == 1)
            out[pending_key] = value;
        else if (depth == 2 && !path.empty() && path.back() == "features")
            out["features." + pending_key] = value;
        expecting_value = false;
    };
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (c == '\\' && i + 1 < text.size()) {
                token.push_back(text[++i]);
                continue;
            }
            if (c == '"') {
                in_string = false;
                if (string_is_value)
                    record(token);
                else
                    pending_key = token;
                continue;
            }
            token.push_back(c);
            continue;
        }
        switch (c) {
        case '"':
            in_string = true;
            token.clear();
            string_is_value = expecting_value; // a string after ':' is a value
            break;
        case ':':
            expecting_value = true;
            break;
        case '{':
        case '[':
            path.push_back(expecting_value ? pending_key : std::string());
            expecting_value = false;
            ++depth;
            break;
        case '}':
        case ']':
            if (!path.empty())
                path.pop_back();
            --depth;
            break;
        case ',':
        case ' ':
        case '\t':
        case '\n':
        case '\r':
            break;
        default: {
            if (!expecting_value)
                break;
            // A bare scalar: true, false, null or a number.
            size_t end = i;
            while (end < text.size() && text[end] != ',' && text[end] != '}' && text[end] != ']' &&
                   text[end] != ' ' && text[end] != '\n' && text[end] != '\r' && text[end] != '\t')
                ++end;
            record(std::string(text.substr(i, end - i)));
            i = end - 1;
            break;
        }
        }
    }
    return out;
}

} // namespace detail

// The gates the build directory's cache proves, mirroring
// pulp_release_artifact_active_gates: only an option the cache recorded
// contributes, an explicit PULP_RELOAD_DEV_WATCHER / PULP_BRIDGE_EXEC_ENABLED
// wins over its profile-level fallback, and a build that predates an option
// gates nothing it cannot know. Order is stable so a message can quote it.
inline std::vector<std::string> release_scan_gates_from_cache(std::string_view cache) {
    std::vector<std::string> gates;
    const auto off = [&cache](std::string_view name) -> std::optional<bool> {
        const auto value = cmake_cache_value(cache, name);
        if (!value)
            return std::nullopt;
        return !cmake_cache_flag_on(*value);
    };
    if (off("PULP_ENABLE_JS").value_or(false))
        detail::add_gate(gates, kGateJsOff);
    if (off("PULP_TRACING").value_or(false))
        detail::add_gate(gates, kGateTracingOff);
    if (const auto watcher = off("PULP_RELOAD_DEV_WATCHER")) {
        if (*watcher)
            detail::add_gate(gates, kGateReloadShip);
    } else if (off("PULP_DEV_BUILD").value_or(false)) {
        detail::add_gate(gates, kGateReloadShip);
    }
    if (off("PULP_DEV_BUILD").value_or(false))
        detail::add_gate(gates, kGateDevEnvOff);
    if (const auto enabled = off("PULP_BRIDGE_EXEC_ENABLED")) {
        if (*enabled)
            detail::add_gate(gates, kGateBridgeExecOff);
    } else if (off("PULP_DEV_BRIDGE_EXEC").value_or(false)) {
        detail::add_gate(gates, kGateBridgeExecOff);
    }
    return gates;
}

// The gates an installed SDK's provenance proves for every binary linked
// against it. Two marker shapes exist (tools/scripts/sdk_provenance.py and
// tools/cmake/PulpStampSourceInstall.cmake.in):
// - an official release SDK: `"profile": "official-release"` with a
//   `"features"` object carrying `"bridge_exec": false` and `"tracing": false`;
// - a source install: top-level `"bridge_exec"` and, once recorded,
//   `"build_profile": "release"` or `"dev"`.
// A release-profile SDK compiles out the dev environment hooks, the reload
// watcher and the exec bridge, so `official-release` and `build_profile:
// release` each prove `dev-env-off`, `reload-ship` and `bridge-exec-off`;
// `bridge_exec: false` (top-level or under features) proves `bridge-exec-off`
// and `features.tracing: false` proves `tracing-off`. A consumer project's own
// cache rarely records those options, so without this an SDK-based plug-in
// would be scanned against nothing. Only the top level and the features
// object are read, so a key of the same name inside gpu_audio or integrity
// cannot shadow them.
inline std::vector<std::string> release_scan_gates_from_provenance(std::string_view provenance) {
    std::vector<std::string> gates;
    const auto scalars = detail::provenance_scalars(provenance);
    const auto is = [&scalars](const char* key, std::string_view want) {
        const auto it = scalars.find(key);
        return it != scalars.end() && it->second == want;
    };
    if (is("profile", "official-release") || is("build_profile", "release")) {
        detail::add_gate(gates, kGateDevEnvOff);
        detail::add_gate(gates, kGateReloadShip);
        detail::add_gate(gates, kGateBridgeExecOff);
    }
    if (is("features.bridge_exec", "false") || is("bridge_exec", "false"))
        detail::add_gate(gates, kGateBridgeExecOff);
    if (is("features.tracing", "false"))
        detail::add_gate(gates, kGateTracingOff);
    return gates;
}

// The SDK prefix a cache's Pulp_DIR (<prefix>/lib/cmake/Pulp) names, or
// nullopt when the cache records none.
inline std::optional<std::filesystem::path> sdk_prefix_from_cache(std::string_view cache) {
    const auto pulp_dir = cmake_cache_value(cache, "Pulp_DIR");
    if (!pulp_dir || pulp_dir->empty() || *pulp_dir == "Pulp_DIR-NOTFOUND")
        return std::nullopt;
    return std::filesystem::path(*pulp_dir).parent_path().parent_path().parent_path();
}

// The union of the cache's gates and the installed SDK's provenance gates, in
// a stable order. The provenance text is passed in, so the fixture tests run
// without an installed SDK.
inline std::vector<std::string>
release_scan_gates(std::string_view cache, const std::optional<std::string>& sdk_provenance) {
    auto gates = release_scan_gates_from_cache(cache);
    if (sdk_provenance)
        for (const auto& gate : release_scan_gates_from_provenance(*sdk_provenance))
            detail::add_gate(gates, gate);
    return gates;
}

// Text of the installed SDK's provenance file for the build the cache
// describes, or nullopt when there is no SDK or no stamp.
inline std::optional<std::string> read_sdk_provenance(std::string_view cache) {
    const auto prefix = sdk_prefix_from_cache(cache);
    if (!prefix)
        return std::nullopt;
    std::ifstream in(*prefix / std::string(kSdkProvenanceName), std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The scanner to run for the build the cache describes: the one beside the
// build's source tree (a Pulp checkout) first, then the copy the SDK installs
// under lib/cmake/Pulp. nullopt when neither exists; the list must sit beside
// the script, as the scanner reads it from its own directory.
inline std::optional<std::filesystem::path> release_scan_script(std::string_view cache) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto usable = [&ec](const fs::path& dir) {
        return fs::is_regular_file(dir / std::string(kReleaseScanScriptName), ec) && !ec &&
               fs::is_regular_file(dir / std::string(kReleaseScanListName), ec) && !ec;
    };
    if (const auto home = cmake_cache_value(cache, "CMAKE_HOME_DIRECTORY");
        home && !home->empty()) {
        const fs::path dir = fs::path(*home) / "tools" / "cmake";
        if (usable(dir))
            return dir / std::string(kReleaseScanScriptName);
    }
    if (const auto pulp_dir = cmake_cache_value(cache, "Pulp_DIR");
        pulp_dir && !pulp_dir->empty() && *pulp_dir != "Pulp_DIR-NOTFOUND") {
        const fs::path dir(*pulp_dir);
        if (usable(dir))
            return dir / std::string(kReleaseScanScriptName);
    }
    return std::nullopt;
}

// The cmake the build was configured with (CMAKE_COMMAND), so the scan runs
// under the same CMake as the build; "cmake" from PATH when unrecorded.
inline std::string release_scan_cmake(std::string_view cache) {
    const auto recorded = cmake_cache_value(cache, "CMAKE_COMMAND");
    std::error_code ec;
    if (recorded && !recorded->empty() && std::filesystem::exists(*recorded, ec) && !ec)
        return *recorded;
    return "cmake";
}

// Whether the first bytes are a Mach-O (thin, either byte order, or fat), ELF
// or PE image. The scanner runs nm on what it is given, and nm fails on a
// plist, a resource or a text file, so a bundle is reduced to its native
// binaries by content rather than by layout.
inline bool looks_like_native_binary(std::string_view head) {
    if (head.size() < 4)
        return false;
    const auto b = [&head](size_t i) { return static_cast<unsigned char>(head[i]); };
    const std::uint32_t be = (std::uint32_t(b(0)) << 24) | (std::uint32_t(b(1)) << 16) |
                             (std::uint32_t(b(2)) << 8) | std::uint32_t(b(3));
    switch (be) {
    case 0xFEEDFACEu:
    case 0xFEEDFACFu: // Mach-O 32/64, big-endian order
    case 0xCEFAEDFEu:
    case 0xCFFAEDFEu: // Mach-O 32/64, little-endian order
    case 0xCAFEBABEu:
    case 0xCAFEBABFu: // fat Mach-O
    case 0x7F454C46u: // ELF
        return true;
    default:
        break;
    }
    return head[0] == 'M' && head[1] == 'Z'; // PE / COFF image
}

inline bool is_native_binary(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return false;
    char head[4] = {};
    in.read(head, sizeof head);
    return looks_like_native_binary(std::string_view(head, static_cast<size_t>(in.gcount())));
}

// Every native binary the artifact holds: the artifact itself when it is a
// loose file, or every regular file under a bundle directory that carries a
// native image header. Sorted, so the scan and its report are stable. Nothing
// here is allowed to fail quietly into a clean verdict: a top-level symlink
// (an alias could point the scan at something else), a directory the walk
// cannot read, and a path carrying `;` (the CMake list separator the BINS
// argument is joined with) each set `error` and yield no targets, which the
// caller must refuse rather than pass. Symlinks inside a bundle are not
// followed.
inline std::vector<std::filesystem::path>
release_scan_targets(const std::filesystem::path& artifact, std::string* error = nullptr) {
    namespace fs = std::filesystem;
    std::vector<fs::path> targets;
    const auto fail = [&](const std::string& why) {
        if (error)
            *error = why;
        targets.clear();
        return targets;
    };
    if (artifact.string().find(';') != std::string::npos)
        return fail(artifact.string() + ": a path containing ';' cannot be passed to the scanner");
    std::error_code ec;
    const auto status = fs::symlink_status(artifact, ec);
    if (status.type() == fs::file_type::not_found)
        return targets; // an optional root that was never built: nothing to read
    if (ec)
        return fail(artifact.string() + ": cannot stat: " + ec.message());
    if (status.type() == fs::file_type::symlink)
        return fail(artifact.string() + ": a symlink is not scanned; pass the real path");
    if (fs::is_directory(artifact, ec) && !ec) {
        for (fs::recursive_directory_iterator it(artifact, ec), end; it != end; it.increment(ec)) {
            if (ec)
                return fail(artifact.string() + ": cannot walk: " + ec.message());
            if (it->is_symlink(ec)) {
                if (it->is_directory(ec))
                    it.disable_recursion_pending();
                continue;
            }
            if (it->path().string().find(';') != std::string::npos)
                return fail(it->path().string() +
                            ": a path containing ';' cannot be passed to the scanner");
            if (it->is_regular_file(ec) && !ec && is_native_binary(it->path()))
                targets.push_back(it->path());
        }
        if (ec)
            return fail(artifact.string() + ": cannot walk: " + ec.message());
    } else if (fs::is_regular_file(artifact, ec) && !ec && is_native_binary(artifact)) {
        targets.push_back(artifact);
    }
    std::sort(targets.begin(), targets.end());
    return targets;
}

// The argv that runs the scanner in forbid mode over `targets` with `gates`;
// the caller quotes each element for its shell. BINS and GATES are CMake
// lists, joined with `;` inside one argument.
inline std::vector<std::string> release_scan_argv(const std::string& cmake,
                                                  const std::filesystem::path& script,
                                                  const std::vector<std::string>& gates,
                                                  const std::vector<std::filesystem::path>& targets,
                                                  std::string_view label) {
    const auto join = [](const auto& items, auto to_string) {
        std::string joined;
        for (const auto& item : items) {
            if (!joined.empty())
                joined += ';';
            joined += to_string(item);
        }
        return joined;
    };
    std::vector<std::string> argv{cmake};
    argv.push_back("-DBINS=" +
                   join(targets, [](const std::filesystem::path& p) { return p.string(); }));
    argv.push_back("-DMODE=forbid");
    argv.push_back("-DGATES=" + join(gates, [](const std::string& g) { return g; }));
    argv.push_back("-DLABEL=" + std::string(label));
    argv.push_back("-P");
    argv.push_back(script.string());
    return argv;
}

} // namespace pulp::cli
