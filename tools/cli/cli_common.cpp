// cli_common.cpp — Shared implementations for the Pulp CLI

#include "cli_common.hpp"

#include "fetchcontent_cache.hpp"
#include "version_diag.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <regex>
#include <sstream>
#include <thread>
#include <utility>

#include <pulp/runtime/system.hpp>
#include <choc/text/choc_StringUtilities.h>
#include <choc/text/choc_Files.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>  // _NSGetExecutablePath
#endif

#ifdef _WIN32
#include <pulp/platform/win32_sane.hpp>  // GetModuleFileNameA, MAX_PATH
#include <io.h>       // _isatty, _fileno
#include <process.h>  // _getpid
#define popen _popen
#define pclose _pclose
#define getpid _getpid
#else
#include <unistd.h>  // isatty, getpid
#endif

#include "pulp_version_gen.h"
#include "package_registry.hpp"
#include "update_check.hpp"

// ── SDK Constants ────────────────���──────────────────────────────────────────

const char* PULP_SDK_VERSION = PULP_SDK_VERSION_GENERATED;
const char* PULP_GITHUB_REPO = "danielraffel/pulp";

// ── Color / Terminal ──���────────────────────���────────────────────────────���───

bool g_color_enabled = true;
bool g_no_color = false;

bool is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

void init_color() {
    if (g_no_color) { g_color_enabled = false; return; }
    if (pulp::runtime::get_env("NO_COLOR")) { g_color_enabled = false; return; }
    g_color_enabled = is_tty();
}

namespace color {
std::string reset()   { return g_color_enabled ? "\033[0m"  : ""; }
std::string bold()    { return g_color_enabled ? "\033[1m"  : ""; }
std::string dim()     { return g_color_enabled ? "\033[2m"  : ""; }
std::string green()   { return g_color_enabled ? "\033[32m" : ""; }
std::string yellow()  { return g_color_enabled ? "\033[33m" : ""; }
std::string red()     { return g_color_enabled ? "\033[31m" : ""; }
std::string cyan()    { return g_color_enabled ? "\033[36m" : ""; }
}

void print_ok(const std::string& msg) {
    std::cout << "  " << color::green() << "\xe2\x9c\x93" << color::reset() << " " << msg << "\n";
}
void print_fail(const std::string& msg) {
    std::cout << "  " << color::red() << "\xe2\x9c\x97" << color::reset() << " " << msg << "\n";
}
void print_warn(const std::string& msg) {
    std::cout << "  " << color::yellow() << "\xe2\x9a\xa0" << color::reset() << " " << msg << "\n";
}

// Timestamped phase marker for the `PULP_DEBUG=1` env. Silent by default.
// Use at the top of any user-entrypoint phase that could plausibly hang,
// so the next "`pulp` at 0% CPU with no output" report (#682) pins itself:
// the user re-runs with PULP_DEBUG=1, and the last line printed before the
// hang is the phase we blocked in.
void pulp_debug(const char* phase) {
    static const bool enabled = []() {
        auto v = pulp::runtime::get_env("PULP_DEBUG");
        return v && !v->empty() && *v != "0" && *v != "false";
    }();
    if (!enabled) return;
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        clock::now() - start).count();
    std::cerr << "[pulp-debug " << std::setw(7) << ms << "ms] " << phase << "\n"
              << std::flush;
}

// ── Shell Execution ───��─────────────────────────────────────────────────────

int run(const std::string& cmd) {
    return std::system(cmd.c_str());
}

std::string shell_quote(const std::string& s) {
#ifdef _WIN32
    // pulp #776: on Windows, MSVCRT argv parsing treats backslashes
    // literally unless they immediately precede a double quote. Unix-
    // style escaping (`\` -> `\\`) breaks file paths: `git clone
    // "C:\\Users\\foo\\origin.git" feature` writes the doubled-backslash
    // URL into feature/.git/config; the next `git fetch origin main`
    // looks up a path that doesn't exist, fails, and `bump_one` skips
    // the origin/main redundancy gate it relied on for `"skipped"`.
    //
    // Implementation follows the canonical MSVCRT-correct algorithm
    // (Microsoft docs, Daniel Colascione's "Everyone quotes command
    // line arguments the wrong way"):
    //
    //   - Backslashes NOT followed by `"` are literal.
    //   - To embed a literal `"`, prefix with one extra backslash and
    //     double every backslash already in the run before it.
    //   - Trailing backslashes before the closing `"` must be doubled
    //     so they don't escape it.
    //
    // cmd.exe's own metacharacters (`&`, `|`, `<`, `>`, `%`, `!`, `^`)
    // are inert inside `"..."`, so wrapping is enough to neutralize them.
    std::string out = "\"";
    int backslashes = 0;
    for (char c : s) {
        if (c == '\\') {
            backslashes++;
        } else if (c == '"') {
            out.append(static_cast<std::size_t>(backslashes) * 2 + 1, '\\');
            out += '"';
            backslashes = 0;
        } else {
            out.append(static_cast<std::size_t>(backslashes), '\\');
            out += c;
            backslashes = 0;
        }
    }
    out.append(static_cast<std::size_t>(backslashes) * 2, '\\');
    out += "\"";
    return out;
#else
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    out += "\"";
    return out;
#endif
}

std::string shell_quote(const fs::path& p) {
    return shell_quote(p.string());
}

fs::path platform_executable(fs::path p) {
#ifdef _WIN32
    if (p.extension() != ".exe") p += ".exe";
#endif
    return p;
}

static bool parse_uint64_arg(const std::string& text, const char* flag, uint64_t& out) {
    if (text.empty()) {
        std::cerr << "Error: invalid value for " << flag << ": " << text << "\n";
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const auto value = std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end != text.c_str() + text.size()) {
        std::cerr << "Error: invalid value for " << flag << ": " << text << "\n";
        return false;
    }

    out = static_cast<uint64_t>(value);
    return true;
}

bool parse_size_arg(const std::string& text, const char* flag, std::size_t& out) {
    uint64_t value = 0;
    if (!parse_uint64_arg(text, flag, value))
        return false;
    if (value > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
        std::cerr << "Error: invalid value for " << flag << ": " << text << "\n";
        return false;
    }

    out = static_cast<std::size_t>(value);
    return true;
}

bool parse_double_arg(const std::string& text, const char* flag, double& out) {
    if (text.empty()) {
        std::cerr << "Error: invalid value for " << flag << ": " << text << "\n";
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const auto value = std::strtod(text.c_str(), &end);
    if (errno == ERANGE || end != text.c_str() + text.size() || !std::isfinite(value)) {
        std::cerr << "Error: invalid value for " << flag << ": " << text << "\n";
        return false;
    }

    out = value;
    return true;
}

int run_with_spinner(const std::string& cmd, const std::string& label) {
    if (!is_tty() || g_no_color) {
        std::cout << label << "...\n";
        return run(cmd);
    }

    // Redirect output to temp file so it doesn't interleave with spinner
#ifdef _WIN32
    std::string tmp = pulp::runtime::get_env("TEMP").value_or(".") + "\\pulp-spinner-" + std::to_string(getpid()) + ".log";
#else
    std::string tmp = "/tmp/pulp-spinner-" + std::to_string(getpid()) + ".log";
#endif
    std::string redirected = cmd + " >" + tmp + " 2>&1";

    std::atomic<bool> done{false};
    std::atomic<int> result{0};

    std::thread worker([&]() {
        result = std::system(redirected.c_str());
        done = true;
    });

    const char* frames[] = {"\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
                            "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
                            "\xe2\xa0\x87", "\xe2\xa0\x8f"};
    int frame = 0;
    auto start = std::chrono::steady_clock::now();

    while (!done) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << "\r  " << color::cyan() << frames[frame % 10] << color::reset()
                  << " " << label << color::dim() << " (" << elapsed << "s)" << color::reset() << "  " << std::flush;
        frame++;
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }

    worker.join();

    // Clear spinner line
    std::cout << "\r\033[K";
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    if (result == 0) {
        print_ok(label + color::dim() + " (" + std::to_string(elapsed) + "s)" + color::reset());
    } else {
        print_fail(label + color::dim() + " (" + std::to_string(elapsed) + "s)" + color::reset());
        // Show last 20 lines of captured output on failure
#ifdef _WIN32
        std::string tail_cmd = "powershell -NoProfile -Command \"Get-Content -Path "
                             + shell_quote(tmp) + " -Tail 20\"";
#else
        std::string tail_cmd = "tail -20 " + tmp;
#endif
        run(tail_cmd);
    }

    std::remove(tmp.c_str());
    return result;
}

