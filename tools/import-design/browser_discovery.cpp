// SPDX-License-Identifier: MIT
#include "browser_capture_backend.hpp"

#include <pulp/platform/child_process.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace pulp::import_design::browser_capture {

namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

fs::path pulp_home() {
    if (const char* value = std::getenv("PULP_HOME"); value && *value)
        return fs::path(value);
#ifdef _WIN32
    if (const char* value = std::getenv("USERPROFILE"); value && *value)
        return fs::path(value) / ".pulp";
#else
    if (const char* value = std::getenv("HOME"); value && *value)
        return fs::path(value) / ".pulp";
#endif
    return {};
}

fs::path default_managed_root() {
    const auto home = pulp_home();
    return home.empty()
        ? fs::path{}
        : home / "tools" / "chrome-for-testing";
}

std::string configured_mode() {
    const auto home = pulp_home();
    if (home.empty()) return {};
    std::ifstream input(home / "config.toml");
    if (!input) return {};
    std::string section;
    for (std::string line; std::getline(input, line);) {
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        auto normalized = trim(line);
        if (normalized.size() >= 2 && normalized.front() == '[' &&
            normalized.back() == ']') {
            section = normalized.substr(1, normalized.size() - 2);
            continue;
        }
        if (section != "import_design") continue;
        const auto equal = normalized.find('=');
        if (equal == std::string::npos ||
            trim(normalized.substr(0, equal)) != "browser") {
            continue;
        }
        return trim(normalized.substr(equal + 1));
    }
    return {};
}

std::optional<BrowserMode> parse_mode(const std::string& value) {
    const auto normalized = trim(value);
    if (normalized.empty() || normalized == "auto")
        return BrowserMode::auto_select;
    if (normalized == "managed") return BrowserMode::managed;
    if (normalized == "system") return BrowserMode::system;
    return std::nullopt;
}

std::string path_key(const fs::path& path) {
    std::error_code ec;
    auto absolute = fs::absolute(path, ec);
    if (ec) absolute = path;
    return absolute.lexically_normal().generic_string();
}

void append_candidate(std::vector<BrowserCandidate>& out,
                      std::set<std::string>& seen,
                      const fs::path& path,
                      BrowserOrigin origin) {
    if (!path.empty() && seen.insert(path_key(path)).second)
        out.push_back({path, origin});
}

std::optional<std::string> json_string(
    const std::string& body, std::string_view key) {
    const std::regex pattern{
        "\"" + std::string(key) + "\"\\s*:\\s*\"([^\"]+)\""};
    std::smatch match;
    if (!std::regex_search(body, match, pattern)) return std::nullopt;
    return match[1].str();
}

std::optional<unsigned> json_unsigned(
    const std::string& body, std::string_view key) {
    const std::regex pattern{
        "\"" + std::string(key) + "\"\\s*:\\s*([0-9]+)"};
    std::smatch match;
    if (!std::regex_search(body, match, pattern)) return std::nullopt;
    try {
        return static_cast<unsigned>(std::stoul(match[1].str()));
    } catch (...) {
        return std::nullopt;
    }
}

std::size_t json_key_count(
    const std::string& body, std::string_view key) {
    const std::regex pattern{"\"" + std::string(key) + "\"\\s*:"};
    return static_cast<std::size_t>(
        std::distance(
            std::sregex_iterator(body.begin(), body.end(), pattern),
            std::sregex_iterator{}));
}

bool path_is_within(const fs::path& child, const fs::path& root) {
    auto child_it = child.begin();
    for (auto root_it = root.begin(); root_it != root.end(); ++root_it) {
        if (child_it == child.end() || *child_it != *root_it) return false;
        ++child_it;
    }
    return true;
}

