// SPDX-License-Identifier: MIT
#include "browser_capture_backend.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef _WIN32
#include <pulp/platform/win32_sane.hpp>
#endif

namespace pulp::import_design::browser_capture {

namespace {

constexpr std::string_view kChromeDownloadUrl =
    "https://www.google.com/chrome/";
constexpr int kLauncherCleanupGraceMs = 5000;

int outer_process_timeout(int runtime_timeout_ms) {
    if (runtime_timeout_ms >
        std::numeric_limits<int>::max() - kLauncherCleanupGraceMs) {
        return std::numeric_limits<int>::max();
    }
    return runtime_timeout_ms + kLauncherCleanupGraceMs;
}

std::string trim_copy(std::string value) {
    while (!value.empty()
           && (value.back() == '\r' || value.back() == '\n'
               || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t first = 0;
    while (first < value.size()
           && (value[first] == '\r' || value[first] == '\n'
               || value[first] == ' ' || value[first] == '\t')) {
        ++first;
    }
    if (first != 0) value.erase(0, first);
    return value;
}

std::string one_line(std::string value) {
    value = trim_copy(std::move(value));
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    constexpr std::size_t kLimit = 500;
    if (value.size() > kLimit) {
        value.resize(kLimit);
        value += "...";
    }
    return value;
}

std::string sanitize_subprocess_output(std::string value) {
    static const std::regex kExternalUrl{
        R"(\b(?:https?|wss?)://[^\s"'<>]+)", std::regex::icase};
    static const std::regex kFileUrl{
        R"(file:///+[^\r\n;,)]*)", std::regex::icase};
    static const std::regex kQuotedPosixPath{
        R"((["'])(/[^"'\r\n]+)\1)"};
    static const std::regex kUnquotedPosixPath{
        R"((^|[\s=(:,])/(?!/)[^\r\n;,)]*)"};
    static const std::regex kWindowsPath{
        R"(\b[A-Za-z]:\\[^\r\n;,)]*)"};
    value = std::regex_replace(value, kExternalUrl, "<external-url>");
    value = std::regex_replace(value, kFileUrl, "file:///<local-path>");
    value = std::regex_replace(
        value, kQuotedPosixPath, "$1<local-path>$1");
    value = std::regex_replace(
        value, kUnquotedPosixPath, "$1<local-path>");
    return std::regex_replace(value, kWindowsPath, "<local-path>");
}

std::optional<std::string> capture_error_code(std::string_view stderr_text) {
    const auto colon = stderr_text.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon > 80)
        return std::nullopt;
    const auto candidate = stderr_text.substr(0, colon);
    if (!std::all_of(candidate.begin(), candidate.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                || c == '-';
        })) {
        return std::nullopt;
    }
    return std::string(candidate);
}

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 16);
    for (const unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    constexpr char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(kHex[(c >> 4) & 0xf]);
                    out.push_back(kHex[c & 0xf]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

fs::path default_managed_root() {
    if (const char* pulp_home = std::getenv("PULP_HOME");
        pulp_home && *pulp_home) {
        return fs::path(pulp_home) / "tools" / "browser-capture";
    }
#ifdef _WIN32
    if (const char* local_app_data = std::getenv("LOCALAPPDATA");
        local_app_data && *local_app_data) {
        return fs::path(local_app_data) / "Pulp" / "tools"
            / "browser-capture";
    }
#else
    if (const char* home = std::getenv("HOME"); home && *home) {
        return fs::path(home) / ".pulp" / "tools" / "browser-capture";
    }
#endif
    return fs::temp_directory_path() / "pulp" / "tools"
        / "browser-capture";
}

std::optional<fs::path> resolve_node(
    const std::optional<fs::path>& override_path) {
    if (override_path && !override_path->empty()) return *override_path;
    if (auto node = platform::find_on_path("node")) return node;
    if (auto node = platform::find_on_path("nodejs")) return node;
    return std::nullopt;
}

std::optional<fs::path> current_process_executable() {
#ifdef __APPLE__
    uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return std::nullopt;
    std::error_code ec;
    auto path = fs::weakly_canonical(buffer.data(), ec);
    return ec ? std::optional<fs::path>{fs::path(buffer.data())} : path;
#elif defined(_WIN32)
    std::vector<char> buffer(32768);
    const DWORD size = GetModuleFileNameA(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size()) return std::nullopt;
    return fs::path(std::string(buffer.data(), size));
#else
    std::error_code ec;
    auto path = fs::canonical("/proc/self/exe", ec);
    return ec ? std::nullopt : std::optional<fs::path>{std::move(path)};
#endif
}

std::optional<fs::path> resolve_capture_script(
    const std::optional<fs::path>& override_path) {
    if (override_path && !override_path->empty()) return *override_path;
    if (const char* environment =
            std::getenv("PULP_BROWSER_CAPTURE_SCRIPT");
        environment && *environment) {
        return fs::path(environment);
    }
    if (const auto executable = current_process_executable()) {
        for (const auto directory : {
                 kBrowserCaptureRuntimeDirectory,
                 kLegacyBrowserCaptureRuntimeDirectory}) {
            const auto sibling =
                executable->parent_path() / directory / "capture.mjs";
            std::error_code ec;
            if (fs::is_regular_file(sibling, ec) && !ec) return sibling;
        }
    }
    const auto managed =
        default_managed_root() / "capture.mjs";
    std::error_code ec;
    if (fs::is_regular_file(managed, ec) && !ec) return managed;
#ifdef PULP_BROWSER_CAPTURE_SCRIPT
    return fs::path(PULP_BROWSER_CAPTURE_SCRIPT);
#else
    return std::nullopt;
#endif
}

bool is_executable_file(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) return false;
#ifdef _WIN32
    return true;
#else
    return ::access(path.c_str(), X_OK) == 0;
#endif
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
    if (path.empty()) return;
    if (seen.insert(path_key(path)).second) {
        out.push_back(BrowserCandidate{path, origin});
    }
}

bool is_managed_browser_filename(std::string_view name) {
#ifdef _WIN32
    return name == "chrome.exe" || name == "chromium.exe";
#else
    return name == "chrome" || name == "chromium"
        || name == "Google Chrome" || name == "Google Chrome for Testing";
#endif
}

std::vector<fs::path> managed_browser_paths(const fs::path& root) {
    std::vector<fs::path> paths;
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) return paths;

    fs::recursive_directory_iterator it{
        root, fs::directory_options::skip_permission_denied, ec};
    const fs::recursive_directory_iterator end;
    std::size_t visited = 0;
    while (!ec && it != end && visited < 4096) {
        ++visited;
        const auto& entry = *it;
        if (entry.is_regular_file(ec) && !ec
            && is_managed_browser_filename(
                entry.path().filename().string())) {
            paths.push_back(entry.path());
        }
        it.increment(ec);
    }
    std::sort(paths.begin(), paths.end());
    return paths;
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
        const fs::path applications =
            fs::path(home) / "Applications";
        paths.push_back(
            applications / "Google Chrome.app/Contents/MacOS/Google Chrome");
        paths.push_back(
            applications
            / "Google Chrome for Testing.app/Contents/MacOS/"
              "Google Chrome for Testing");
        paths.push_back(
            applications / "Chromium.app/Contents/MacOS/Chromium");
    }