static std::string command_with_user_local_bin(std::string cmd) {
#if !defined(_WIN32)
    if (const char* home = std::getenv("HOME")) {
        auto local_bin = fs::path(home) / ".local" / "bin";
        if (fs::is_directory(local_bin)) {
            cmd = "PATH=" + shell_quote(local_bin.string()) + ":$PATH " + cmd;
        }
    }
#endif
    return cmd;
}

std::string exec_output(const std::string& cmd) {
    std::string result;
    auto prepared_cmd = command_with_user_local_bin(cmd);
    FILE* pipe = popen(prepared_cmd.c_str(), "r");
    if (!pipe) return {};
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;
    }
    pclose(pipe);
    // Trim trailing newline
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

// ── String Utilities ───────────────���───────────────────────────────────���────

std::string trim(const std::string& s) {
    return std::string(choc::text::trim(s));
}

std::string strip_quotes(const std::string& s) {
    if (s.size() >= 2) {
        if ((s.front() == '"' && s.back() == '"') ||
            (s.front() == '\'' && s.back() == '\'')) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}

std::string read_file_contents(const fs::path& path) {
    return choc::file::loadFileAsString(path.string());
}

std::string replace_all_str(const std::string& str,
                            const std::string& from,
                            const std::string& to) {
    return choc::text::replace(str, from, to);
}

bool icontains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    return it != haystack.end();
}

std::string yaml_value(const std::string& line, const std::string& key) {
    auto pos = line.find(key + ":");
    if (pos == std::string::npos) return {};
    auto val_start = pos + key.size() + 1;
    if (val_start >= line.size()) return {};
    return trim(line.substr(val_start));
}

std::string sanitize_process_output(std::string output) {
    output.erase(std::remove(output.begin(), output.end(), '\0'), output.end());
    return output;
}

std::string truncate_message(std::string value, std::size_t max_chars) {
    if (value.size() <= max_chars) return value;
    value.resize(max_chars);
    value += "...";
    return value;
}

// ── Path / Project Detection ────────────────────────────────────────────────

fs::path user_home_dir() {
    if (auto home = pulp::runtime::get_env("HOME")) {
        return fs::path(*home);
    }
#ifdef _WIN32
    if (auto home = pulp::runtime::get_env("USERPROFILE")) {
        return fs::path(*home);
    }
#endif
    return {};
}

std::string find_executable_in_path(const std::string& name) {
#ifdef _WIN32
    auto output = exec_output("where " + shell_quote(name) + " 2>nul");
#else
    auto output = exec_output("command -v " + shell_quote(name) + " 2>/dev/null");
#endif
    if (output.empty()) return {};
    auto newline = output.find_first_of("\r\n");
    return newline == std::string::npos ? output : output.substr(0, newline);
}

fs::path find_project_root_from(fs::path dir) {
    if (fs::is_regular_file(dir)) dir = dir.parent_path();
    if (dir.empty()) dir = fs::current_path();
    while (!dir.empty()) {
        if (fs::exists(dir / "CMakeLists.txt") && fs::exists(dir / "core")) {
            return dir;
        }
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

fs::path find_project_root() {
    return find_project_root_from(fs::current_path());
}

fs::path current_executable_path() {
#ifdef __APPLE__
    char buf[1024];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        auto canonical = fs::canonical(buf, ec);
        return ec ? fs::path(buf) : canonical;
    }
#elif defined(__linux__)
    std::error_code ec;
    auto canonical = fs::canonical("/proc/self/exe", ec);
    if (!ec) return canonical;
#elif defined(_WIN32)
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::error_code ec;
    auto canonical = fs::canonical(buf, ec);
    return ec ? fs::path(buf) : canonical;
#endif
    return {};
}

fs::path cmake_home_directory(const fs::path& build_dir) {
    auto cache = build_dir / "CMakeCache.txt";
    if (!fs::exists(cache)) return {};

    std::ifstream in(cache);
    std::string line;
    const std::string prefix = "CMAKE_HOME_DIRECTORY:INTERNAL=";
    while (std::getline(in, line)) {
        if (line.rfind(prefix, 0) == 0) {
            return fs::path(line.substr(prefix.size()));
        }
    }
    return {};
}

fs::path build_dir_from_current_binary() {
    auto self = current_executable_path();
    if (self.empty()) return {};

    auto dir = self.parent_path();
    while (!dir.empty()) {
        if (fs::exists(dir / "CMakeCache.txt")) {
            return dir;
        }
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }

    return {};
}

static fs::path source_checkout_root_from_current_binary() {
    auto build_dir = build_dir_from_current_binary();
    if (build_dir.empty()) return {};

    auto root = cmake_home_directory(build_dir);
    if (root.empty()) return {};
    if (!fs::exists(root / "CMakeLists.txt")) return {};
    if (!fs::exists(root / "core")) return {};
    return root;
}

fs::path find_standalone_root() {
    auto dir = fs::current_path();
    while (!dir.empty()) {
        if (fs::exists(dir / "pulp.toml") && !fs::exists(dir / "core")) {
            return dir;
        }
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

fs::path resolve_active_project_root(bool* is_standalone) {
    auto standalone_root = find_standalone_root();
    if (!standalone_root.empty()) {
        if (is_standalone) *is_standalone = true;
        return standalone_root;
    }

    auto root = find_project_root();
    if (is_standalone) *is_standalone = false;
    return root;
}

std::optional<fs::path> require_project_root() {
    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return std::nullopt;
    }
    return root;
}

std::optional<fs::path> require_active_project_root(bool* is_standalone) {
    auto root = resolve_active_project_root(is_standalone);
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return std::nullopt;
    }
    return root;
}

bool path_is_within(const fs::path& path, const fs::path& root) {
    auto normalized_path = fs::absolute(path).lexically_normal();
    auto normalized_root = fs::absolute(root).lexically_normal();

    auto path_it = normalized_path.begin();
    auto root_it = normalized_root.begin();
    for (; root_it != normalized_root.end(); ++root_it, ++path_it) {
        if (path_it == normalized_path.end() || *path_it != *root_it) return false;
    }

    return true;
}

fs::path resolve_create_projects_base_dir(const fs::path& repo_root) {
    auto home_env = pulp::runtime::get_env("HOME");
#ifdef _WIN32
    if (!home_env) home_env = pulp::runtime::get_env("USERPROFILE");
#endif
    fs::path user_home = home_env ? fs::path(*home_env) : fs::path{};

    if (auto env_projects_dir = pulp::runtime::get_env("PULP_PROJECTS_DIR")) {
        auto configured = strip_quotes(trim(*env_projects_dir));
        if (!configured.empty()) {
            auto path = fs::path(configured);
            if (!path.empty() && path.string()[0] == '~' && !user_home.empty()) {
                path = configured == "~"
                    ? user_home
                    : user_home / configured.substr(2);
            }
            return path.is_absolute() ? path : fs::absolute(path);
        }
    }

    auto config_projects_dir = read_user_config_value("create", "projects_dir");
    if (!config_projects_dir.empty()) {
        auto path = fs::path(config_projects_dir);
        if (!path.empty() && path.string()[0] == '~' && !user_home.empty()) {
            path = config_projects_dir == "~"
                ? user_home
                : user_home / config_projects_dir.substr(2);
        }
        return path.is_absolute() ? path : fs::absolute(path);
    }

    if (!repo_root.empty()) return repo_root.parent_path();
    return fs::current_path();
}

// ── SDK / Config ──────────────��────────────────────────────���────────────────

fs::path pulp_home() {
    if (auto pulp_home_env = pulp::runtime::get_env("PULP_HOME"))
        return fs::path(*pulp_home_env);

    auto home = pulp::runtime::get_env("HOME");
#ifdef _WIN32
    if (!home) home = pulp::runtime::get_env("USERPROFILE");
#endif
    if (!home) return {};
    return fs::path(*home) / ".pulp";
}

fs::path sdk_cache_path(const std::string& version) {
    return pulp_home() / "sdk" / version;
}

static void remove_tree(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

fs::path local_sdk_cache_path(const std::string& version) {
    return pulp_home() / "sdk-local" / detect_platform() / version;
}

// pulp #1814 — sdk_tarball_filename / legacy_unversioned_sdk_tarball_filename
// live in tools/cli/sdk_cache_paths.cpp so the matching unit test can
// compile + link them standalone.

std::string detect_platform() {
#ifdef __APPLE__
    #if defined(__aarch64__) || defined(__arm64__)
        return "darwin-arm64";
    #else
        return "darwin-x64";
    #endif
#elif defined(_WIN32)
    #if defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
        return "windows-arm64";
    #else
        return "windows-x64";
    #endif
#elif defined(__linux__)
    #if defined(__aarch64__)
        return "linux-arm64";
    #else
        return "linux-x64";
    #endif
#else
    return "unknown";
#endif
}

// ── #2087 follow-up: newer-SDK-available banner ─────────────────────────
//
// Pulp #2087 follow-up (#22): emit a one-line banner when a newer SDK
// is available on GitHub Releases. To keep CLI invocations fast and
// avoid hitting GitHub on every command, we cache the result at
// `~/.pulp/cache/latest_release.txt` with a 24h TTL. Cache is plain
// text — first line is the version string (no leading `v`), second
// line is the Unix timestamp of the fetch. Cache miss / stale →
// opportunistic refresh via curl with a hard 2s timeout so a slow
// network never blocks the user.

namespace {

constexpr int kLatestReleaseCacheTtlSeconds = 24 * 60 * 60;

fs::path latest_release_cache_path() {
    auto home = pulp_home();
    return home.empty() ? fs::path{} : (home / "cache" / "latest_release.txt");
}

bool semver_strictly_greater(const std::string& a, const std::string& b) {
    // Compare two `X.Y.Z` strings. Returns true iff a > b. Tolerates
    // a leading `v` on either side and any pre-release suffix (which
    // we ignore — pre-releases are intentionally not surfaced by the
    // banner, the user opted into them by installing).
    auto parse = [](std::string s) -> std::array<int, 3> {
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
        auto dash = s.find('-');
        if (dash != std::string::npos) s.resize(dash);
        std::array<int, 3> out{0, 0, 0};
        std::sscanf(s.c_str(), "%d.%d.%d", &out[0], &out[1], &out[2]);
        return out;
    };
    auto pa = parse(a);
    auto pb = parse(b);
    return pa > pb;
}

}  // namespace

std::string latest_available_sdk_version() {
    auto cache = latest_release_cache_path();
    if (cache.empty()) return {};

    // Try cache first.
    if (fs::exists(cache)) {
        std::ifstream in(cache);
        std::string version_line, timestamp_line;
        std::getline(in, version_line);
        std::getline(in, timestamp_line);
        try {
            auto ts = std::stoll(timestamp_line);
            auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (!version_line.empty()
                && (now - ts) < kLatestReleaseCacheTtlSeconds) {
                return version_line;
            }
        } catch (...) {
            // Bad cache — fall through to refresh.
        }
    }

    // Refresh — 2s timeout so a slow network never costs the user
    // more than ~2 seconds. Failure is silent (returns empty); the
    // banner just doesn't print this run.
    std::string url = "https://api.github.com/repos/"
                      + std::string(PULP_GITHUB_REPO)
                      + "/releases/latest";
    std::string cmd = "curl -fsSL --max-time 2 -H 'Accept: application/vnd.github+json' "
                      + shell_quote(url) + " 2>/dev/null";
    // Codex P1 on PR #2138: mirror the _WIN32 popen/pclose mapping used
    // elsewhere in tools/cli/ so this builds on Windows.
#if defined(_WIN32)
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return {};
    std::string body;
    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof(buf), pipe)) body.append(buf, n);
#if defined(_WIN32)
    if (_pclose(pipe) != 0 || body.empty()) return {};
#else
    if (pclose(pipe) != 0 || body.empty()) return {};
#endif

    // Custom raw-string delimiter `RE` — the default `R"(...)"` form
    // would close at the first `)"` inside the regex (after the
    // version-capture group), splitting the literal. CI on GCC caught
    // this. The delimiter eliminates the ambiguity.
    std::regex tag_re(R"RE("tag_name"\s*:\s*"v?([0-9]+\.[0-9]+\.[0-9]+)")RE");
    std::smatch m;
    if (!std::regex_search(body, m, tag_re)) return {};
    std::string version = m[1].str();

    // Write cache (best-effort; ignore failures).
    std::error_code ec;
    fs::create_directories(cache.parent_path(), ec);
    if (!ec) {
        std::ofstream out(cache);
        if (out) {
            auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            out << version << "\n" << now << "\n";
        }
    }
    return version;
}