std::optional<fs::path> managed_current_browser(const fs::path& root) {
    if (root.empty()) return std::nullopt;
    std::ifstream input(root / "current.json", std::ios::binary);
    if (!input) return std::nullopt;
    const std::string body{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    for (const auto key : {"schema", "version", "platform", "executable"}) {
        if (json_key_count(body, key) != 1) return std::nullopt;
    }
    const auto schema = json_unsigned(body, "schema");
    const auto version = json_string(body, "version");
    const auto platform = json_string(body, "platform");
    const auto executable = json_string(body, "executable");
    if (!schema || *schema != 1 || !version || !platform || !executable)
        return std::nullopt;

    const fs::path relative(*executable);
    if (relative.is_absolute() || relative.empty()) return std::nullopt;
    const auto normalized = relative.lexically_normal();
    if (normalized.begin() == normalized.end() ||
        *normalized.begin() != fs::path(*version) ||
        std::next(normalized.begin()) == normalized.end() ||
        *std::next(normalized.begin()) != fs::path(*platform)) {
        return std::nullopt;
    }
    for (const auto& component : normalized) {
        if (component == "..") return std::nullopt;
    }
    const auto candidate = root / normalized;
    std::error_code ec;
    if (!fs::is_regular_file(candidate, ec) || ec) return std::nullopt;
    const auto canonical_root = fs::canonical(root, ec);
    if (ec) return std::nullopt;
    const auto canonical_candidate = fs::canonical(candidate, ec);
    if (ec || !path_is_within(canonical_candidate, canonical_root))
        return std::nullopt;
    return candidate;
}

std::vector<fs::path> default_system_browser_paths() {
    std::vector<fs::path> paths;
#ifdef __APPLE__
    paths.emplace_back(
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome");
    paths.emplace_back(
        "/Applications/Google Chrome for Testing.app/Contents/MacOS/"
        "Google Chrome for Testing");
    paths.emplace_back(
        "/Applications/Chromium.app/Contents/MacOS/Chromium");
    if (const char* home = std::getenv("HOME"); home && *home) {
        const fs::path applications = fs::path(home) / "Applications";
        paths.push_back(
            applications / "Google Chrome.app/Contents/MacOS/Google Chrome");
        paths.push_back(
            applications / "Google Chrome for Testing.app/Contents/MacOS/"
            "Google Chrome for Testing");
        paths.push_back(applications /
                        "Chromium.app/Contents/MacOS/Chromium");
    }
#elif defined(_WIN32)
    auto append_from = [&](const char* variable, const fs::path& suffix) {
        if (const char* value = std::getenv(variable); value && *value)
            paths.push_back(fs::path(value) / suffix);
    };
    append_from("PROGRAMFILES", "Google/Chrome/Application/chrome.exe");
    append_from("PROGRAMFILES(X86)", "Google/Chrome/Application/chrome.exe");
    append_from("LOCALAPPDATA", "Google/Chrome/Application/chrome.exe");
    append_from("LOCALAPPDATA",
                "Google/Chrome for Testing/Application/chrome.exe");
#else
    paths.emplace_back("/usr/bin/google-chrome");
    paths.emplace_back("/usr/bin/google-chrome-stable");
    paths.emplace_back("/usr/bin/chromium");
    paths.emplace_back("/usr/bin/chromium-browser");
    paths.emplace_back("/snap/bin/chromium");
#endif
#ifdef _WIN32
    constexpr std::string_view names[] = {"chrome.exe", "chromium.exe"};
#else
    constexpr std::string_view names[] = {
        "google-chrome", "google-chrome-stable", "chromium",
        "chromium-browser"};
#endif
    for (const auto name : names) {
        if (auto found = platform::find_on_path(std::string(name)))
            paths.push_back(*found);
    }
    return paths;
}

}  // namespace

std::string browser_mode_name(BrowserMode mode) {
    switch (mode) {
        case BrowserMode::auto_select: return "auto";
        case BrowserMode::managed: return "managed";
        case BrowserMode::system: return "system";
    }
    return "auto";
}

BrowserModeSelection resolve_browser_mode(
    const BrowserDiscoveryOptions& options) {
    std::string raw;
    std::string source = "default";
    if (options.mode_override) {
        raw = *options.mode_override;
        source = "caller";
    } else if (const char* value = std::getenv("PULP_DESIGN_BROWSER_MODE");
               value && *value) {
        raw = value;
        source = "env:PULP_DESIGN_BROWSER_MODE";
    } else if (!(raw = configured_mode()).empty()) {
        source = "config:import_design.browser";
    }
    auto mode = parse_mode(raw);
    if (!mode) {
        return {
            std::nullopt,
            source,
            "invalid browser mode '" + raw + "' from " + source +
                " (expected auto, managed, or system)"};
    }
    return {mode, source, {}};
}

std::vector<BrowserCandidate>
collect_browser_candidates(const BrowserDiscoveryOptions& options) {
    std::vector<BrowserCandidate> candidates;
    std::set<std::string> seen;
    if (options.explicit_path && !options.explicit_path->empty()) {
        append_candidate(candidates, seen, *options.explicit_path,
                         BrowserOrigin::explicit_override);
        return candidates;
    }
    std::string environment;
    if (options.environment_override) {
        environment = *options.environment_override;
    } else if (const char* value = std::getenv("PULP_DESIGN_BROWSER");
               value && *value) {
        environment = value;
    }
    if (!environment.empty()) {
        append_candidate(candidates, seen, environment,
                         BrowserOrigin::environment_override);
        return candidates;
    }

    const auto selection = resolve_browser_mode(options);
    if (!selection.mode) return candidates;
    if (*selection.mode != BrowserMode::system) {
        const auto root =
            options.managed_root.value_or(default_managed_root());
        if (const auto current = managed_current_browser(root)) {
            append_candidate(
                candidates, seen, *current, BrowserOrigin::managed);
        }
    }
    if (*selection.mode != BrowserMode::managed) {
        for (const auto& path : options.system_candidates)
            append_candidate(candidates, seen, path, BrowserOrigin::system);
        if (options.include_default_system_candidates) {
            for (const auto& path : default_system_browser_paths())
                append_candidate(
                    candidates, seen, path, BrowserOrigin::system);
        }
    }
    return candidates;
}

}  // namespace pulp::import_design::browser_capture
