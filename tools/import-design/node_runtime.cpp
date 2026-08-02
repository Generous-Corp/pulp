// SPDX-License-Identifier: MIT
#include "node_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <set>
#include <system_error>
#include <utility>

#include <pulp/platform/child_process.hpp>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace pulp::import_design::browser_capture {

namespace {

#ifdef _WIN32
constexpr std::string_view kExecutableName = "node.exe";
constexpr char kPathSeparator = ';';
#else
constexpr std::string_view kExecutableName = "node";
constexpr char kPathSeparator = ':';
#endif

bool is_executable_file(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) return false;
#ifdef _WIN32
    return true;
#else
    return ::access(path.c_str(), X_OK) == 0;
#endif
}

std::optional<std::string> environment_value(const char* name) {
    const char* value = std::getenv(name);
    if (!value || *value == '\0') return std::nullopt;
    return std::string(value);
}

fs::path home_directory(const NodeSearchOptions& options) {
    if (options.home) return *options.home;
#ifdef _WIN32
    if (auto profile = environment_value("USERPROFILE")) return fs::path(*profile);
#endif
    if (auto home = environment_value("HOME")) return fs::path(*home);
    return {};
}

// A version-manager root holding one directory per installed version.
struct VersionedRoot {
    std::string source;
    fs::path root;
    fs::path suffix;
};

std::vector<VersionedRoot> versioned_roots(const fs::path& home) {
    if (home.empty()) return {};
    return {
#ifdef _WIN32
        {"fnm", home / "AppData" / "Roaming" / "fnm" / "node-versions",
         fs::path("installation") / kExecutableName},
        {"nvm", home / "AppData" / "Roaming" / "nvm", fs::path(kExecutableName)},
#else
        {"mise", home / ".local" / "share" / "mise" / "installs" / "node",
         fs::path("bin") / kExecutableName},
        {"nvm", home / ".nvm" / "versions" / "node",
         fs::path("bin") / kExecutableName},
        {"fnm", home / ".local" / "share" / "fnm" / "node-versions",
         fs::path("installation") / "bin" / kExecutableName},
        {"asdf", home / ".asdf" / "installs" / "nodejs",
         fs::path("bin") / kExecutableName},
#endif
    };
}

struct FixedLocation {
    std::string source;
    fs::path executable;
};

std::vector<FixedLocation> fixed_locations() {
#ifdef _WIN32
    std::vector<FixedLocation> locations;
    if (auto program_files = environment_value("ProgramFiles")) {
        locations.push_back(
            {"Node.js installer",
             fs::path(*program_files) / "nodejs" / kExecutableName});
    }
    if (auto local_app_data = environment_value("LOCALAPPDATA")) {
        locations.push_back(
            {"Node.js installer",
             fs::path(*local_app_data) / "Programs" / "nodejs"
                 / kExecutableName});
    }
    return locations;
#else
    return {
        {"Homebrew", "/opt/homebrew/bin/node"},
        {"Homebrew or installer", "/usr/local/bin/node"},
        {"system", "/usr/bin/node"},
    };
#endif
}

// Numeric components of a version-manager directory name. "v22.11.0" and
// "22.11.0" both yield {22, 11, 0}; "latest" and "lts-jod" yield {}, which
// sorts them behind every numbered install rather than wherever the alphabet
// happens to put them.
std::vector<int> directory_version(const std::string& name) {
    std::vector<int> parts;
    std::size_t index = 0;
    if (index < name.size() && (name[index] == 'v' || name[index] == 'V'))
        ++index;
    if (index >= name.size()
        || !std::isdigit(static_cast<unsigned char>(name[index]))) {
        return {};
    }
    while (index < name.size()) {
        if (!std::isdigit(static_cast<unsigned char>(name[index]))) break;
        int value = 0;
        while (index < name.size()
               && std::isdigit(static_cast<unsigned char>(name[index]))) {
            // A pathological directory name must not overflow the compare key.
            if (value < 1'000'000) value = value * 10 + (name[index] - '0');
            ++index;
        }
        parts.push_back(value);
        if (index < name.size() && name[index] == '.') {
            ++index;
            continue;
        }
        break;
    }
    return parts;
}