void maybe_print_newer_sdk_banner(const std::string& installed) {
    if (installed.empty()) return;
    auto latest = latest_available_sdk_version();
    if (latest.empty()) return;
    if (!semver_strictly_greater(latest, installed)) return;
    std::cout << "  (Note: Pulp SDK v" << latest
              << " is available — installed: v" << installed
              << ". Run `pulp upgrade` or `pulp sdk install --version "
              << latest << "` to update.)\n";
}

fs::path ensure_sdk(const std::string& version) {
    auto sdk_dir = sdk_cache_path(version);

    // Already cached?
    if (fs::exists(sdk_dir / "version.txt")) {
        return sdk_dir;
    }

    auto platform = detect_platform();
    if (platform == "unknown") {
        std::cerr << "Error: unsupported platform for SDK download.\n";
        return {};
    }

    std::string ext = (platform.find("windows") != std::string::npos) ? "tar.gz" : "tar.gz";
    std::string tarball = "pulp-sdk-" + platform + ".tar.gz";
    std::string url = "https://github.com/" + std::string(PULP_GITHUB_REPO)
                    + "/releases/download/v" + version + "/" + tarball;

    std::cout << "Downloading Pulp SDK v" << version << " (" << platform << ")...\n";

    // Create cache directory
    fs::create_directories(sdk_dir);

    std::string tmp_dir = "/tmp/pulp-sdk-download-" + version;
#ifdef _WIN32
    tmp_dir = pulp::runtime::get_env("TEMP").value_or(".") + "\\pulp-sdk-download-" + version;
#endif
    fs::create_directories(tmp_dir);

    // Download
    std::string download_cmd;
#ifdef _WIN32
    download_cmd = "powershell -Command \"Invoke-WebRequest -Uri '" + url
                 + "' -OutFile '" + tmp_dir + "\\" + tarball + "'\"";
#else
    download_cmd = "mkdir -p " + tmp_dir + " && curl -fSL -o " + tmp_dir + "/" + tarball + " " + url;
#endif

    int rc = run_with_spinner(download_cmd, "Downloading SDK");
    if (rc != 0) {
        std::cerr << "Error: failed to download SDK from:\n  " << url << "\n";
        std::cerr << "Check your internet connection or download manually.\n";
        fs::remove_all(sdk_dir);
        remove_tree(tmp_dir);
        return {};
    }

    // Extract
    std::string extract_cmd;
#ifdef _WIN32
    extract_cmd = "tar -xzf \"" + tmp_dir + "\\" + tarball + "\" -C \"" + tmp_dir + "\"";
#else
    extract_cmd = "tar -xzf " + tmp_dir + "/" + tarball + " -C " + tmp_dir;
#endif

    rc = run_with_spinner(extract_cmd, "Extracting SDK");
    if (rc != 0) {
        std::cerr << "Error: failed to extract SDK archive.\n";
        fs::remove_all(sdk_dir);
        remove_tree(tmp_dir);
        return {};
    }

    // Move extracted SDK into cache location
    auto extracted = fs::path(tmp_dir) / "pulp-sdk";
    if (!fs::exists(extracted)) {
        extracted = fs::path(tmp_dir);
    }

    try {
        for (auto& entry : fs::directory_iterator(extracted)) {
            auto dest = sdk_dir / entry.path().filename();
            if (entry.is_directory()) {
                fs::copy(entry.path(), dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            } else {
                fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error installing SDK to cache: " << e.what() << "\n";
        fs::remove_all(sdk_dir);
        remove_tree(tmp_dir);
        return {};
    }

    remove_tree(tmp_dir);

    if (!fs::exists(sdk_dir / "version.txt")) {
        std::cerr << "Error: SDK installation incomplete — version.txt not found.\n";
        fs::remove_all(sdk_dir);
        return {};
    }

    print_ok("SDK v" + version + " cached at " + sdk_dir.string());
    return sdk_dir;
}

int ensure_checkout_dependencies(const fs::path& repo_root) {
    auto script = repo_root /
#ifdef _WIN32
        "setup.ps1";
#else
        "setup.sh";
#endif
    if (!fs::exists(script)) {
        std::cerr << "Error: bootstrap script not found in checkout at " << repo_root.string() << "\n";
        return 1;
    }

#ifdef _WIN32
    std::string cmd = "powershell -NoProfile -ExecutionPolicy Bypass -File "
        + shell_quote(script) + " --deps-only";
#else
    std::string cmd = "cd " + shell_quote(repo_root) + " && ./setup.sh --deps-only";
#endif
    return run_with_spinner(cmd, "Preparing checkout dependencies");
}

fs::path ensure_checkout_sdk(const fs::path& repo_root, const std::string& version) {
    auto sdk_dir = local_sdk_cache_path(version);
    auto config = sdk_dir / "lib" / "cmake" / "Pulp" / "PulpConfig.cmake";
    if (fs::exists(config) && fs::exists(sdk_dir / "version.txt")) {
        return sdk_dir;
    }

    if (ensure_checkout_dependencies(repo_root) != 0) {
        return {};
    }

    auto build_dir = pulp_home() / "sdk-build" / (detect_platform() + "-" + version);
    fs::create_directories(build_dir.parent_path());
    fs::create_directories(sdk_dir);

    std::string configure_cmd = "cmake -S " + repo_root.string()
        + " -B " + build_dir.string()
        + " -DCMAKE_BUILD_TYPE=Release"
        + " -DCMAKE_INSTALL_PREFIX=" + sdk_dir.string()
        + " -DPULP_BUILD_TESTS=OFF"
        + " -DPULP_BUILD_EXAMPLES=OFF"
        + " -DPULP_ENABLE_GPU=OFF";
    append_windows_visual_studio_generator_args(configure_cmd);
    if (run_with_spinner(configure_cmd, "Configuring local SDK") != 0) {
        return {};
    }

    std::string install_cmd = "cmake --build " + build_dir.string() + " --target install --parallel";
    if (run_with_spinner(install_cmd, "Installing local SDK") != 0) {
        return {};
    }

    if (!fs::exists(config)) {
        std::cerr << "Error: local SDK installation incomplete �� " << config.string() << " not found.\n";
        return {};
    }

    print_ok("Local SDK cached at " + sdk_dir.string());
    return sdk_dir;
}

std::string read_pulp_toml_value(const fs::path& project_root, const std::string& key) {
    auto toml_path = project_root / "pulp.toml";
    if (!fs::exists(toml_path)) return {};

    std::ifstream f(toml_path);
    std::string line;
    while (std::getline(f, line)) {
        auto pos = line.find(key);
        if (pos != std::string::npos) {
            auto q1 = line.find('"', pos);
            auto q2 = line.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                return line.substr(q1 + 1, q2 - q1 - 1);
            }
        }
    }
    return {};
}

// Return the newest semver triple under ~/.pulp/sdk/<x.y.z>/. Skips
// directories whose name doesn't parse as semver, and skips entries
// without a version.txt (those are interrupted installs). Used by
// the "latest" floating-SDK resolution in read_sdk_version.
std::string newest_installed_sdk() {
    auto home = pulp_home();
    if (home.empty()) return {};
    auto sdk_base = home / "sdk";
    if (!fs::exists(sdk_base)) return {};

    auto parse_triple = [](const std::string& s, int& maj, int& min, int& patch) -> bool {
        maj = min = patch = -1;
        std::size_t i = 0;
        auto eat = [&](int& out) {
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
            int v = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                v = v * 10 + (s[i] - '0'); ++i;
            }
            out = v; return true;
        };
        if (!eat(maj)) return false;
        if (i >= s.size() || s[i] != '.') return false; ++i;
        if (!eat(min)) return false;
        if (i >= s.size() || s[i] != '.') return false; ++i;
        if (!eat(patch)) return false;
        return true;
    };

    std::string best;
    int bM = -1, bm = -1, bp = -1;
    for (auto& entry : fs::directory_iterator(sdk_base)) {
        if (!entry.is_directory()) continue;
        auto ver = entry.path().filename().string();
        int M, m, p;
        if (!parse_triple(ver, M, m, p)) continue;
        if (!fs::exists(entry.path() / "version.txt")) continue;
        bool newer = best.empty() || M > bM ||
                     (M == bM && m > bm) ||
                     (M == bM && m == bm && p > bp);
        if (newer) { best = ver; bM = M; bm = m; bp = p; }
    }
    return best;
}

std::string read_raw_sdk_version(const fs::path& project_root) {
    return read_pulp_toml_value(project_root, "sdk_version");
}

bool is_floating_sdk(const fs::path& project_root) {
    auto raw = read_raw_sdk_version(project_root);
    if (raw.empty()) return false;  // no project / no pin = not floating
    // Case-insensitive compare against "latest"
    std::string lower;
    lower.reserve(raw.size());
    for (char c : raw) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower == "latest";
}

std::string read_sdk_version(const fs::path& project_root) {
    auto version = read_raw_sdk_version(project_root);
    // Empty (no pulp.toml or no sdk_version key) preserves pre-#2087
    // behavior: fall back to the CLI's own SDK version. This is the
    // "no project at all" path — read_sdk_version is called from
    // contexts that have no project root, and returning newest-installed
    // there would surprise downstream code that expects PULP_SDK_VERSION.
    if (version.empty()) return PULP_SDK_VERSION;
    // Pulp #2087 floating mode: ONLY an explicit `sdk_version = "latest"`
    // resolves to the newest installed SDK at command time. The CLI's
    // own SDK version is the final fallback when no SDKs are installed
    // under ~/.pulp/sdk/ yet — common on a fresh `curl install.sh`
    // machine before the user has run `pulp sdk install`.
    std::string lower;
    lower.reserve(version.size());
    for (char c : version) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "latest") {
        auto resolved = newest_installed_sdk();
        if (!resolved.empty()) return resolved;
        return PULP_SDK_VERSION;
    }
    return version;
}