#elif defined(_WIN32)
    auto append_from = [&](const char* variable, const fs::path& suffix) {
        if (const char* value = std::getenv(variable); value && *value) {
            paths.push_back(fs::path(value) / suffix);
        }
    };
    append_from("PROGRAMFILES",
                "Google/Chrome/Application/chrome.exe");
    append_from("PROGRAMFILES(X86)",
                "Google/Chrome/Application/chrome.exe");
    append_from("LOCALAPPDATA",
                "Google/Chrome/Application/chrome.exe");
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
    constexpr std::string_view kNames[] = {
        "chrome.exe", "chromium.exe"};
#else
    constexpr std::string_view kNames[] = {
        "google-chrome", "google-chrome-stable", "chromium",
        "chromium-browser"};
#endif
    for (const auto name : kNames) {
        if (auto found = platform::find_on_path(std::string(name))) {
            paths.push_back(*found);
        }
    }
    return paths;
}

struct TempDirectory {
    fs::path path;

    TempDirectory() = default;
    explicit TempDirectory(fs::path value) : path(std::move(value)) {}
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory(TempDirectory&& other) noexcept
        : path(std::move(other.path)) {
        other.path.clear();
    }
    TempDirectory& operator=(TempDirectory&& other) noexcept {
        if (this == &other) return *this;
        std::error_code ec;
        if (!path.empty()) fs::remove_all(path, ec);
        path = std::move(other.path);
        other.path.clear();
        return *this;
    }

