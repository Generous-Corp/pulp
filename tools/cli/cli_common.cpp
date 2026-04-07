// cli_common.cpp — Shared implementations for the Pulp CLI

#include "cli_common.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <regex>
#include <sstream>
#include <thread>

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

// ── Shell Execution ───��─────────────────────────────────────────────────────

int run(const std::string& cmd) {
    return std::system(cmd.c_str());
}

std::string shell_quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    out += "\"";
    return out;
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

std::string read_sdk_version(const fs::path& project_root) {
    auto version = read_pulp_toml_value(project_root, "sdk_version");
    if (!version.empty()) return version;
    return PULP_SDK_VERSION;
}

fs::path read_sdk_path_hint(const fs::path& project_root) {
    auto value = read_pulp_toml_value(project_root, "sdk_path");
    return value.empty() ? fs::path{} : fs::path(value);
}

fs::path read_sdk_checkout_hint(const fs::path& project_root) {
    auto value = read_pulp_toml_value(project_root, "sdk_checkout");
    return value.empty() ? fs::path{} : fs::path(value);
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

static std::string first_line(std::string text) {
    auto newline = text.find_first_of("\r\n");
    if (newline != std::string::npos) {
        text.erase(newline);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    return text;
}

// ── Doctor checks ───────────────���──────────────────────��────────────────────

static bool sdk_config_ready(const fs::path& sdk_dir) {
    if (sdk_dir.empty()) return false;
    return fs::exists(sdk_dir / "lib" / "cmake" / "Pulp" / "PulpConfig.cmake");
}

std::vector<DoctorCheck> run_doctor_checks(const fs::path& active_root, bool standalone_mode) {
    std::vector<DoctorCheck> checks;
    auto repo_root = standalone_mode ? fs::path{} : active_root;

    // 1. C++20 compiler
    {
        DoctorCheck c{"C++20 compiler", false, {}, {}};
#ifdef __APPLE__
        auto ver = first_line(exec_output("clang++ --version 2>&1"));
        if (!ver.empty()) {
            c.passed = true;
            c.detail = ver;
        } else {
            c.fix = "xcode-select --install";
        }
#elif defined(_WIN32)
        auto ver = first_line(exec_output("cl 2>&1"));
        if (!ver.empty()) {
            c.passed = true;
            c.detail = ver;
        } else {
            auto vswhere = exec_output(
                "\"%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe\""
                " -latest -requiresAny"
                " -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
                " -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64"
                " -property displayName 2>nul");
            if (!vswhere.empty()) {
                c.passed = true;
                c.detail = vswhere + " (CLI auto-selects the Visual Studio generator when cl.exe is not on PATH)";
            } else {
                c.fix = "Install Visual Studio Build Tools 2022+:\n"
                        "    https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022\n"
                        "    Select workload: 'Desktop development with C++'\n"
                        "    Or: winget install Microsoft.VisualStudio.2022.BuildTools";
            }
        }
#else
        auto ver = first_line(exec_output("g++ --version 2>&1"));
        if (ver.empty()) ver = first_line(exec_output("clang++ --version 2>&1"));
        if (!ver.empty()) {
            c.passed = true;
            c.detail = ver;
        } else {
            auto distro_id = exec_output("grep '^ID=' /etc/os-release 2>/dev/null | cut -d= -f2 | tr -d '\"'");
            if (distro_id == "ubuntu" || distro_id == "debian" || distro_id == "pop" || distro_id == "mint")
                c.fix = "sudo apt install g++-13";
            else if (distro_id == "fedora" || distro_id == "rhel" || distro_id == "centos")
                c.fix = "sudo dnf install gcc-c++";
            else if (distro_id == "arch" || distro_id == "manjaro")
                c.fix = "sudo pacman -S gcc";
            else if (distro_id == "opensuse" || distro_id == "sles")
                c.fix = "sudo zypper install gcc-c++";
            else
                c.fix = "Install g++-13 or clang++-15 (check your distro's package manager)";
        }
#endif
        checks.push_back(c);
    }

    // 2. CMake version
    {
        DoctorCheck c{"CMake >= 3.24", false, {}, {}};
        auto ver_str = first_line(exec_output("cmake --version 2>&1"));
        if (!ver_str.empty()) {
            auto pos = ver_str.find_first_of("0123456789");
            if (pos != std::string::npos) {
                auto ver_num = ver_str.substr(pos);
                auto dot1 = ver_num.find('.');
                auto dot2 = (dot1 != std::string::npos) ? ver_num.find('.', dot1 + 1) : std::string::npos;
                int major = 0, minor = 0;
                try {
                    major = std::stoi(ver_num.substr(0, dot1));
                    if (dot1 != std::string::npos)
                        minor = std::stoi(ver_num.substr(dot1 + 1, dot2 - dot1 - 1));
                } catch (...) {}

                if (major > 3 || (major == 3 && minor >= 24)) {
                    c.passed = true;
                    c.detail = "cmake " + ver_num.substr(0, dot2 != std::string::npos ? dot2 + 2 : std::string::npos);
                } else {
                    c.detail = "cmake " + ver_num.substr(0, dot2 != std::string::npos ? dot2 + 2 : std::string::npos) + " (too old)";
#ifdef __APPLE__
                    c.fix = "brew upgrade cmake";
#elif defined(_WIN32)
                    c.fix = "winget install Kitware.CMake";
#else
                    c.fix = "Install CMake 3.24+ from https://cmake.org/download/ or your package manager";
#endif
                }
            }
        } else {
#ifdef __APPLE__
            c.fix = "brew install cmake";
#elif defined(_WIN32)
            c.fix = "winget install Kitware.CMake";
#else
            c.fix = "Install CMake 3.24+ from https://cmake.org/download/ or your package manager";
#endif
        }
        checks.push_back(c);
    }

    // 3. git
    {
        DoctorCheck c{"git", false, {}, {}};
        auto ver = first_line(exec_output("git --version 2>&1"));
        if (!ver.empty() && ver.find("git version") != std::string::npos) {
            c.passed = true;
            c.detail = ver;
        } else {
#ifdef __APPLE__
            c.fix = "xcode-select --install (includes git) or brew install git";
#elif defined(_WIN32)
            c.fix = "Install Git for Windows: https://gitforwindows.org/";
#else
            c.fix = "sudo apt install git";
#endif
        }
        checks.push_back(c);
    }

    // 4. git-lfs
    {
        DoctorCheck c{"git-lfs", false, {}, {}};
        auto ver = first_line(exec_output("git lfs version 2>&1"));
#if !defined(_WIN32)
        if (ver.empty() || ver.find("git-lfs") == std::string::npos) {
            auto local_git_lfs = user_home_dir() / ".local" / "bin" / "git-lfs";
            if (fs::exists(local_git_lfs)) {
                ver = first_line(exec_output(shell_quote(local_git_lfs.string()) + " version 2>&1"));
                if (!ver.empty() && ver.find("git-lfs") != std::string::npos) {
                    ver += " (" + local_git_lfs.string() + ")";
                }
            }
        }
#endif
        if (!ver.empty() && ver.find("git-lfs") != std::string::npos) {
            c.passed = true;
            c.detail = ver;
        } else {
#ifdef __APPLE__
            c.fix = "brew install git-lfs && git lfs install";
#elif defined(_WIN32)
            c.fix = "Install Git for Windows if needed, then run: git lfs install";
#else
            c.fix = "sudo apt install git-lfs && git lfs install (or add ~/.local/bin to the non-interactive PATH)";
#endif
        }
        checks.push_back(c);
    }

    if (standalone_mode && !active_root.empty()) {
        DoctorCheck c{"pulp.toml", false, {}, {}};
        auto pulp_toml = active_root / "pulp.toml";
        if (fs::exists(pulp_toml)) {
            c.passed = true;
            c.detail = pulp_toml.string();
        } else {
            c.detail = "Not found";
        }
        checks.push_back(c);

        auto version = read_sdk_version(active_root);
        auto sdk_hint = read_sdk_path_hint(active_root);
        auto checkout_hint = read_sdk_checkout_hint(active_root);

        DoctorCheck sdk{"Installed SDK", false, {}, {}};
        if (!sdk_hint.empty() && sdk_config_ready(sdk_hint)) {
            sdk.passed = true;
            sdk.detail = sdk_hint.string();
        } else if (auto local_sdk = local_sdk_cache_path(version); sdk_config_ready(local_sdk)) {
            sdk.passed = true;
            sdk.detail = local_sdk.string() + " (local cache)";
        } else if (auto downloaded_sdk = sdk_cache_path(version); sdk_config_ready(downloaded_sdk)) {
            sdk.passed = true;
            sdk.detail = downloaded_sdk.string() + " (download cache)";
        } else if (!sdk_hint.empty()) {
            sdk.detail = sdk_hint.string() + " missing PulpConfig.cmake";
            sdk.fix = "pulp build";
        } else if (!checkout_hint.empty()) {
            sdk.detail = "SDK v" + version + " not materialized from checkout";
            sdk.fix = "pulp build";
        } else {
            sdk.detail = "SDK v" + version + " not installed";
            sdk.fix = "pulp build";
        }
        checks.push_back(sdk);

        if (!checkout_hint.empty()) {
            DoctorCheck checkout{"SDK checkout", false, {}, {}};
            if (fs::exists(checkout_hint / "setup.sh")) {
                checkout.passed = true;
                checkout.detail = checkout_hint.string();
            } else {
                checkout.detail = checkout_hint.string() + " missing setup.sh";
            }
            checks.push_back(checkout);
        }
    }

    if (!repo_root.empty()) {
        DoctorCheck c{"LFS files pulled", false, {}, {}};
        bool found_pointer = false;
        bool found_any = false;
        auto skia_dir = repo_root / "external" / "skia-build";
        if (fs::exists(skia_dir)) {
            for (auto& entry : fs::recursive_directory_iterator(skia_dir)) {
                auto ext = entry.path().extension().string();
                if (ext == ".a" || ext == ".lib") {
                    found_any = true;
                    std::ifstream f(entry.path(), std::ios::binary);
                    char buf[40] = {};
                    f.read(buf, 39);
                    if (std::string(buf).find("version https://git-lfs") != std::string::npos) {
                        found_pointer = true;
                        break;
                    }
                }
            }
        }
        if (found_pointer) {
            c.detail = "Skia files are LFS pointers, not binaries";
            c.fix = "git lfs pull";
        } else if (found_any) {
            c.passed = true;
            c.detail = "Skia binaries present";
        } else {
            c.passed = true;
            c.detail = "No LFS-tracked binaries found (OK if Skia not needed)";
        }
        checks.push_back(c);
    }

    if (!repo_root.empty()) {
        DoctorCheck c{"VST3 SDK", false, {}, {}};
        auto vst3_dir = repo_root / "external" / "vst3sdk";
        if (fs::exists(vst3_dir / "pluginterfaces")) {
            c.passed = true;
            c.detail = "external/vst3sdk/";
        } else if (fs::is_symlink(vst3_dir)) {
            c.detail = "Broken symlink at external/vst3sdk";
            c.fix = "rm external/vst3sdk && ./setup.sh";
        } else {
            c.detail = "Not found";
            c.fix = "git clone --depth 1 --recursive https://github.com/steinbergmedia/vst3sdk.git external/vst3sdk";
        }
        checks.push_back(c);
    }

#ifdef __APPLE__
    if (!repo_root.empty()) {
        DoctorCheck c{"AudioUnitSDK", false, {}, {}};
        auto au_dir = repo_root / "external" / "AudioUnitSDK";
        if (fs::exists(au_dir / "include")) {
            c.passed = true;
            c.detail = "external/AudioUnitSDK/";
        } else if (fs::is_symlink(au_dir)) {
            c.detail = "Broken symlink at external/AudioUnitSDK";
            c.fix = "rm external/AudioUnitSDK && ./setup.sh";
        } else {
            c.detail = "Not found";
            c.fix = "git clone --depth 1 https://github.com/apple/AudioUnitSDK.git external/AudioUnitSDK";
        }
        checks.push_back(c);
    }
#endif

#if defined(__APPLE__) || defined(_WIN32)
    {
        DoctorCheck c{"AAX SDK (optional)", true, {}, {}};
        if (auto sdk_root = find_aax_sdk_root(); !sdk_root.empty()) {
            c.detail = sdk_root.string();
        } else {
            c.detail = "Not configured (download AAX SDK from https://developer.avid.com/aax/)";
        }
        checks.push_back(c);
    }
    {
        DoctorCheck c{"AAX validator (optional)", true, {}, {}};
        if (auto validator_root = find_aax_validator_root(); !validator_root.empty()) {
            c.detail = validator_root.string();
        } else {
            c.detail = "Not installed (download DigiShell and AAX Validator from https://developer.avid.com/aax/)";
        }
        checks.push_back(c);
    }
#else
    {
        DoctorCheck c{"AAX", true, {}, {}};
        c.detail = "Unsupported on Linux/Ubuntu";
        checks.push_back(c);
    }
#endif

#ifdef __linux__
    {
        DoctorCheck c{"ALSA dev headers", false, {}, {}};
        int rc = std::system("pkg-config --exists alsa 2>/dev/null");
        if (rc == 0) {
            c.passed = true;
            c.detail = "libasound2-dev";
        } else {
            auto distro_id = exec_output("grep '^ID=' /etc/os-release 2>/dev/null | cut -d= -f2 | tr -d '\"'");
            if (distro_id == "ubuntu" || distro_id == "debian" || distro_id == "pop" || distro_id == "mint")
                c.fix = "sudo apt install libasound2-dev";
            else if (distro_id == "fedora" || distro_id == "rhel" || distro_id == "centos")
                c.fix = "sudo dnf install alsa-lib-devel";
            else if (distro_id == "arch" || distro_id == "manjaro")
                c.fix = "sudo pacman -S alsa-lib";
            else if (distro_id == "opensuse" || distro_id == "sles")
                c.fix = "sudo zypper install alsa-devel";
            else
                c.fix = "Install ALSA development headers (check your distro's package manager)";
        }
        checks.push_back(c);
    }
#endif

    if (!active_root.empty()) {
        DoctorCheck c{"Build configured", false, {}, {}};
        if (fs::exists(active_root / "build" / "CMakeCache.txt")) {
            c.passed = true;
            c.detail = "build/CMakeCache.txt present";
        } else {
            c.detail = "Not yet configured";
            c.fix = "pulp build  (or cmake -B build)";
        }
        checks.push_back(c);
    }

    // Package health checks
    if (!active_root.empty()) {
        auto lock_path = active_root / "packages.lock.json";
        auto reg_path = active_root / "tools" / "packages" / "registry.json";

        {
            DoctorCheck c{"Package lock file", false, {}, {}};
            if (!fs::exists(lock_path)) {
                c.passed = true;
                c.detail = "No packages installed (OK)";
            } else if (!fs::exists(reg_path)) {
                c.passed = false;
                c.detail = "Lock file exists but registry missing";
            } else {
                c.passed = true;
                std::ifstream f(lock_path);
                std::string content((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
                int count = 0;
                std::string::size_type pos = 0;
                while ((pos = content.find("\"version\"", pos)) != std::string::npos) {
                    ++count; ++pos;
                }
                c.detail = std::to_string(count) + " package(s) installed";
            }
            checks.push_back(c);
        }

        if (fs::exists(lock_path) && fs::exists(reg_path)) {
            DoctorCheck c{"Package platform alignment", false, {}, {}};
            auto targets = pulp::cli::pkg::read_project_targets(active_root);
            auto [reg, err] = pulp::cli::pkg::load_registry(reg_path);
            auto lock = pulp::cli::pkg::load_lock_file(lock_path);
            int gaps = 0;
            for (auto& [id, lp] : lock.packages) {
                auto it = reg.packages.find(id);
                if (it == reg.packages.end()) continue;
                auto unsup = pulp::cli::pkg::unsupported_targets(it->second, targets);
                gaps += static_cast<int>(unsup.size());
            }
            if (gaps == 0) {
                c.passed = true;
                c.detail = "All packages support all project targets";
            } else {
                c.detail = std::to_string(gaps) + " platform gap(s)";
                c.fix = "pulp audit --platforms";
            }
            checks.push_back(c);
        }
    }

    return checks;
}

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
    auto root = require_project_root();
    if (!root) return 1;

    auto binary = *root / "build" / relative_binary;
    if (!fs::exists(binary)) {
        std::cerr << "Error: " << fs::path(relative_binary).filename().string()
                  << " not built. Run `pulp build` first.\n";
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