fs::path read_sdk_path_hint(const fs::path& project_root) {
    auto value = read_pulp_toml_value(project_root, "sdk_path");
    return value.empty() ? fs::path{} : fs::path(value);
}

fs::path read_sdk_checkout_hint(const fs::path& project_root) {
    auto value = read_pulp_toml_value(project_root, "sdk_checkout");
    return value.empty() ? fs::path{} : fs::path(value);
}

static bool sdk_config_exists(const fs::path& sdk_dir) {
    if (sdk_dir.empty()) return false;
    return fs::exists(sdk_dir / "lib" / "cmake" / "Pulp" / "PulpConfig.cmake");
}

static std::string read_sdk_dir_version(const fs::path& sdk_dir) {
    auto version_file = sdk_dir / "version.txt";
    if (!fs::exists(version_file)) return {};
    std::ifstream f(version_file);
    if (!f.is_open()) return {};
    std::string line;
    std::getline(f, line);
    return trim(line);
}

StandaloneSdkResolution resolve_standalone_sdk(const fs::path& project_root,
                                               bool materialize) {
    StandaloneSdkResolution out;
    out.requested_version = read_sdk_version(project_root);
    out.sdk_path_hint = read_sdk_path_hint(project_root);
    out.sdk_checkout_hint = read_sdk_checkout_hint(project_root);

    if (!out.sdk_path_hint.empty()) {
        out.sdk_path_config_ready = sdk_config_exists(out.sdk_path_hint);
        if (out.sdk_path_config_ready) {
            out.sdk_path_version = read_sdk_dir_version(out.sdk_path_hint);
            out.sdk_path_version_known = !out.sdk_path_version.empty();
            if (out.sdk_path_version_known) {
                auto actual = out.sdk_path_version;
                if (!actual.empty() && (actual.front() == 'v' || actual.front() == 'V')) {
                    actual.erase(0, 1);
                }
                out.sdk_path_version_matches = actual == out.requested_version;
                if (out.sdk_path_version_matches) {
                    out.resolved_sdk_dir = out.sdk_path_hint;
                    out.used_sdk_path_hint = true;
                    return out;
                }
                out.warning = "sdk_path points at SDK v" + actual +
                              " but pulp.toml requests v" +
                              out.requested_version + "; ignoring sdk_path";
            } else {
                out.sdk_path_custom_unverifiable = true;
                out.resolved_sdk_dir = out.sdk_path_hint;
                out.used_sdk_path_hint = true;
                return out;
            }
        }
    }

    if (!out.sdk_checkout_hint.empty() && fs::exists(out.sdk_checkout_hint)) {
        if (materialize) {
            out.resolved_sdk_dir = ensure_checkout_sdk(out.sdk_checkout_hint,
                                                       out.requested_version);
            if (!out.resolved_sdk_dir.empty()) return out;
        } else {
            auto local = local_sdk_cache_path(out.requested_version);
            if (sdk_config_exists(local)) {
                out.resolved_sdk_dir = local;
                return out;
            }
        }
    }

    auto downloaded = sdk_cache_path(out.requested_version);
    if (sdk_config_exists(downloaded)) {
        out.resolved_sdk_dir = downloaded;
        return out;
    }

    if (materialize) {
        out.resolved_sdk_dir = ensure_sdk(out.requested_version);
    }
    return out;
}