// Newest first. Equal-prefix versions order by component count, so 22.11.1
// outranks 22.11.
bool newer_first(const std::vector<int>& left, const std::vector<int>& right) {
    const auto shared = std::min(left.size(), right.size());
    for (std::size_t i = 0; i < shared; ++i) {
        if (left[i] != right[i]) return left[i] > right[i];
    }
    return left.size() > right.size();
}

std::vector<fs::path> split_search_path(const std::string& value) {
    std::vector<fs::path> entries;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(kPathSeparator, start);
        const auto piece = value.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!piece.empty()) entries.emplace_back(piece);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return entries;
}

std::string display_path(const fs::path& path) { return path.string(); }

std::optional<std::string> run_node_version(const fs::path& executable) {
    platform::ProcessOptions options;
    options.timeout_ms = 5000;
    options.max_output_bytes = 64 * 1024;
    auto process = platform::ChildProcess::run(
        executable.string(), {"--version"}, options);
    if (process.timed_out || process.exit_code != 0) return std::nullopt;
    if (!process.stdout_output.empty()) return process.stdout_output;
    if (!process.stderr_output.empty()) return process.stderr_output;
    return std::nullopt;
}

}  // namespace

std::string node_origin_name(NodeOrigin origin) {
    switch (origin) {
        case NodeOrigin::explicit_override: return "explicit";
        case NodeOrigin::search_path: return "path";
        case NodeOrigin::install_location: return "install-location";
        case NodeOrigin::version_manager: return "version-manager";
    }
    return "install-location";
}

bool parse_node_version(std::string_view output,
                        std::string& version,
                        int& major) {
    static const std::regex kVersionPattern{
        R"(\bv?([0-9]+(?:\.[0-9]+){1,3})\b)"};
    std::string line(output);
    const auto newline = line.find_first_of("\r\n");
    if (newline != std::string::npos) line.resize(newline);
    std::smatch match;
    if (!std::regex_search(line, match, kVersionPattern)) return false;
    version = match[1].str();
    try {
        major = std::stoi(match[1].str());
    } catch (...) {
        return false;
    }
    return major > 0;
}

std::vector<NodeCandidate> collect_node_candidates(
    const NodeSearchOptions& options) {
    std::vector<NodeCandidate> candidates;
    std::set<fs::path> seen;

    const auto add = [&](fs::path executable,
                         NodeOrigin origin,
                         std::string source) {
        std::error_code ec;
        auto key = fs::weakly_canonical(executable, ec);
        if (ec) key = executable;
        if (!seen.insert(key).second) return false;
        candidates.push_back(
            {std::move(executable), origin, std::move(source)});
        return true;
    };

    if (options.explicit_path && !options.explicit_path->empty()) {
        // An override that is not there resolves to nothing rather than to a
        // version probe against a missing file, so the caller reports "not
        // found" instead of "could not report its version".
        if (is_executable_file(*options.explicit_path)) {
            add(*options.explicit_path, NodeOrigin::explicit_override,
                "explicit");
        }
        return candidates;
    }

    const auto path_value = options.search_path
        ? *options.search_path
        : environment_value("PATH").value_or(std::string{});
    for (const auto& directory : split_search_path(path_value)) {
        const auto executable = directory / kExecutableName;
        if (is_executable_file(executable))
            add(executable, NodeOrigin::search_path, "PATH");
    }

    for (const auto& extra : options.extra_locations) {
        if (is_executable_file(extra))
            add(extra, NodeOrigin::install_location, "configured location");
    }

    if (options.include_default_locations) {
        for (const auto& location : fixed_locations()) {
            if (is_executable_file(location.executable))
                add(location.executable, NodeOrigin::install_location,
                    location.source);
        }
    }

    for (const auto& root : versioned_roots(home_directory(options))) {
        struct Entry {
            std::vector<int> version;
            std::string name;
            fs::path executable;
        };
        std::vector<Entry> entries;
        std::error_code ec;
        for (fs::directory_iterator it(root.root, ec), end; !ec && it != end;
             it.increment(ec)) {
            const auto name = it->path().filename().string();
            const auto executable = it->path() / root.suffix;
            if (!is_executable_file(executable)) continue;
            entries.push_back({directory_version(name), name, executable});
        }
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& left, const Entry& right) {
                      if (left.version.empty() != right.version.empty())
                          return right.version.empty();
                      if (left.version.empty()) return left.name < right.name;
                      return newer_first(left.version, right.version);
                  });
        for (auto& entry : entries) {
            add(std::move(entry.executable), NodeOrigin::version_manager,
                root.source);
        }
    }

    return candidates;
}

