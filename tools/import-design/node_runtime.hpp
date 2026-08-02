// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::import_design::browser_capture {

namespace fs = std::filesystem;

inline constexpr int kMinimumNodeMajor = 22;

// Where a Node.js executable came from. A GUI-launched process inherits a
// minimal PATH (`/usr/bin:/bin:/usr/sbin:/sbin` on macOS), so `search_path`
// finds nothing for a Homebrew or version-manager install that a terminal
// launch resolves immediately.
enum class NodeOrigin {
    explicit_override,
    search_path,
    install_location,
    version_manager,
};

std::string node_origin_name(NodeOrigin origin);

struct NodeCandidate {
    fs::path executable;
    NodeOrigin origin = NodeOrigin::install_location;
    // Human label for diagnostics: "PATH", "Homebrew", "mise", "nvm", ...
    std::string source;
};

// A location the search looks at, recorded whether or not it produced a
// candidate. Glob roots are reported as the pattern so the failure message can
// say which install layouts were considered.
struct NodeSearchLocation {
    std::string source;
    std::string location;
};

struct NodeSearchOptions {
    // Wins outright when set: no discovery, no reordering.
    std::optional<fs::path> explicit_path;

    // nullopt reads the process PATH. An engaged value (including an empty
    // string, meaning "nothing on PATH") keeps discovery hermetic in tests and
    // reproduces a GUI launch without mutating the environment.
    std::optional<std::string> search_path;

    // Root of the version-manager search (mise, nvm, fnm, asdf all install
    // under the home directory). nullopt reads HOME; an engaged empty path
    // searches no version manager at all.
    std::optional<fs::path> home;

    // Absolute executables considered ahead of the platform defaults.
    std::vector<fs::path> extra_locations;

    // The fixed OS install paths (/opt/homebrew/bin/node and friends). Set
    // false to keep a search off the real machine; this does not affect the
    // version-manager roots, which `home` governs independently.
    bool include_default_locations = true;

    int minimum_major = kMinimumNodeMajor;
};

struct NodeAttempt {
    fs::path executable;
    std::string source;
    // Empty when the executable could not report a version.
    std::string version;
    int major = 0;
    // Empty on success.
    std::string failure;
};

enum class NodeResolutionFailure {
    none,
    // No Node.js executable exists in any searched location.
    not_found,
    // Node.js exists but no installation meets the version floor.
    incompatible,
};

struct NodeResolution {
    std::optional<fs::path> executable;
    std::string version;
    int major_version = 0;
    std::string source;
    NodeOrigin origin = NodeOrigin::install_location;

    // Executables that existed and were version-probed, in probe order.
    std::vector<NodeAttempt> attempts;
    // Every location the search considered, in search order.
    std::vector<NodeSearchLocation> searched;

    NodeResolutionFailure failure = NodeResolutionFailure::not_found;

    [[nodiscard]] bool ok() const noexcept { return executable.has_value(); }

    // True when PATH alone would have failed but a direct install location
    // succeeded — the GUI-launch case.
    [[nodiscard]] bool resolved_off_path() const noexcept {
        return ok() && origin != NodeOrigin::search_path
            && origin != NodeOrigin::explicit_override;
    }
};

// Raw `node --version` output, or nullopt when the executable could not run.
using NodeVersionProbe =
    std::function<std::optional<std::string>(const fs::path&)>;

bool parse_node_version(std::string_view output,
                        std::string& version,
                        int& major);

// Ordered executables to try. Version-manager roots glob across installed
// versions and are ordered newest-first by the version parsed out of the
// directory name (numerically, so 22 sorts above 9); names that carry no
// version ("latest", "lts-jod") follow the numbered ones.
std::vector<NodeCandidate> collect_node_candidates(
    const NodeSearchOptions& options = {});

// Probes candidates in order and selects the first that meets the floor.
NodeResolution resolve_node(const NodeSearchOptions& options = {},
                            NodeVersionProbe probe = {});

// The searched-locations / probed-versions block for a failure message.
std::string node_search_report(const NodeResolution& resolution);

}  // namespace pulp::import_design::browser_capture