bool enforce_project_cli_compatibility(const fs::path& project_root,
                                       const std::string& command_name,
                                       bool allow_unsupported_sdk) {
    if (allow_unsupported_sdk || project_root.empty()) return true;

    namespace vd = pulp::cli::version_diag;
    auto cli = vd::parse_semver(PULP_SDK_VERSION);
    auto project_sdk = vd::parse_semver(read_sdk_version(project_root));
    auto project_cli_min = vd::read_project_cli_min_version(project_root);
    auto preflight = vd::analyze_execution_preflight(cli, project_sdk, project_cli_min);
    if (preflight.supported) return true;

    auto format_semver = [](const vd::Semver& value) {
        return value.raw.empty() ? std::string("(unknown)")
                                 : "v" + value.raw;
    };

    std::cerr << "Error: " << command_name
              << " blocked: project requires a newer Pulp CLI.\n";
    std::cerr << "  Installed CLI: " << format_semver(cli) << "\n";
    if (!project_sdk.raw.empty()) {
        std::cerr << "  Project SDK:   " << format_semver(project_sdk) << "\n";
    }
    if (project_cli_min.comparable) {
        std::cerr << "  CLI minimum:   " << format_semver(project_cli_min) << "\n";
    }
    std::cerr << "  Project root:  " << project_root.string() << "\n";
    for (const auto& blocker : preflight.blockers) {
        std::cerr << "  - " << blocker << "\n";
    }
    std::cerr << "\n";
    if (preflight.required_cli.comparable) {
        std::cerr << "Run `pulp upgrade " << preflight.required_cli.raw
                  << "` and retry.\n";
    } else {
        std::cerr << "Run `pulp upgrade` and retry.\n";
    }
    std::cerr << "Use `pulp doctor --versions` to inspect the mismatch.\n";
    std::cerr << "If you need to bypass this guard, rerun with "
              << "`--allow-unsupported-sdk` (unsupported).\n";
    return false;
}

bool cache_preflight_check(const fs::path& project_root,
                           const std::string& command_name) {
    namespace fcc = pulp::cli::fetchcontent_cache;

    // Escape hatch — CI environments that intentionally maintain a
    // sealed cache can opt out. Mirrors the `--allow-unsupported-sdk`
    // shape used by the version preflight above.
    if (const char* skip = std::getenv("PULP_SKIP_CACHE_PREFLIGHT");
        skip && skip[0] != '\0' && std::string(skip) != "0") {
        return true;
    }

    auto cache_root = fcc::default_cache_root();
    if (cache_root.empty()) return true;  // no derivable cache root → nothing to check

    fcc::DeclaredRefs refs;
    if (!project_root.empty()) {
        refs = fcc::parse_declared_refs_from_file(
            project_root / "CMakeLists.txt");
    }
    auto env = fcc::make_real_env(cache_root, refs);
    auto entries = fcc::discover_fetchcontent_cache(env);
    // Only gate on states that genuinely break configure/build —
    // StaleCommit entries are harmless because CMake's override path
    // keys on the *current* sanitized ref. See blocks_preflight() and
    // the Codex P1 review on PR #753.
    if (!fcc::blocks_preflight(entries)) return true;

    std::cerr << "Error: " << command_name
              << " blocked by FetchContent cache health check.\n";
    (void)fcc::render_preflight(entries, cache_root, std::cerr);
    return false;
}

std::string read_user_config_value(const std::string& section, const std::string& key) {
    auto config_path = pulp_home() / "config.toml";
    if (!fs::exists(config_path)) return {};

    std::ifstream f(config_path);
    if (!f.is_open()) return {};

    std::string line;
    std::string current_section;
    while (std::getline(f, line)) {
        auto comment = line.find('#');
        if (comment != std::string::npos) line = line.substr(0, comment);

        auto trimmed = trim(line);
        if (trimmed.empty()) continue;

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            current_section = trim(trimmed.substr(1, trimmed.size() - 2));
            continue;
        }

        if (current_section != section) continue;

        auto eq = trimmed.find('=');
        if (eq == std::string::npos) continue;

        auto parsed_key = trim(trimmed.substr(0, eq));
        if (parsed_key != key) continue;

        return strip_quotes(trim(trimmed.substr(eq + 1)));
    }

    return {};
}

std::string normalize_pr_workflow(std::string workflow) {
    workflow = trim(strip_quotes(workflow));
    std::transform(workflow.begin(), workflow.end(), workflow.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return workflow;
}

bool is_valid_pr_workflow(const std::string& workflow) {
    const auto normalized = normalize_pr_workflow(workflow);
    return normalized == "shipyard" ||
           normalized == "github" ||
           normalized == "manual";
}

PrWorkflowSelection resolve_pr_workflow(const std::string& cli_override) {
    auto make = [](std::string raw, std::string source) {
        PrWorkflowSelection out;
        out.workflow = normalize_pr_workflow(raw);
        out.source = std::move(source);
        if (!is_valid_pr_workflow(out.workflow)) {
            out.error = "pr.workflow must be one of: shipyard, github, manual";
        }
        return out;
    };

    if (!trim(cli_override).empty()) {
        return make(cli_override, "cli");
    }
    if (const char* env = std::getenv("PULP_PR_WORKFLOW"); env && *env) {
        return make(env, "env:PULP_PR_WORKFLOW");
    }
    auto configured = read_user_config_value("pr", "workflow");
    if (!configured.empty()) {
        return make(configured, "config:pr.workflow");
    }
    return PrWorkflowSelection{"shipyard", "default", {}};
}

std::string read_pinned_shipyard_version(const fs::path& root) {
    std::ifstream f(root / "tools" / "shipyard.toml");
    if (!f) return {};
    std::string line;
    while (std::getline(f, line)) {
        auto t = trim(line);
        if (t.rfind("version", 0) != 0) continue;
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        auto rhs = trim(t.substr(eq + 1));
        if (rhs.size() >= 2 && rhs.front() == '"' && rhs.back() == '"') {
            return rhs.substr(1, rhs.size() - 2);
        }
        return rhs;
    }
    return {};
}

static std::string parse_shipyard_version_output(std::string out) {
    for (char& c : out) {
        if (c == ',' || c == '(' || c == ')') c = ' ';
    }
    std::istringstream tokens(out);
    std::string token;
    while (tokens >> token) {
        while (!token.empty() &&
               (token.back() == ',' || token.back() == ';' || token.back() == ':')) {
            token.pop_back();
        }
        auto check = token;
        if (!check.empty() && check.front() == 'v') check.erase(check.begin());
        if (check.empty() || !std::isdigit(static_cast<unsigned char>(check.front()))) {
            continue;
        }
        if (check.find('.') == std::string::npos) continue;
        return token.front() == 'v' ? token : ("v" + token);
    }
    return {};
}

std::string capture_shipyard_version(const std::string& shipyard_bin) {
    if (shipyard_bin.empty()) return {};
    auto out = exec_output(shell_quote(shipyard_bin) + " --version 2>&1");
    return parse_shipyard_version_output(trim(out));
}

bool write_user_config_value(const std::string& section,
                             const std::string& key,
                             const std::string& value) {
    auto home = pulp_home();
    if (home.empty()) return false;
    auto path = home / "config.toml";

    std::string contents;
    if (fs::exists(path)) {
        std::ifstream f(path);
        if (f.is_open()) {
            std::ostringstream buf;
            buf << f.rdbuf();
            contents = buf.str();
        }
    }

    auto rewritten = pulp::cli::update_check::write_toml_key_in_section(
        contents, section, key, value);

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f << rewritten;
    return f.good();
}

std::string read_project_cmake_version(const fs::path& project_root) {
    auto cmake_path = project_root / "CMakeLists.txt";
    if (!fs::exists(cmake_path)) return {};

    std::ifstream f(cmake_path);
    std::string line;
    std::regex version_re(R"(project\s*\([^)]*VERSION\s+(\d+\.\d+\.\d+))");
    while (std::getline(f, line)) {
        std::smatch m;
        if (std::regex_search(line, m, version_re)) {
            return m[1].str();
        }
    }
    return {};
}

// ── Build Helpers ───────────────────────────────────────────────────────────

#ifdef _WIN32
static bool windows_host_is_arm64() {
    auto normalize = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return value;
    };

    auto arch = normalize(pulp::runtime::get_env("PROCESSOR_ARCHITECTURE").value_or(""));
    auto wow64 = normalize(pulp::runtime::get_env("PROCESSOR_ARCHITEW6432").value_or(""));
    return arch == "ARM64" || wow64 == "ARM64";
}