NodeResolution resolve_node(const NodeSearchOptions& options,
                            NodeVersionProbe probe) {
    NodeResolution resolution;
    if (!probe) probe = run_node_version;

    const auto candidates = collect_node_candidates(options);

    // Record what was looked at before any probe runs, so a "nothing found"
    // message can name the locations rather than the browsers.
    const auto note = [&](std::string source, std::string location) {
        resolution.searched.push_back(
            {std::move(source), std::move(location)});
    };
    if (options.explicit_path && !options.explicit_path->empty()) {
        note("explicit", display_path(*options.explicit_path));
    } else {
        note("PATH", std::string(kExecutableName) + " on PATH");
        for (const auto& extra : options.extra_locations)
            note("configured location", display_path(extra));
        if (options.include_default_locations) {
            for (const auto& location : fixed_locations())
                note(location.source, display_path(location.executable));
        }
        for (const auto& root : versioned_roots(home_directory(options)))
            note(root.source, display_path(root.root / "*" / root.suffix));
    }

    for (const auto& candidate : candidates) {
        NodeAttempt attempt;
        attempt.executable = candidate.executable;
        attempt.source = candidate.source;

        const auto output = probe(candidate.executable);
        if (!output) {
            attempt.failure = "version probe failed";
            resolution.attempts.push_back(std::move(attempt));
            continue;
        }
        if (!parse_node_version(*output, attempt.version, attempt.major)) {
            attempt.failure = "version output was not recognized";
            resolution.attempts.push_back(std::move(attempt));
            continue;
        }
        if (attempt.major < options.minimum_major) {
            attempt.failure = "too old (need "
                + std::to_string(options.minimum_major) + " or newer)";
            resolution.attempts.push_back(std::move(attempt));
            continue;
        }

        resolution.executable = candidate.executable;
        resolution.version = attempt.version;
        resolution.major_version = attempt.major;
        resolution.source = candidate.source;
        resolution.origin = candidate.origin;
        resolution.failure = NodeResolutionFailure::none;
        resolution.attempts.push_back(std::move(attempt));
        return resolution;
    }

    resolution.failure = resolution.attempts.empty()
        ? NodeResolutionFailure::not_found
        : NodeResolutionFailure::incompatible;
    return resolution;
}

std::string node_search_report(const NodeResolution& resolution) {
    std::string out;
    if (!resolution.attempts.empty()) {
        out += "Node.js installations checked:";
        for (const auto& attempt : resolution.attempts) {
            out += "\n  " + attempt.source + ": "
                + display_path(attempt.executable) + " — ";
            if (attempt.failure.empty()) {
                out += "v" + attempt.version + " (selected)";
            } else if (!attempt.version.empty()) {
                out += "v" + attempt.version + ", " + attempt.failure;
            } else {
                out += attempt.failure;
            }
        }
        return out;
    }
    if (resolution.searched.empty()) return out;
    out += "Searched for Node.js in:";
    for (const auto& location : resolution.searched)
        out += "\n  " + location.source + ": " + location.location;
    return out;
}

}  // namespace pulp::import_design::browser_capture
