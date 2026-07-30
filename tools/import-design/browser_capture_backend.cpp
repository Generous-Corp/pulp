// SPDX-License-Identifier: MIT
#include "browser_capture_backend.hpp"
#include "browser_capture_diagnostics.hpp"

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

constexpr int kMinimumNodeMajor = 22;
constexpr int kLauncherCleanupGraceMs = 5000;

int outer_process_timeout(int runtime_timeout_ms) {
    if (runtime_timeout_ms >
        std::numeric_limits<int>::max() - kLauncherCleanupGraceMs) {
        return std::numeric_limits<int>::max();
    }
    return runtime_timeout_ms + kLauncherCleanupGraceMs;
}

std::vector<std::string> declared_https_origins(const fs::path& staged_root) {
    // These candidates are authority, not discovery: the explicit network
    // opt-in may only reach origins literally present in the reviewed staged
    // document. The Node policy canonicalizes them to exact HTTPS origins.
    static const std::regex pattern(
        R"(https://(?:\[[0-9a-f:.]+\]|[a-z0-9.-]+)(?::[0-9]{1,5})?)",
        std::regex::ECMAScript | std::regex::icase);
    std::set<std::string> unique;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(staged_root, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        std::ifstream stream(it->path(), std::ios::binary);
        if (!stream) continue;
        const std::string source{
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
        for (auto match = std::sregex_iterator(
                 source.begin(), source.end(), pattern);
             match != std::sregex_iterator(); ++match) {
            const auto match_end = static_cast<std::size_t>(
                match->position() + match->length());
            // A regex prefix of https://user:password@host must not become an
            // authority candidate or leak credentials into child argv.
            if (match_end < source.size() &&
                (source[match_end] == ':' || source[match_end] == '@')) {
                continue;
            }
            unique.insert(match->str());
        }
    }
    return {unique.begin(), unique.end()};
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
#ifndef _WIN32
            fs::permissions(candidate, fs::perms::owner_all,
                            fs::perm_options::replace, ec);
            if (ec) {
                std::error_code cleanup_error;
                fs::remove_all(candidate, cleanup_error);
                error =
                    "could not restrict temporary browser profile permissions: "
                    + ec.message();
                return std::nullopt;
            }
#endif
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

bool parse_node_version(std::string_view output,
                        std::string& version,
                        int& major) {
    static const std::regex kVersionPattern{
        R"(\bv?([0-9]+(?:\.[0-9]+){1,3})\b)"};
    std::smatch match;
    const std::string line = one_line(std::string(output));
    if (!std::regex_search(line, match, kVersionPattern)) return false;
    version = match[1].str();
    try {
        major = std::stoi(match[1].str());
    } catch (...) {
        return false;
    }
    return major > 0;
}

Diagnostic discovery_diagnostic(
    const std::vector<BrowserProbeResult>& probes) {
    const auto has_failure = [&](BrowserProbeFailure kind) {
        return std::any_of(
            probes.begin(), probes.end(),
            [kind](const BrowserProbeResult& probe) {
                return probe.failure_kind == kind;
            });
    };
    if (has_failure(BrowserProbeFailure::node_unavailable)) {
        return {
            "node-unavailable",
            "Node.js was not found; faithful HTML import needs Node.js 22 or newer.",
            "runtime-discovery"};
    }
    if (has_failure(BrowserProbeFailure::node_incompatible)) {
        return {
            "node-incompatible",
            "The installed Node.js is too old; faithful HTML import needs Node.js 22 or newer.",
            "runtime-discovery"};
    }
    if (has_failure(BrowserProbeFailure::capture_runtime_unavailable)) {
        return {
            "capture-runtime-unavailable",
            "The Pulp browser-capture runtime is missing or incomplete.",
            "runtime-discovery"};
    }
    if (has_failure(BrowserProbeFailure::capture_capability_unavailable)) {
        return {
            "browser-capability-unavailable",
            "Chrome or Chromium does not provide the required headless capture capabilities.",
            "browser-discovery"};
    }
    if (has_failure(BrowserProbeFailure::browser_incompatible)) {
        return {
            "browser-incompatible",
            "Chrome or Chromium was found, but it is too old or incompatible.",
            "browser-discovery"};
    }
    return {
        "browser-unavailable",
        "No compatible Google Chrome or Chromium installation was found.",
        "browser-discovery"};
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
        << "  \"code\": \"" << detail::json_escape(diagnostic.code) << "\",\n"
        << "  \"phase\": \"" << detail::json_escape(diagnostic.phase) << "\",\n"
        << "  \"message\": \"" << detail::json_escape(diagnostic.message)
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

    constexpr std::array<std::string_view, 7> kGeneratedArtifacts{
        "capture.json",
        "browser.png",
        "semantic-report.json",
        "tokens.json",
        "dom-snapshot.json",
        "interaction-report.json",
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

BrowserProbeResult probe_browser(
    const BrowserCandidate& candidate,
    const BrowserDiscoveryOptions& options) {
    BrowserProbeResult result;
    result.candidate = candidate;

    if (!is_executable_file(candidate.executable)) {
        result.failure_kind = BrowserProbeFailure::browser_unavailable;
        result.failure = "browser executable does not exist or is not executable";
        return result;
    }

    platform::ProcessOptions version_options;
    version_options.timeout_ms = std::min(options.probe_timeout_ms, 5000);
    version_options.max_output_bytes = 64 * 1024;
    auto version_process = platform::ChildProcess::run(
        candidate.executable.string(), {"--version"}, version_options);
    if (version_process.timed_out) {
        result.failure_kind = BrowserProbeFailure::browser_incompatible;
        result.failure = "browser version probe timed out";
        return result;
    }
    if (version_process.exit_code != 0) {
        result.failure_kind = BrowserProbeFailure::browser_incompatible;
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
        result.failure_kind = BrowserProbeFailure::browser_incompatible;
        result.failure = "browser version output was not recognized";
        return result;
    }
    result.product = sanitize_subprocess_output(std::move(result.product));
    if (result.major_version < options.minimum_major) {
        result.failure_kind = BrowserProbeFailure::browser_incompatible;
        result.failure = "browser is too old (found "
            + std::to_string(result.major_version) + ", need "
            + std::to_string(options.minimum_major) + " or newer)";
        return result;
    }

    const auto node = resolve_node(options.node_executable);
    if (!node || !is_executable_file(*node)) {
        result.failure_kind = BrowserProbeFailure::node_unavailable;
        result.failure =
            "Node.js was not found; browser capture needs Node.js 22 or newer";
        return result;
    }
    platform::ProcessOptions node_version_options;
    node_version_options.timeout_ms = std::min(options.probe_timeout_ms, 5000);
    node_version_options.max_output_bytes = 64 * 1024;
    auto node_version_process = platform::ChildProcess::run(
        node->string(), {"--version"}, node_version_options);
    if (node_version_process.timed_out ||
        node_version_process.exit_code != 0) {
        result.failure_kind = BrowserProbeFailure::node_incompatible;
        result.failure = "Node.js version probe failed";
        return result;
    }
    const auto node_version_output =
        !node_version_process.stdout_output.empty()
            ? node_version_process.stdout_output
            : node_version_process.stderr_output;
    std::string node_version;
    int node_major = 0;
    if (!parse_node_version(node_version_output, node_version, node_major)) {
        result.failure_kind = BrowserProbeFailure::node_incompatible;
        result.failure = "Node.js version output was not recognized";
        return result;
    }
    if (node_major < kMinimumNodeMajor) {
        result.failure_kind = BrowserProbeFailure::node_incompatible;
        result.failure = "Node.js is too old (found " + node_version +
            ", need 22 or newer)";
        return result;
    }
    const auto script = resolve_capture_script(options.capture_script);
    if (!script || !fs::is_regular_file(*script)) {
        result.failure_kind =
            BrowserProbeFailure::capture_runtime_unavailable;
        result.failure = "browser capture script was not found";
        return result;
    }

    std::string temp_error;
    auto profile = make_temp_directory("pulp-browser-probe", temp_error);
    if (!profile) {
        result.failure_kind =
            BrowserProbeFailure::capture_capability_unavailable;
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
        result.failure_kind =
            BrowserProbeFailure::capture_capability_unavailable;
        result.failure = "browser CDP capability probe timed out";
        return result;
    }
    if (process.exit_code != 0) {
        result.failure_kind =
            BrowserProbeFailure::capture_capability_unavailable;
        result.failure = "browser does not provide the required headless/CDP "
                         "capture capabilities";
        const auto detail = one_line(process.stderr_output);
        if (!detail.empty()) result.failure += ": " + detail;
        return result;
    }

    result.compatible = true;
    result.failure_kind = BrowserProbeFailure::none;
    return result;
}

BrowserDiscoveryResult discover_browser(
    const BrowserDiscoveryOptions& options,
    BrowserProbeFunction probe) {
    BrowserDiscoveryResult result;
    const bool has_explicit =
        options.explicit_path && !options.explicit_path->empty();
    const bool has_path_environment =
        options.environment_override
            ? !options.environment_override->empty()
            : (std::getenv("PULP_DESIGN_BROWSER") != nullptr &&
               *std::getenv("PULP_DESIGN_BROWSER") != '\0');
    const auto mode = resolve_browser_mode(options);
    if (!has_explicit && !has_path_environment && !mode.mode) {
        result.diagnostic = {
            "browser-mode-invalid", mode.error, "browser-discovery"};
        return result;
    }
    auto candidates = collect_browser_candidates(options);
    if (candidates.empty() && !has_explicit && !has_path_environment &&
        mode.mode == BrowserMode::managed) {
        result.diagnostic = {
            "managed-browser-unavailable",
            "Managed Chrome for Testing is selected but is not installed or "
            "its current.json is invalid.",
            "browser-discovery"};
        return result;
    }
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

    result.diagnostic = discovery_diagnostic(result.probes);
    return result;
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
    if (request.interaction_plan) {
        args.push_back("--interactions");
        args.push_back(request.interaction_plan->string());
    }
    if (request.allow_network) args.push_back("--allow-network");
    if (request.allow_network) {
        for (const auto& origin : declared_https_origins(canonical_root)) {
            args.push_back("--declared-network-origin");
            args.push_back(origin);
        }
    }

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
        *output_directory / "dom-snapshot.json",
        request.interaction_plan
            ? std::optional<fs::path>{
                  *output_directory / "interaction-report.json"}
            : std::nullopt};
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
    if (artifacts.interaction_report &&
        !nonempty_regular_file(*artifacts.interaction_report)) {
        auto failure = capture_failure(
            "browser-capture-incomplete", "capture-artifacts",
            "browser capture did not emit a non-empty interaction report",
            *output_directory);
        failure.process = std::move(process);
        return failure;
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