static std::string windows_visual_studio_generator_args() {
    if (!find_executable_in_path("cl").empty()) {
        return {};
    }

    auto install_path = trim(exec_output(
        "\"%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe\""
        " -latest -products * -requiresAny"
        " -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
        " -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64"
        " -property installationPath 2>nul"));
    install_path = strip_quotes(install_path);
    if (install_path.empty()) {
        return {};
    }

    std::string args;
    args += " -G " + shell_quote(std::string("Visual Studio 17 2022"));
    args += (windows_host_is_arm64() ? " -A ARM64" : " -A x64");
    args += " -DCMAKE_GENERATOR_INSTANCE=" + shell_quote(install_path);
    return args;
}

void append_windows_visual_studio_generator_args(std::string& cmd) {
    cmd += windows_visual_studio_generator_args();
}
#else
void append_windows_visual_studio_generator_args(std::string&) {}
#endif

std::string default_create_formats(const fs::path& repo_root, const std::string& type) {
    if (type == "app" || type == "bare") {
        return "Standalone";
    }

    if (!repo_root.empty()) {
        std::vector<std::string> formats;
        if (fs::exists(repo_root / "external" / "vst3sdk" / "pluginterfaces")) {
            formats.push_back("VST3");
        }
#ifdef __APPLE__
        if (fs::exists(repo_root / "external" / "AudioUnitSDK")) {
            formats.push_back("AU");
        }
#endif
        formats.push_back("CLAP");
#if defined(__APPLE__) || defined(_WIN32)
        if (!find_aax_sdk_root().empty()) {
            formats.push_back("AAX");
        }
#endif
#ifdef __linux__
        formats.push_back("LV2");
#endif
        formats.push_back("Standalone");
        std::ostringstream out;
        for (size_t i = 0; i < formats.size(); ++i) {
            if (i > 0) out << ' ';
            out << formats[i];
        }
        return out.str();
    }

#ifdef __APPLE__
    return find_aax_sdk_root().empty() ? "VST3 AU CLAP Standalone"
                                       : "VST3 AU CLAP AAX Standalone";
#elif defined(_WIN32)
    return find_aax_sdk_root().empty() ? "VST3 CLAP Standalone"
                                       : "VST3 CLAP AAX Standalone";
#else
    return "VST3 CLAP LV2 Standalone";
#endif
}

bool checkout_supports_vst3(const fs::path& repo_root) {
    return fs::exists(repo_root / "external" / "vst3sdk" / "pluginterfaces");
}

#ifdef __APPLE__
bool checkout_supports_au(const fs::path& repo_root) {
    return fs::exists(repo_root / "external" / "AudioUnitSDK");
}
#endif

int ensure_repo_build_configured(const fs::path& project_root, const fs::path& build_dir) {
    bool needs_configure = !fs::exists(build_dir / "CMakeCache.txt");

    if (!needs_configure && fs::exists(build_dir / "CMakeCache.txt")) {
        auto cmake_time = fs::last_write_time(project_root / "CMakeLists.txt");
        auto cache_time = fs::last_write_time(build_dir / "CMakeCache.txt");
        if (cmake_time > cache_time) needs_configure = true;
    }

    if (!needs_configure) return 0;
    std::string configure_cmd = "cmake -B " + shell_quote(build_dir) + " -S " + shell_quote(project_root);
    append_windows_visual_studio_generator_args(configure_cmd);
    return run_with_spinner(configure_cmd, "Configuring");
}

// ── AAX Helpers ─────��────────────────────────────��──────────────────────────

bool aax_supported_on_host() {
#if defined(__APPLE__) || defined(_WIN32)
    return true;
#else
    return false;
#endif
}

std::string aax_download_url() {
    return "https://developer.avid.com/aax/";
}

std::string aax_sdk_download_label() {
    return "AAX SDK";
}

std::string aax_validator_download_label() {
    return "DigiShell and AAX Validator";
}

bool looks_like_aax_sdk_root(const fs::path& path) {
    return !path.empty()
        && fs::exists(path / "Interfaces" / "AAX.h")
        && fs::exists(path / "Interfaces" / "AAX_Exports.cpp");
}

static std::vector<fs::path> aax_sdk_candidates() {
    std::vector<fs::path> candidates;
    if (auto env = pulp::runtime::get_env("PULP_AAX_SDK_DIR")) {
        auto trimmed = strip_quotes(trim(*env));
        if (!trimmed.empty()) candidates.emplace_back(trimmed);
    }

    auto home = user_home_dir();
    if (!home.empty()) {
        candidates.push_back(home / "SDKs" / "avid" / "aax-sdk" / "current");
        candidates.push_back(home / "SDKs" / "avid" / "aax-sdk");
        candidates.push_back(home / "SDKs" / "Avid" / "AAXSDK" / "current");
        candidates.push_back(home / "SDKs" / "Avid" / "AAXSDK");
    }

    return candidates;
}

fs::path find_aax_sdk_root() {
    for (const auto& candidate : aax_sdk_candidates()) {
        if (looks_like_aax_sdk_root(candidate)) {
            return fs::absolute(candidate);
        }
    }
    return {};
}

static fs::path aax_validator_commandline_dir(const fs::path& root) {
    if (root.empty()) return {};
    auto commandline = root / "CommandLineTools";
    if (fs::exists(platform_executable(commandline / "dsh"))) {
        return commandline;
    }
    if (fs::exists(platform_executable(root / "dsh"))) {
        return root;
    }
    return {};
}

static bool looks_like_aax_validator_root(const fs::path& path) {
    auto tool_dir = aax_validator_commandline_dir(path);
    if (tool_dir.empty()) return false;
#ifdef __APPLE__
    return fs::exists(tool_dir / "Dishes" / "aaxval.dish" / "Contents" / "MacOS" / "aaxval");
#else
    return fs::exists(tool_dir / "Dishes" / "aaxval.dish");
#endif
}