    ~TempDirectory() {
        if (path.empty()) return;
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::optional<TempDirectory> make_temp_directory(
    std::string_view prefix, std::string& error) {
    std::error_code ec;
    const auto base = fs::temp_directory_path(ec);
    if (ec) {
        error = "could not resolve the temporary directory: " + ec.message();
        return std::nullopt;
    }

    const auto seed = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (unsigned int attempt = 0; attempt < 128; ++attempt) {
        const auto name = std::string(prefix) + "-"
            + std::to_string(seed) + "-" + std::to_string(attempt);
        const auto candidate = base / name;
        if (fs::create_directory(candidate, ec)) {
            return TempDirectory{candidate};
        }
        if (ec && ec != std::errc::file_exists) {
            error = "could not create the temporary browser profile: "
                + ec.message();
            return std::nullopt;
        }
        ec.clear();
    }
    error = "could not allocate a unique temporary browser profile";
    return std::nullopt;
}

bool parse_browser_version(std::string_view output,
                           std::string& product,
                           std::string& version,
                           int& major) {
    static const std::regex kVersionPattern{
        R"((.*?)([0-9]+(?:\.[0-9]+){1,4}))"};
    std::smatch match;
    const std::string line = one_line(std::string(output));
    if (!std::regex_search(line, match, kVersionPattern)) return false;

    product = trim_copy(match[1].str());
    version = match[2].str();
    try {
        major = std::stoi(match[2].str());
    } catch (...) {
        return false;
    }
    return !product.empty() && major > 0;
}

bool path_is_within(const fs::path& root,
                    const fs::path& child,
                    fs::path& canonical_root,
                    fs::path& canonical_child,
                    std::string& error) {
    std::error_code ec;
    canonical_root = fs::canonical(root, ec);
    if (ec) {
        error = "staged root is not readable: " + ec.message();
        return false;
    }
    canonical_child = fs::canonical(child, ec);
    if (ec) {
        error = "input file is not readable: " + ec.message();
        return false;
    }
    if (!fs::is_directory(canonical_root, ec) || ec) {
        error = "staged root is not a directory";
        return false;
    }
    if (!fs::is_regular_file(canonical_child, ec) || ec) {
        error = "input is not a regular file";
        return false;
    }

    auto root_it = canonical_root.begin();
    auto child_it = canonical_child.begin();
    for (; root_it != canonical_root.end(); ++root_it, ++child_it) {
        if (child_it == canonical_child.end() || *root_it != *child_it) {
            error = "input file escapes the authorized staged root";
            return false;
        }
    }
    return true;
}

void write_capture_error(const fs::path& output_directory,
                         const Diagnostic& diagnostic) {
    std::error_code ec;
    fs::create_directories(output_directory, ec);
    if (ec) return;
    std::ofstream out(output_directory / "capture-error.json",
                      std::ios::binary);
    if (!out) return;
    out << "{\n"
        << "  \"schema\": \"pulp-browser-capture-error-v1\",\n"
        << "  \"code\": \"" << json_escape(diagnostic.code) << "\",\n"
        << "  \"phase\": \"" << json_escape(diagnostic.phase) << "\",\n"
        << "  \"message\": \"" << json_escape(diagnostic.message)
        << "\"\n"
        << "}\n";
}

bool nonempty_regular_file(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && !ec
        && fs::file_size(path, ec) > 0 && !ec;
}

std::optional<fs::path> prepare_capture_output_directory(
    const fs::path& requested, std::string& error) {
    if (requested.empty()) {
        error = "capture output directory must be specified";
        return std::nullopt;
    }

    std::error_code ec;
    auto output = fs::absolute(requested, ec);
    if (ec) {
        error = "could not resolve capture output directory: " + ec.message();
        return std::nullopt;
    }
    fs::create_directories(output, ec);
    if (ec) {
        error = "could not create capture output directory: " + ec.message();
        return std::nullopt;
    }
    output = fs::canonical(output, ec);
    if (ec || !fs::is_directory(output, ec) || ec) {
        error = "capture output path is not a readable directory";
        return std::nullopt;
    }

    constexpr std::array<std::string_view, 6> kGeneratedArtifacts{
        "capture.json",
        "browser.png",
        "semantic-report.json",
        "tokens.json",
        "dom-snapshot.json",
        "capture-error.json",
    };
    for (const auto name : kGeneratedArtifacts) {
        fs::remove(output / name, ec);
        if (ec) {
            error = "could not clear prior capture artifact "
                + std::string(name) + ": " + ec.message();
            return std::nullopt;
        }
    }
    return output;
}

CaptureResult capture_failure(std::string code,
                              std::string phase,
                              std::string message,
                              const fs::path& output_directory = {}) {
    CaptureResult result;
    result.diagnostic =
        Diagnostic{std::move(code), std::move(message), std::move(phase)};
    if (!output_directory.empty() &&
        !nonempty_regular_file(output_directory / "capture-error.json")) {
        write_capture_error(output_directory, result.diagnostic);
    }
    return result;
}

}  // namespace

std::string browser_origin_name(BrowserOrigin origin) {
    switch (origin) {
        case BrowserOrigin::explicit_override: return "explicit";
        case BrowserOrigin::environment_override: return "environment";
        case BrowserOrigin::managed: return "managed";
        case BrowserOrigin::system: return "system";
    }
    return "system";
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

    const auto managed_root =
        options.managed_root.value_or(default_managed_root());
    for (const auto& path : managed_browser_paths(managed_root)) {
        append_candidate(candidates, seen, path, BrowserOrigin::managed);
    }

    for (const auto& path : options.system_candidates) {
        append_candidate(candidates, seen, path, BrowserOrigin::system);
    }
    if (options.include_default_system_candidates) {
        for (const auto& path : default_system_browser_paths()) {
            append_candidate(candidates, seen, path, BrowserOrigin::system);
        }
    }
    return candidates;
}

BrowserProbeResult probe_browser(
    const BrowserCandidate& candidate,
    const BrowserDiscoveryOptions& options) {
    BrowserProbeResult result;
    result.candidate = candidate;

    if (!is_executable_file(candidate.executable)) {
        result.failure = "browser executable does not exist or is not executable";
        return result;
    }

    platform::ProcessOptions version_options;
    version_options.timeout_ms = std::min(options.probe_timeout_ms, 5000);
    version_options.max_output_bytes = 64 * 1024;
    auto version_process = platform::ChildProcess::run(
        candidate.executable.string(), {"--version"}, version_options);
    if (version_process.timed_out) {
        result.failure = "browser version probe timed out";
        return result;
    }
    if (version_process.exit_code != 0) {
        result.failure = "browser version probe failed";
        const auto detail = one_line(
            sanitize_subprocess_output(version_process.stderr_output));
        if (!detail.empty()) result.failure += ": " + detail;
        return result;
    }
    const std::string version_output =
        !version_process.stdout_output.empty()
            ? version_process.stdout_output
            : version_process.stderr_output;
    if (!parse_browser_version(version_output, result.product,
                               result.version, result.major_version)) {
        result.failure = "browser version output was not recognized";
        return result;
    }
    result.product = sanitize_subprocess_output(std::move(result.product));
    if (result.major_version < options.minimum_major) {
        result.failure = "browser is too old (found "
            + std::to_string(result.major_version) + ", need "
            + std::to_string(options.minimum_major) + " or newer)";
        return result;
    }

    const auto node = resolve_node(options.node_executable);
    if (!node || !is_executable_file(*node)) {
        result.failure =
            "Node.js was not found; browser capture needs Node.js 22 or newer";
        return result;
    }
    const auto script = resolve_capture_script(options.capture_script);
    if (!script || !fs::is_regular_file(*script)) {
        result.failure = "browser capture script was not found";
        return result;
    }

    std::string temp_error;
    auto profile = make_temp_directory("pulp-browser-probe", temp_error);
    if (!profile) {
        result.failure = temp_error;
        return result;
    }

    platform::ProcessOptions process_options;
    // The Node launcher owns the advertised deadline and then needs time to
    // terminate Chromium and remove its isolated profile.
    process_options.timeout_ms =
        outer_process_timeout(options.probe_timeout_ms);
    process_options.max_output_bytes = 1024 * 1024;
    auto process = platform::ChildProcess::run(
        node->string(),
        {script->string(),
         "probe",
         "--browser", candidate.executable.string(),
         "--profile-dir", profile->path.string(),
         "--timeout-ms", std::to_string(options.probe_timeout_ms)},
        process_options);
    process.stderr_output =
        sanitize_subprocess_output(std::move(process.stderr_output));
    if (process.timed_out) {
        result.failure = "browser CDP capability probe timed out";
        return result;
    }
    if (process.exit_code != 0) {
        result.failure = "browser does not provide the required headless/CDP "
                         "capture capabilities";
        const auto detail = one_line(process.stderr_output);
        if (!detail.empty()) result.failure += ": " + detail;
        return result;
    }

    result.compatible = true;
    return result;
}

BrowserDiscoveryResult discover_browser(
    const BrowserDiscoveryOptions& options,
    BrowserProbeFunction probe) {
    BrowserDiscoveryResult result;
    auto candidates = collect_browser_candidates(options);
    if (!probe) {
        probe = [&](const BrowserCandidate& candidate) {
            return probe_browser(candidate, options);
        };
    }

    for (const auto& candidate : candidates) {
        auto candidate_result = probe(candidate);
        candidate_result.candidate = candidate;
        result.probes.push_back(candidate_result);
        if (!candidate_result.compatible) continue;

        result.selected = BrowserInstallation{
            candidate.executable,
            candidate.origin,
            candidate_result.product,
            candidate_result.version,
            candidate_result.major_version};
        return result;
    }

    result.diagnostic = Diagnostic{
        "browser-unavailable",
        "No compatible Google Chrome or Chromium installation was found.",
        "browser-discovery"};
    return result;
}

std::string browser_unavailable_human(
    const BrowserDiscoveryResult& discovery) {
    std::ostringstream out;
    out << "Faithful HTML import needs Google Chrome or Chromium at import "
           "time.\n"
        << "Install Google Chrome: " << kChromeDownloadUrl << "\n\n"
        << "Pulp launches it with a temporary isolated profile to evaluate "
           "layout and make\n"
        << "the reference image. Chrome is not embedded in your generated "
           "plugin.\n"
        << "Use --offline for the lower-fidelity static/runtime fallback.";
    if (!discovery.probes.empty()) {
        out << "\n\nChecked:";
        for (const auto& probe : discovery.probes) {
            out << "\n  " << browser_origin_name(probe.candidate.origin)
                << ": " << probe.candidate.executable.string()
                << " — "
                << (probe.failure.empty() ? "incompatible" : probe.failure);
        }
    }
    return out.str();
}

std::string browser_unavailable_json(
    const BrowserDiscoveryResult& discovery) {
    std::ostringstream out;
    out << "{"
        << "\"code\":\"browser-unavailable\","
        << "\"message\":\""
        << json_escape(discovery.diagnostic.message.empty()
                           ? "No compatible Google Chrome or Chromium "
                             "installation was found."
                           : discovery.diagnostic.message)
        << "\","
        << "\"install_url\":\"" << kChromeDownloadUrl << "\","
        << "\"offline_flag\":\"--offline\","
        << "\"probes\":[";
    for (std::size_t i = 0; i < discovery.probes.size(); ++i) {
        if (i != 0) out << ",";
        const auto& probe = discovery.probes[i];
        out << "{"
            << "\"origin\":\""
            << browser_origin_name(probe.candidate.origin) << "\","
            << "\"path\":\""
            << json_escape(probe.candidate.executable.string()) << "\","
            << "\"compatible\":"
            << (probe.compatible ? "true" : "false") << ","
            << "\"product\":\"" << json_escape(probe.product) << "\","
            << "\"version\":\"" << json_escape(probe.version) << "\","
            << "\"failure\":\"" << json_escape(probe.failure) << "\""
            << "}";
    }
    out << "]}";
    return out.str();
}

CaptureResult capture_document(
    const BrowserInstallation& browser,
    const CaptureRequest& request) {
    if (!is_executable_file(browser.executable)) {
        return capture_failure(
            "browser-unavailable", "browser-launch",
            "selected browser executable does not exist or is not executable",
            request.output_directory);
    }
    if (!viewport_within_capture_limits(
            request.initial_width, request.initial_height,
            request.device_scale_factor)
        || request.device_scale_factor != kDefaultDeviceScaleFactor
        || request.timeout_ms <= 0) {
        return capture_failure(
            "invalid-capture-options", "capture-setup",
            "capture dimensions must be positive and no larger than 8192px "
            "per axis or 64 megapixels at DPR 2; timeout must be positive, "
            "and browser capture currently requires DPR 2",
            request.output_directory);
    }

    fs::path canonical_root;
    fs::path canonical_input;
    std::string path_error;
    if (!path_is_within(request.staged_root, request.input_file,
                        canonical_root, canonical_input, path_error)) {
        return capture_failure(
            "invalid-capture-input", "capture-setup", path_error,
            request.output_directory);
    }

    const auto node = resolve_node(request.node_executable);
    if (!node || !is_executable_file(*node)) {
        return capture_failure(
            "capture-runtime-unavailable", "capture-setup",
            "Node.js was not found; browser capture needs Node.js 22 or newer",
            request.output_directory);
    }
    const auto script = resolve_capture_script(request.capture_script);
    if (!script || !fs::is_regular_file(*script)) {
        return capture_failure(
            "capture-runtime-unavailable", "capture-setup",
            "browser capture script was not found",
            request.output_directory);
    }

    std::string output_error;
    const auto output_directory =
        prepare_capture_output_directory(request.output_directory, output_error);
    if (!output_directory) {
        return capture_failure(
            "capture-output-error", "capture-setup",
            std::move(output_error));
    }

    std::string temp_error;
    auto profile = make_temp_directory("pulp-browser-capture", temp_error);
    if (!profile) {
        return capture_failure(
            "capture-profile-error", "capture-setup", temp_error,
            *output_directory);
    }

    std::vector<std::string> args{
        script->string(),
        "capture",
        "--browser", browser.executable.string(),
        "--browser-product", browser.product,
        "--browser-version", browser.version,
        "--browser-origin", browser_origin_name(browser.origin),
        "--input", canonical_input.string(),
        "--root", canonical_root.string(),
        "--output", output_directory->string(),
        "--profile-dir", profile->path.string(),
        "--initial-width", std::to_string(request.initial_width),
        "--initial-height", std::to_string(request.initial_height),
        "--dpr", std::to_string(request.device_scale_factor),
        "--timeout-ms", std::to_string(request.timeout_ms)};
    if (request.allow_network) args.push_back("--allow-network");

    platform::ProcessOptions process_options;
    // Keep the outer process deadline behind the capture runtime's deadline so
    // its SIGTERM/SIGKILL and profile cleanup path can finish.
    process_options.timeout_ms = outer_process_timeout(request.timeout_ms);
    process_options.max_output_bytes = 8 * 1024 * 1024;
    auto process = platform::ChildProcess::run(
        node->string(), args, process_options);
    const auto raw_stderr = process.stderr_output;
    process.stdout_output =
        sanitize_subprocess_output(std::move(process.stdout_output));
    process.stderr_output =
        sanitize_subprocess_output(std::move(process.stderr_output));

    if (process.timed_out) {
        auto failure = capture_failure(
            "browser-capture-timeout", "browser-capture",
            "browser capture timed out", *output_directory);
        failure.process = std::move(process);
        return failure;
    }
    if (process.exit_code != 0) {
        std::string message = "browser capture failed";
        const auto detail = one_line(process.stderr_output);
        if (!detail.empty()) message += ": " + detail;
        auto failure = capture_failure(
            capture_error_code(raw_stderr)
                .value_or("browser-capture-failed"),
            "browser-capture",
            std::move(message), *output_directory);
        failure.process = std::move(process);
        return failure;
    }

    CaptureArtifacts artifacts{
        *output_directory / "capture.json",
        *output_directory / "browser.png",
        *output_directory / "semantic-report.json",
        *output_directory / "tokens.json",
        *output_directory / "dom-snapshot.json"};
    const std::pair<const char*, fs::path> required[] = {
        {"capture envelope", artifacts.envelope},
        {"browser reference", artifacts.reference_png},
        {"semantic report", artifacts.semantic_report},
        {"token report", artifacts.token_report},
        {"DOM snapshot", artifacts.dom_snapshot},
    };
    for (const auto& [name, path] : required) {
        if (!nonempty_regular_file(path)) {
            auto failure = capture_failure(
                "browser-capture-incomplete", "capture-artifacts",
                std::string("browser capture did not emit a non-empty ")
                    + name,
                *output_directory);
            failure.process = std::move(process);
            return failure;
        }
    }

    CaptureResult result;
    result.artifacts = std::move(artifacts);
    result.process = std::move(process);
    return result;
}

DiscoverAndCaptureResult discover_and_capture(
    const BrowserDiscoveryOptions& discovery_options,
    const CaptureRequest& request,
    BrowserProbeFunction probe) {
    DiscoverAndCaptureResult result;
    result.discovery = discover_browser(discovery_options, std::move(probe));
    if (!result.discovery.selected) {
        result.capture.diagnostic = result.discovery.diagnostic;
        return result;
    }
    result.capture = capture_document(*result.discovery.selected, request);
    return result;
}

}  // namespace pulp::import_design::browser_capture