static std::vector<fs::path> aax_validator_candidates() {
    std::vector<fs::path> candidates;
    if (auto env = pulp::runtime::get_env("PULP_AAX_VALIDATOR_DIR")) {
        auto trimmed = strip_quotes(trim(*env));
        if (!trimmed.empty()) candidates.emplace_back(trimmed);
    }

    auto home = user_home_dir();
    if (!home.empty()) {
        candidates.push_back(home / "SDKs" / "avid" / "aax-validator" / "current");
        candidates.push_back(home / "SDKs" / "avid" / "aax-validator");
        candidates.push_back(home / "SDKs" / "Avid" / "AAXValidator" / "current");
        candidates.push_back(home / "SDKs" / "Avid" / "AAXValidator");
    }

    return candidates;
}

fs::path find_aax_validator_root() {
    for (const auto& candidate : aax_validator_candidates()) {
        if (looks_like_aax_validator_root(candidate)) {
            return fs::absolute(candidate);
        }
    }
    return {};
}

void print_aax_setup_guidance(bool need_sdk, bool need_validator) {
    std::cout << "      AAX is optional and supported only on macOS and Windows.\n";
    std::cout << "      Sign in at " << aax_download_url() << " and download:\n";
    if (need_sdk) {
        std::cout << "        - " << aax_sdk_download_label() << "\n";
    }
    if (need_validator) {
        std::cout << "        - " << aax_validator_download_label() << "\n";
    }
    std::cout << "      Suggested install locations:\n";
    std::cout << "        - ~/SDKs/avid/aax-sdk/current\n";
    std::cout << "        - ~/SDKs/avid/aax-validator/current\n";
    if (need_sdk) {
        std::cout << "      Set PULP_AAX_SDK_DIR to the unpacked AAX SDK root.\n";
    }
    if (need_validator) {
        std::cout << "      Set PULP_AAX_VALIDATOR_DIR to the extracted validator root containing CommandLineTools/.\n";
    }
}

fs::path write_temp_text_file(const std::string& prefix, const std::string& content) {
    auto tmp = fs::temp_directory_path()
             / (prefix + "-" + std::to_string(getpid()) + "-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    std::ofstream out(tmp);
    out << content;
    return tmp;
}

bool bundle_contains_payload(const fs::path& bundle_path) {
    if (bundle_path.empty() || !fs::exists(bundle_path)) return false;
    if (!fs::is_directory(bundle_path)) return fs::is_regular_file(bundle_path);

    const auto stem = bundle_path.stem().string();
    const auto ext = bundle_path.extension().string();

    for (auto it = fs::recursive_directory_iterator(bundle_path, fs::directory_options::skip_permission_denied);
         it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) continue;
        const auto filename = it->path().filename().string();
        if (filename == stem || filename == stem + ext) {
            return true;
        }
    }

    return false;
}

std::string run_aax_validator_command(const fs::path& validator_root,
                                     const fs::path& plugin_path,
                                     bool run_all)
{
    auto tool_dir = aax_validator_commandline_dir(validator_root);
    if (tool_dir.empty()) return {};

    auto dsh = platform_executable(tool_dir / "dsh");
    if (!fs::exists(dsh)) return {};

    std::ostringstream script;
    script << "load_dish aaxval\n";
    if (run_all) {
        script << "runtests \"" << plugin_path.string() << "\"\n";
    } else {
        script << "runtest [test.describe_validation, \"" << plugin_path.string() << "\"]\n";
    }
    script << "quit\n";

    auto script_path = write_temp_text_file("pulp-aaxval", script.str());
    auto command = "cd " + shell_quote(tool_dir)
                 + " && " + shell_quote(dsh)
                 + " < " + shell_quote(script_path)
                 + " 2>&1";
    auto output = sanitize_process_output(exec_output(command));
    std::error_code ec;
    fs::remove(script_path, ec);
    return output;
}

bool aax_validator_passed(const std::string& output) {
    if (output.find("result_status: E_COMPLETED_PASS") != std::string::npos) {
        return true;
    }

    static const std::regex summary_re(R"((\d+)\s+passed,\s+(\d+)\s+failed,\s+(\d+)\s+warnings,\s+(\d+)\s+cancelled)",
                                       std::regex::icase);
    std::smatch match;
    if (std::regex_search(output, match, summary_re) && match.size() == 5) {
        const auto failed = std::stoi(match[2].str());
        const auto cancelled = std::stoi(match[4].str());
        return failed == 0 && cancelled == 0;
    }

    if (output.find("result_status: E_COMPLETED_FAIL") != std::string::npos
        || output.find("result_status: E_FAILED") != std::string::npos
        || output.find("result_status: E_CANCELED") != std::string::npos
        || output.find("FAILED:") != std::string::npos
        || output.find("failed to complete") != std::string::npos) {
        return false;
    }

    return false;
}

// Doctor checks (run_doctor_checks, run_doctor_android_checks,
// run_doctor_ios_checks) moved to tools/cli/cli_doctor_helpers.cpp
// in the 2026-05 Phase 2 (R2-4) batch. Public API stays in
// cli_common.hpp.

// ── Script/Binary Delegation ────────────────────────────────────────────────

int delegate_to_python_script(const fs::path& relative_script,
                              const std::vector<std::string>& args) {
    auto root = require_project_root();
    if (!root) return 1;
    auto script = *root / relative_script;
    if (!fs::exists(script)) {
        std::cerr << "Error: script not found at " << script.string() << "\n";
        return 1;
    }
    std::string cmd = "python3 " + shell_quote(script);
    for (auto& arg : args) cmd += " " + shell_quote(arg);
    return run(cmd);
}

int delegate_to_build_binary(const fs::path& relative_binary,
                             const std::vector<std::string>& args,
                             const std::string& prepend_flag) {
    // pulp #-friction-1+#-friction-2 — delegate lookup must NOT require
    // cwd to be inside a Pulp project. Sibling binaries (pulp-import-design,
    // pulp-design-debug) live next to the CLI binary itself, so we can
    // resolve them from argv[0] alone. Only fall back to the project root
    // when the self-path lookup misses (e.g. when an installed `pulp` is
    // dispatching to a build dir).
    std::vector<fs::path> candidates;
    auto add_candidate = [&](fs::path path) {
        path = platform_executable(std::move(path));
        for (const auto& existing : candidates) {
            if (existing == path) return;
        }
        candidates.push_back(std::move(path));
    };

    // Dev/CI builds can use matrix-scoped build directories such as
    // build-linux or build-macos. Resolve sibling helper binaries from the
    // running CLI's build tree before falling back to the legacy build/ path.
    auto self = current_executable_path();
    if (!self.empty()) {
        auto cli_dir = self.parent_path();
        auto tools_dir = cli_dir.parent_path();
        auto build_dir = tools_dir.parent_path();
        if (cli_dir.filename() == "cli" && tools_dir.filename() == "tools" &&
            !build_dir.empty()) {
            add_candidate(build_dir / relative_binary);
        }
    }

    // Project-root-relative paths (and $PULP_BUILD_DIR overrides) are only
    // meaningful when the user is operating inside a Pulp checkout.
    // find_project_root() returns empty when there isn't one; we tolerate
    // that and rely on the self-path candidate above.
    fs::path root = find_project_root();  // empty if outside a project

    if (const char* env = std::getenv("PULP_BUILD_DIR"); env && *env) {
        fs::path build_dir(env);
        if (build_dir.is_relative() && !root.empty()) {
            build_dir = root / build_dir;
        }
        if (!build_dir.is_relative()) {
            add_candidate(build_dir / relative_binary);
        }
    }

    if (!root.empty()) {
        add_candidate(root / "build" / relative_binary);
    }

    fs::path binary;
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) {
            binary = candidate;
            break;
        }
    }

    if (binary.empty()) {
        const auto leaf = fs::path(relative_binary).filename().string();
        std::cerr << "Error: " << leaf << " helper not found.\n";
        std::cerr << "  Looked in:\n";
        for (const auto& c : candidates) {
            std::cerr << "    " << c.string() << "\n";
        }
        std::cerr << "  Fix: from a Pulp checkout, run\n"
                  << "    cmake --build build --target " << leaf << "\n";
        if (root.empty()) {
            std::cerr << "  (cwd is not inside a Pulp project; set PULP_BUILD_DIR or\n"
                      << "  run from a checkout to use a project-relative build dir)\n";
        }
        return 1;
    }

    std::string cmd = shell_quote(binary);
    if (!prepend_flag.empty()) cmd += " " + prepend_flag;
    for (auto& arg : args) cmd += " " + shell_quote(arg);
    return run(cmd);
}

// ── Interactive Prompts ────��────────────────────────────────────────────────

namespace cli {

bool confirm(const std::string& question, bool default_yes) {
    std::string hint = default_yes ? "[Y/n]" : "[y/N]";
    std::cout << color::bold() << question << color::reset() << " " << hint << " ";
    std::cout.flush();

    std::string line;
    if (!std::getline(std::cin, line) || line.empty()) {
        return default_yes;
    }

    char c = static_cast<char>(std::tolower(static_cast<unsigned char>(line[0])));
    return c == 'y';
}

int choose(const std::string& prompt, const std::vector<std::string>& options) {
    if (options.empty()) return -1;

    std::cout << color::bold() << prompt << color::reset() << "\n";
    for (size_t i = 0; i < options.size(); ++i) {
        std::cout << "  " << color::cyan() << (i + 1) << color::reset()
                  << ") " << options[i] << "\n";
    }
    std::cout << color::dim() << "Enter number (1-" << options.size() << "): " << color::reset();
    std::cout.flush();

    std::string line;
    if (!std::getline(std::cin, line) || line.empty()) {
        return 0;
    }

    try {
        int choice = std::stoi(line);
        if (choice >= 1 && choice <= static_cast<int>(options.size())) {
            return choice - 1;
        }
    } catch (...) {}

    return 0;
}

std::string input(const std::string& prompt, const std::string& default_value) {
    std::cout << color::bold() << prompt << color::reset();
    if (!default_value.empty()) {
        std::cout << " " << color::dim() << "(" << default_value << ")" << color::reset();
    }
    std::cout << ": ";
    std::cout.flush();

    std::string line;
    if (!std::getline(std::cin, line) || line.empty()) {
        return default_value;
    }
    return line;
}

} // namespace cli

// ── File Watching ───────────────────��───────────────────────────────────────

static std::map<fs::path, fs::file_time_type> snapshot_timestamps(const fs::path& root) {
    std::map<fs::path, fs::file_time_type> timestamps;
    static const std::vector<std::string> watch_extensions = {
        ".cpp", ".hpp", ".h", ".c", ".mm", ".swift", ".js", ".css", ".json"
    };

    for (auto& entry : fs::recursive_directory_iterator(root,
            fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        bool watched = false;
        for (auto& wext : watch_extensions) {
            if (ext == wext) { watched = true; break; }
        }
        if (entry.path().filename() == "CMakeLists.txt") watched = true;
        if (!watched) continue;

        auto rel = entry.path().lexically_relative(root);
        if (!rel.empty() && *rel.begin() == "build") continue;

        std::error_code ec;
        auto mtime = fs::last_write_time(entry.path(), ec);
        if (!ec) timestamps[entry.path()] = mtime;
    }
    return timestamps;
}

int watch_loop(const WatchOptions& opts) {
    std::string mode_label;
    if (!opts.launch_target.empty()) mode_label += " + launch";
    if (opts.run_tests) mode_label += " + test";
    if (opts.run_validate) mode_label += " + validate";

    std::cout << color::cyan() << "Dev loop active" << color::reset()
              << " (build" << mode_label << ") — Ctrl-C to stop\n";

    auto previous = snapshot_timestamps(opts.root);

    // Launch the target process if requested
    auto launch_child = [&]() {
        if (opts.launch_target.empty()) return;
        auto binary = fs::path(opts.launch_target);
        if (!binary.is_absolute()) binary = opts.build_dir / binary;
        if (!fs::exists(binary)) {
            std::cerr << "  " << color::yellow() << "Target not found: "
                      << binary.string() << color::reset() << "\n";
            return;
        }
        std::string cmd = shell_quote(binary);
        for (auto& arg : opts.launch_args) cmd += " " + shell_quote(arg);
        cmd += " &";
        std::cout << "  " << color::dim() << "Launching "
                  << binary.filename().string() << "..." << color::reset() << "\n";
        run(cmd);
    };

    auto kill_child = [&]() {
        if (opts.launch_target.empty()) return;
        auto binary_name = fs::path(opts.launch_target).filename().string();
#ifdef _WIN32
        run("taskkill /F /IM " + shell_quote(binary_name) + " 2>nul");
#else
        run("pkill -f " + shell_quote(binary_name) + " 2>/dev/null");
#endif
    };

    launch_child();

    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        auto current = snapshot_timestamps(opts.root);
        bool changed = false;
        std::string first_changed;

        for (auto& [path, mtime] : current) {
            auto it = previous.find(path);
            if (it == previous.end() || it->second != mtime) {
                if (!changed) {
                    first_changed = path.lexically_relative(opts.root).string();
                }
                changed = true;
            }
        }
        for (auto& [path, mtime] : previous) {
            if (current.find(path) == current.end()) {
                if (!changed) first_changed = path.lexically_relative(opts.root).string() + " (deleted)";
                changed = true;
            }
        }

        if (!changed) { previous = current; continue; }

        std::cout << "\n" << color::yellow() << "Change detected"
                  << color::reset() << ": " << first_changed << "\n";

        // Build
        std::string build_cmd = "cmake --build " + opts.build_dir.string();
        for (auto& arg : opts.build_args) build_cmd += " " + arg;
        int rc = run_with_spinner(build_cmd, "Rebuilding");

        if (rc != 0) {
            std::cout << color::red() << "Build failed." << color::reset()
                      << " Watching for more changes...\n";
            previous = snapshot_timestamps(opts.root);
            continue;
        }

        // Tests
        if (opts.run_tests) {
            std::string test_cmd = "ctest --test-dir " + opts.build_dir.string()
                                 + " --output-on-failure";
            if (!opts.test_filter.empty()) test_cmd += " -R " + shell_quote(opts.test_filter);
            int trc = run_with_spinner(test_cmd, "Testing");
            if (trc != 0) {
                std::cout << color::red() << "Tests failed." << color::reset()
                          << " Watching for more changes...\n";
            }
        }

        // Quick validation (dlopen checks only)
        if (opts.run_validate) {
            for (auto dir_name : {"CLAP", "VST3"}) {
                auto dir = opts.build_dir / dir_name;
                if (!fs::exists(dir)) continue;
                for (auto& entry : fs::directory_iterator(dir)) {
                    auto ext = entry.path().extension().string();
                    if (ext != ".clap" && ext != ".vst3") continue;
                    auto name = entry.path().stem().string();
                    std::string dltest = "ctest --test-dir " + opts.build_dir.string()
                                       + " -R dlopen-" + name + " --output-on-failure 2>/dev/null";
                    if (run(dltest) == 0) print_ok(std::string(dir_name) + ": " + name + " loads OK");
                    else print_fail(std::string(dir_name) + ": " + name + " failed to load");
                }
            }
        }

        // Relaunch target
        if (!opts.launch_target.empty()) {
            kill_child();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            launch_child();
        }

        std::cout << color::cyan() << "Watching for changes..." << color::reset() << "\n";
        previous = snapshot_timestamps(opts.root);
    }

    return 0;
}

int watch_and_rebuild(const fs::path& root, const fs::path& build_dir,
                      const std::vector<std::string>& build_args) {
    WatchOptions opts;
    opts.root = root;
    opts.build_dir = build_dir;
    opts.build_args = build_args;
    return watch_loop(opts);
}

// ── Fuzzy Matching ─────────────────────────────────────────────��────────────

int fuzzy_score(const std::string& text, const std::string& query) {
    if (query.empty()) return 1;
    if (text.empty()) return 0;

    std::string lower_text = text;
    std::string lower_query = query;
    for (auto& c : lower_text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : lower_query) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (lower_text.find(lower_query) != std::string::npos) {
        return 100 + static_cast<int>(query.size());
    }

    int score = 0;
    size_t qi = 0;
    bool prev_matched = false;
    for (size_t ti = 0; ti < lower_text.size() && qi < lower_query.size(); ++ti) {
        if (lower_text[ti] == lower_query[qi]) {
            score += prev_matched ? 3 : 1;
            prev_matched = true;
            ++qi;
        } else {
            prev_matched = false;
        }
    }

    if (qi < lower_query.size()) return 0;

    return score;
}
