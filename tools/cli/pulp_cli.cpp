// pulp — CLI tool for the Pulp audio plugin framework
// Wraps common build/test/status operations

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <pulp/runtime/system.hpp>
#include <pulp/tools/audio/excerpt_service.hpp>
#include <pulp/tools/audio/model_store.hpp>
#include <pulp/tools/audio/service.hpp>

#include "create_targets.hpp"
#include "package_commands.hpp"
#include "package_registry.hpp"
#include "tool_registry.hpp"
#include "design_binding.hpp"
#include <pulp/ship/installer.hpp>

// External command — implemented in cmd_inspect.cpp
int cmd_inspect(const std::vector<std::string>& args);
#include <pulp/view/screenshot.hpp>
#include <pulp/format/editor_ui.hpp>

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

namespace fs = std::filesystem;

// ── SDK Constants ───────────────────────────────────────────────────────────

static const char* PULP_SDK_VERSION = "0.1.1";
static const char* PULP_GITHUB_REPO = "danielraffel/pulp";

// ── Color / Terminal ────────────────────────────────────────────────────────

static bool g_color_enabled = true;
static bool g_no_color = false;  // --no-color flag

static bool is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

static void init_color() {
    if (g_no_color) { g_color_enabled = false; return; }
    // Respect NO_COLOR convention (https://no-color.org/)
    if (pulp::runtime::get_env("NO_COLOR")) { g_color_enabled = false; return; }
    g_color_enabled = is_tty();
}

namespace color {
    static std::string reset()   { return g_color_enabled ? "\033[0m"  : ""; }
    static std::string bold()    { return g_color_enabled ? "\033[1m"  : ""; }
    static std::string dim()     { return g_color_enabled ? "\033[2m"  : ""; }
    static std::string green()   { return g_color_enabled ? "\033[32m" : ""; }
    static std::string yellow()  { return g_color_enabled ? "\033[33m" : ""; }
    static std::string red()     { return g_color_enabled ? "\033[31m" : ""; }
    static std::string cyan()    { return g_color_enabled ? "\033[36m" : ""; }
}

// Formatted status helpers
static void print_ok(const std::string& msg) {
    std::cout << "  " << color::green() << "\xe2\x9c\x93" << color::reset() << " " << msg << "\n";
}
static void print_fail(const std::string& msg) {
    std::cout << "  " << color::red() << "\xe2\x9c\x97" << color::reset() << " " << msg << "\n";
}
static void print_warn(const std::string& msg) {
    std::cout << "  " << color::yellow() << "\xe2\x9a\xa0" << color::reset() << " " << msg << "\n";
}

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::string trim(const std::string& s);
static std::string strip_quotes(const std::string& s);
static std::string find_executable_in_path(const std::string& name);

static int run(const std::string& cmd) {
    return std::system(cmd.c_str());
}

static std::string shell_quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    out += "\"";
    return out;
}

static std::string shell_quote(const fs::path& p) {
    return shell_quote(p.string());
}

static fs::path platform_executable(fs::path p) {
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

static bool parse_size_arg(const std::string& text, const char* flag, std::size_t& out) {
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

static bool parse_double_arg(const std::string& text, const char* flag, double& out) {
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

// Run a shell command with an animated spinner and elapsed time.
// Output is captured to a temp file and shown only on failure.
static int run_with_spinner(const std::string& cmd, const std::string& label) {
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

static std::string exec_output(const std::string& cmd) {
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

static void append_windows_visual_studio_generator_args(std::string& cmd) {
    cmd += windows_visual_studio_generator_args();
}
#else
static void append_windows_visual_studio_generator_args(std::string&) {}
#endif

static fs::path user_home_dir() {
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

static std::string find_executable_in_path(const std::string& name) {
#ifdef _WIN32
    auto output = exec_output("where " + shell_quote(name) + " 2>nul");
#else
    auto output = exec_output("command -v " + shell_quote(name) + " 2>/dev/null");
#endif
    if (output.empty()) return {};
    auto newline = output.find_first_of("\r\n");
    return newline == std::string::npos ? output : output.substr(0, newline);
}

static bool aax_supported_on_host() {
#if defined(__APPLE__) || defined(_WIN32)
    return true;
#else
    return false;
#endif
}

static std::string aax_download_url() {
    return "https://developer.avid.com/aax/";
}

static std::string aax_sdk_download_label() {
    return "AAX SDK";
}

static std::string aax_validator_download_label() {
    return "DigiShell and AAX Validator";
}

static bool looks_like_aax_sdk_root(const fs::path& path) {
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

static fs::path find_aax_sdk_root() {
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

static fs::path find_aax_validator_root() {
    for (const auto& candidate : aax_validator_candidates()) {
        if (looks_like_aax_validator_root(candidate)) {
            return fs::absolute(candidate);
        }
    }
    return {};
}

static void print_aax_setup_guidance(bool need_sdk, bool need_validator) {
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

static fs::path write_temp_text_file(const std::string& prefix, const std::string& content) {
    auto tmp = fs::temp_directory_path()
             / (prefix + "-" + std::to_string(getpid()) + "-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    std::ofstream out(tmp);
    out << content;
    return tmp;
}

static std::string sanitize_process_output(std::string output) {
    output.erase(std::remove(output.begin(), output.end(), '\0'), output.end());
    return output;
}

static std::string truncate_message(std::string value, std::size_t max_chars) {
    if (value.size() <= max_chars) return value;
    value.resize(max_chars);
    value += "...";
    return value;
}

static bool bundle_contains_payload(const fs::path& bundle_path) {
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

static std::string run_aax_validator_command(const fs::path& validator_root,
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

static bool aax_validator_passed(const std::string& output) {
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

static fs::path find_project_root_from(fs::path dir) {
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

static fs::path find_project_root() {
    return find_project_root_from(fs::current_path());
}

static fs::path current_executable_path() {
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

static fs::path cmake_home_directory(const fs::path& build_dir) {
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

static fs::path build_dir_from_current_binary() {
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

// ── Standalone Project Detection ────────────────────────────────────────────

// Check if we're in a standalone project (has pulp.toml but no core/)
static fs::path find_standalone_root() {
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

// ── SDK Cache ──────────────────────────────────────────────────────────────

static fs::path pulp_home() {
    if (auto pulp_home_env = pulp::runtime::get_env("PULP_HOME"))
        return fs::path(*pulp_home_env);

    auto home = pulp::runtime::get_env("HOME");
#ifdef _WIN32
    if (!home) home = pulp::runtime::get_env("USERPROFILE");
#endif
    if (!home) return {};
    return fs::path(*home) / ".pulp";
}

static fs::path sdk_cache_path(const std::string& version) {
    return pulp_home() / "sdk" / version;
}

static void remove_tree(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

static std::string detect_platform();

static fs::path local_sdk_cache_path(const std::string& version) {
    return pulp_home() / "sdk-local" / detect_platform() / version;
}

static std::string trim(const std::string& s);
static std::string strip_quotes(const std::string& s);

static std::string detect_platform() {
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

// Download and cache the Pulp SDK for the given version.
// Returns the SDK root path on success, empty path on failure.
static fs::path ensure_sdk(const std::string& version) {
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
    // The archive extracts to pulp-sdk/ directory
    auto extracted = fs::path(tmp_dir) / "pulp-sdk";
    if (!fs::exists(extracted)) {
        // Try without subdirectory (flat extraction)
        extracted = fs::path(tmp_dir);
    }

    try {
        // Copy all files from extracted dir to sdk_dir
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

static int ensure_checkout_dependencies(const fs::path& repo_root) {
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

static fs::path ensure_checkout_sdk(const fs::path& repo_root, const std::string& version) {
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
        std::cerr << "Error: local SDK installation incomplete — " << config.string() << " not found.\n";
        return {};
    }

    print_ok("Local SDK cached at " + sdk_dir.string());
    return sdk_dir;
}

// Read SDK version from pulp.toml
static std::string read_pulp_toml_value(const fs::path& project_root, const std::string& key) {
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

static std::string read_sdk_version(const fs::path& project_root) {
    auto version = read_pulp_toml_value(project_root, "sdk_version");
    if (!version.empty()) return version;
    return PULP_SDK_VERSION;
}

static fs::path read_sdk_path_hint(const fs::path& project_root) {
    auto value = read_pulp_toml_value(project_root, "sdk_path");
    return value.empty() ? fs::path{} : fs::path(value);
}

static fs::path read_sdk_checkout_hint(const fs::path& project_root) {
    auto value = read_pulp_toml_value(project_root, "sdk_checkout");
    return value.empty() ? fs::path{} : fs::path(value);
}

static std::string default_create_formats(const fs::path& repo_root, const std::string& type) {
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

static bool checkout_supports_vst3(const fs::path& repo_root) {
    return fs::exists(repo_root / "external" / "vst3sdk" / "pluginterfaces");
}

#ifdef __APPLE__
static bool checkout_supports_au(const fs::path& repo_root) {
    return fs::exists(repo_root / "external" / "AudioUnitSDK");
}
#endif

static fs::path resolve_active_project_root(bool* is_standalone) {
    auto standalone_root = find_standalone_root();
    if (!standalone_root.empty()) {
        if (is_standalone) *is_standalone = true;
        return standalone_root;
    }

    auto root = find_project_root();
    if (is_standalone) *is_standalone = false;
    return root;
}

static std::string read_user_config_value(const std::string& section, const std::string& key) {
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

static bool path_is_within(const fs::path& path, const fs::path& root) {
    auto normalized_path = fs::absolute(path).lexically_normal();
    auto normalized_root = fs::absolute(root).lexically_normal();

    auto path_it = normalized_path.begin();
    auto root_it = normalized_root.begin();
    for (; root_it != normalized_root.end(); ++root_it, ++path_it) {
        if (path_it == normalized_path.end() || *path_it != *root_it) return false;
    }

    return true;
}

static fs::path resolve_create_projects_base_dir(const fs::path& repo_root) {
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

// ── Commands ─────────────────────────────────────────────────────────────────

static int cmd_build(const std::vector<std::string>& args) {
    bool standalone_mode = false;
    auto project_root = resolve_active_project_root(&standalone_mode);
    if (project_root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto build_dir = project_root / "build";
    bool needs_configure = !fs::exists(build_dir / "CMakeCache.txt");

    // Check if CMakeLists.txt is newer than CMakeCache
    if (!needs_configure && fs::exists(build_dir / "CMakeCache.txt")) {
        auto cmake_time = fs::last_write_time(project_root / "CMakeLists.txt");
        auto cache_time = fs::last_write_time(build_dir / "CMakeCache.txt");
        if (cmake_time > cache_time) needs_configure = true;
    }

    // Extract --js-engine= flag before passing args through
    std::string js_engine;
    std::vector<std::string> passthrough_args;
    for (auto& arg : args) {
        if (arg.rfind("--js-engine=", 0) == 0) {
            js_engine = arg.substr(12);
            if (js_engine != "auto" && js_engine != "quickjs" && js_engine != "jsc" && js_engine != "v8") {
                std::cerr << "Error: --js-engine must be auto, quickjs, jsc, or v8\n";
                return 1;
            }
            needs_configure = true;  // Engine change requires reconfigure
        } else {
            passthrough_args.push_back(arg);
        }
    }

    if (needs_configure) {
        std::string configure_cmd = "cmake -B " + build_dir.string() + " -S " + project_root.string();
        append_windows_visual_studio_generator_args(configure_cmd);

        // Standalone projects need CMAKE_PREFIX_PATH to find the SDK
        if (standalone_mode) {
            auto version = read_sdk_version(project_root);
            auto sdk_dir = read_sdk_path_hint(project_root);
            auto checkout_hint = read_sdk_checkout_hint(project_root);
            auto config = sdk_dir / "lib" / "cmake" / "Pulp" / "PulpConfig.cmake";
            if (sdk_dir.empty() || !fs::exists(config)) {
                if (!checkout_hint.empty() && fs::exists(checkout_hint)) {
                    sdk_dir = ensure_checkout_sdk(checkout_hint, version);
                } else {
                    sdk_dir = ensure_sdk(version);
                }
            }
            if (sdk_dir.empty()) {
                std::cerr << "Error: could not obtain Pulp SDK v" << version << "\n";
                return 1;
            }
            configure_cmd += " -DCMAKE_PREFIX_PATH=" + sdk_dir.string();
        }

        // JS engine selection
        if (!js_engine.empty()) {
            configure_cmd += " -DPULP_JS_ENGINE=" + js_engine;
        }

        int rc = run_with_spinner(configure_cmd, "Configuring");
        if (rc != 0) return rc;
    }

    std::string build_cmd = "cmake --build " + build_dir.string();

    // Pass through extra args (e.g., --target, -j)
    for (auto& arg : passthrough_args) {
        build_cmd += " " + arg;
    }

    return run_with_spinner(build_cmd, "Building");
}

static int ensure_repo_build_configured(const fs::path& project_root, const fs::path& build_dir) {
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

static int cmd_design(const std::vector<std::string>& args) {
    fs::path cwd_root = find_project_root();
    fs::path build_dir;
    fs::path script_path;
    std::vector<std::string> pass_through;
    bool build_dir_explicit = false;
    std::string root_reason = cwd_root.empty() ? "" : "current checkout";
    std::string build_reason;
    std::string script_reason;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--build-dir" && i + 1 < args.size()) {
            build_dir = fs::absolute(args[++i]);
            build_dir_explicit = true;
            build_reason = "explicit --build-dir";
            continue;
        }
        if (args[i] == "--script" && i + 1 < args.size()) {
            script_path = fs::absolute(args[++i]);
            script_reason = "explicit --script";
            continue;
        }
        pass_through.push_back(args[i]);
    }

    if (script_path.empty() && !pass_through.empty() && !pass_through.front().empty()
        && pass_through.front()[0] != '-') {
        fs::path candidate = pass_through.front();
        auto ext = candidate.extension().string();
        if (ext == ".js" || ext == ".mjs" || ext == ".cjs") {
            script_path = candidate.is_absolute() ? candidate : fs::absolute(candidate);
            script_reason = "positional script argument";
            pass_through.erase(pass_through.begin());
        }
    }

    auto binary_build_dir = build_dir_from_current_binary();
    auto binary_root = cmake_home_directory(binary_build_dir);
    auto cache_root = cmake_home_directory(build_dir);
    pulp::cli::DesignBindingInput binding_input;
    binding_input.cwd_root = cwd_root;
    binding_input.build_dir = build_dir;
    binding_input.script_path = script_path;
    binding_input.script_root = script_path.empty() ? fs::path{} : find_project_root_from(script_path.parent_path());
    binding_input.build_dir_cache_root = cache_root;
    binding_input.binary_build_dir = binary_build_dir;
    binding_input.binary_root = binary_root;
    binding_input.build_dir_explicit = build_dir_explicit;
    binding_input.script_explicit = !script_reason.empty() && script_reason != "positional script argument";

    auto binding = pulp::cli::resolve_design_binding(binding_input);
    if (!binding.ok) {
        std::cerr << "Error: " << binding.error << "\n";
        return 1;
    }

    auto root = binding.root;
    build_dir = binding.build_dir;
    script_path = binding.script_path;
    root_reason = binding.root_reason;
    build_reason = binding.build_reason;
    script_reason = binding.script_reason;

    if (!fs::exists(script_path)) {
        std::cerr << "Error: design tool script not found at " << script_path << "\n";
        return 1;
    }

    std::cout << "Design root:  " << root << " (" << root_reason << ")\n";
    std::cout << "Build dir:    " << build_dir << " (" << build_reason << ")\n";
    std::cout << "Script:       " << script_path << " (" << script_reason << ")\n";

    int rc = ensure_repo_build_configured(root, build_dir);
    if (rc != 0) return rc;

    rc = run_with_spinner("cmake --build " + shell_quote(build_dir) + " --target pulp-design-tool",
                          "Building design tool");
    if (rc != 0) return rc;

    std::vector<fs::path> candidates = {
        platform_executable(build_dir / "tools" / "design" / "pulp-design"),
        platform_executable(build_dir / "examples" / "design-tool" / "pulp-design-tool"),
    };

    fs::path design_bin;
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) {
            design_bin = candidate;
            break;
        }
    }

    if (design_bin.empty()) {
        std::cerr << "Error: pulp-design-tool not found after build in " << build_dir << "\n";
        return 1;
    }

    std::string cmd = shell_quote(design_bin) + " " + shell_quote(script_path);
    for (const auto& arg : pass_through) {
        cmd += " " + shell_quote(arg);
    }
    return run(cmd);
}

static int cmd_test(const std::vector<std::string>& args) {
    bool standalone_mode = false;
    auto project_root = resolve_active_project_root(&standalone_mode);
    if (project_root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto build_dir = project_root / "build";
    if (!fs::exists(build_dir / "CMakeCache.txt")) {
        std::cout << "Build directory not found, building first...\n";
        int rc = cmd_build({});
        if (rc != 0) return rc;
    }

    std::string test_cmd = "ctest --test-dir " + build_dir.string() + " --output-on-failure";

    for (auto& arg : args) {
        test_cmd += " " + arg;
    }

    return run(test_cmd);
}

static int cmd_status([[maybe_unused]] const std::vector<std::string>& args) {
    bool standalone_mode = false;
    auto root = resolve_active_project_root(&standalone_mode);
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    // Read project info from CMakeLists.txt
    std::cout << "Pulp Project Status\n";
    std::cout << "====================\n";
    std::cout << "Root: " << root.string() << "\n";
    if (standalone_mode) {
        std::cout << "Mode: sdk mode\n";
        std::cout << "Mode detail: external project using an installed Pulp SDK artifact\n";
    } else {
        std::cout << "Mode: source-tree mode\n";
        std::cout << "Mode detail: repo/examples build against the current checkout\n";
    }

    // Git info
    auto branch = exec_output("git -C " + root.string() + " branch --show-current 2>/dev/null");
    auto commit = exec_output("git -C " + root.string() + " log --oneline -1 2>/dev/null");
    if (!branch.empty()) std::cout << "Branch: " << branch << "\n";
    if (!commit.empty()) std::cout << "Commit: " << commit << "\n";

    // Build state
    auto build_dir = root / "build";
    if (fs::exists(build_dir / "CMakeCache.txt")) {
        std::cout << "Build: configured\n";
    } else {
        std::cout << "Build: not configured (run `pulp build`)\n";
    }

    if (standalone_mode) {
        const auto version = read_sdk_version(root);
        const auto sdk_hint = read_sdk_path_hint(root);
        const auto checkout_hint = read_sdk_checkout_hint(root);
        std::cout << "SDK version: " << version << "\n";
        if (!sdk_hint.empty()) {
            std::cout << "SDK path: " << sdk_hint.string();
            std::cout << (fs::exists(sdk_hint / "lib" / "cmake" / "Pulp" / "PulpConfig.cmake") ? " (ready)\n" : " (missing)\n");
        } else if (auto local_sdk_dir = local_sdk_cache_path(version); fs::exists(local_sdk_dir)) {
            std::cout << "SDK local cache: " << local_sdk_dir.string() << "\n";
        } else if (auto downloaded_sdk_dir = sdk_cache_path(version); fs::exists(downloaded_sdk_dir)) {
            std::cout << "SDK download cache: " << downloaded_sdk_dir.string() << "\n";
        }
        if (!checkout_hint.empty()) {
            std::cout << "SDK checkout: " << checkout_hint.string();
            std::cout << (fs::exists(checkout_hint / "setup.sh") ? " (ready)\n" : " (missing)\n");
        }
        return 0;
    }

    // Count source files
    int cpp_count = 0, hpp_count = 0, test_count = 0;
    for (auto& entry : fs::recursive_directory_iterator(root / "core")) {
        auto ext = entry.path().extension().string();
        if (ext == ".cpp" || ext == ".mm") ++cpp_count;
        if (ext == ".hpp" || ext == ".h") ++hpp_count;
    }
    for (auto& entry : fs::directory_iterator(root / "test")) {
        if (entry.path().extension() == ".cpp") ++test_count;
    }
    std::cout << "Source files: " << cpp_count << " impl, " << hpp_count << " headers\n";
    std::cout << "Test files: " << test_count << "\n";

    // Count examples
    int example_count = 0;
    if (fs::exists(root / "examples")) {
        for (auto& entry : fs::directory_iterator(root / "examples")) {
            if (entry.is_directory()) ++example_count;
        }
    }
    std::cout << "Examples: " << example_count << "\n";

    // Check for plugin formats
    std::cout << "\nPlugin Formats:\n";
    if (fs::exists(root / "external" / "vst3sdk"))
        std::cout << "  VST3: available\n";
    else
        std::cout << "  VST3: SDK not found\n";

    if (fs::exists(root / "external" / "AudioUnitSDK"))
        std::cout << "  AU:   available\n";
    else
        std::cout << "  AU:   SDK not found\n";

    std::cout << "  CLAP: available (fetched via CMake)\n";

    if (aax_supported_on_host()) {
        auto sdk_root = find_aax_sdk_root();
        if (!sdk_root.empty()) {
            std::cout << "  AAX:  optional SDK found at " << sdk_root.string() << "\n";
        } else {
            std::cout << "  AAX:  optional (set PULP_AAX_SDK_DIR after downloading "
                      << aax_sdk_download_label() << " from " << aax_download_url() << ")\n";
        }
    } else {
        std::cout << "  AAX:  unsupported on Linux/Ubuntu\n";
    }

    return 0;
}

static int cmd_clean([[maybe_unused]] const std::vector<std::string>& args) {
    auto root = resolve_active_project_root(nullptr);
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto build_dir = root / "build";
    if (fs::exists(build_dir)) {
        std::cout << "Removing build directory...\n";
        fs::remove_all(build_dir);
        std::cout << "Clean.\n";
    } else {
        std::cout << "Nothing to clean.\n";
    }
    return 0;
}

static int cmd_validate(const std::vector<std::string>& args) {
    auto root = resolve_active_project_root(nullptr);
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto build_dir = root / "build";
    if (!fs::exists(build_dir)) {
        std::cerr << "Build directory not found. Run `pulp build` first.\n";
        return 1;
    }

    // Parse flags
    bool run_all = false;
    bool json_output = false;
    bool screenshot = false;
    std::string report_path;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--all") run_all = true;
        else if (args[i] == "--json") json_output = true;
        else if (args[i] == "--screenshot") screenshot = true;
        else if (args[i] == "--report" && i + 1 < args.size())
            report_path = args[++i];
    }

    int total = 0, passed = 0, failed = 0, skipped = 0;

    // JSON report accumulator
    std::vector<std::string> report_entries;

    auto record = [&](const std::string& tool, const std::string& plugin_path,
                      const std::string& format, const std::string& status,
                      int exit_code, const std::string& error_msg) {
        if (json_output || !report_path.empty()) {
            std::ostringstream e;
            e << "    {\"type\": \"validator\", \"status\": \"" << status << "\", "
              << "\"target\": \"" << fs::path(plugin_path).filename().string() << "\", "
              << "\"validator\": {"
              << "\"tool\": \"" << tool << "\", "
              << "\"plugin_path\": \"" << plugin_path << "\", "
              << "\"plugin_format\": \"" << format << "\", "
              << "\"exit_code\": " << exit_code;
            if (!error_msg.empty()) e << ", \"stderr\": \"" << error_msg << "\"";
            e << "}}";
            report_entries.push_back(e.str());
        }
    };

    // ── CLAP validation ─────────────────────────────────────────────────

    auto clap_dir = build_dir / "CLAP";
    if (fs::exists(clap_dir)) {
        bool has_clap_validator = !find_executable_in_path("clap-validator").empty();

        for (auto& entry : fs::directory_iterator(clap_dir)) {
            if (entry.path().extension() == ".clap") {
                auto name = entry.path().stem().string();

                if (!bundle_contains_payload(entry.path())) {
                    std::cout << "CLAP: " << name << " SKIPPED (bundle directory exists but plugin binary is missing)\n";
                    ++skipped;
                    record("clap-validator", entry.path().string(), "clap", "skip", -1,
                           "bundle directory exists but plugin binary is missing");
                    continue;
                }

                ++total;

                if (has_clap_validator) {
                    std::cout << "CLAP: validating " << name << "... ";
                    auto clap_path = entry.path().string();
                    int rc = run("clap-validator validate \"" + clap_path + "\" 2>/dev/null");
                    if (rc == 0) {
                        std::cout << "PASSED\n";
                        ++passed;
                        record("clap-validator", clap_path, "clap", "pass", 0, "");
                    } else {
                        std::cout << "FAILED\n";
                        ++failed;
                        record("clap-validator", clap_path, "clap", "fail", rc, "");
                    }
                } else {
                    // Fallback: dlopen test
                    std::cout << "CLAP: " << name << " (dlopen check only, clap-validator not installed)... ";
                    auto test_cmd = "ctest --test-dir " + build_dir.string() + " -R clap-dlopen-" + name + " --output-on-failure 2>/dev/null";
                    int rc = run(test_cmd);
                    if (rc == 0) {
                        std::cout << "PASSED\n";
                        ++passed;
                        record("clap-validator", entry.path().string(), "clap", "pass", 0, "dlopen only");
                    } else {
                        std::cout << "FAILED\n";
                        ++failed;
                        record("clap-validator", entry.path().string(), "clap", "fail", rc, "dlopen only");
                    }
                }
            }
        }
    }

    // ── VST3 validation (pluginval) ─────────────────────────────────────

    auto vst3_dir = build_dir / "VST3";
    if (fs::exists(vst3_dir)) {
        bool has_pluginval = !find_executable_in_path("pluginval").empty();

        for (auto& entry : fs::directory_iterator(vst3_dir)) {
            if (entry.path().extension() == ".vst3") {
                auto name = entry.path().stem().string();
                ++total;

                if (has_pluginval) {
                    std::cout << "VST3: validating " << name << " (pluginval)... ";
                    auto vst3_path = entry.path().string();
                    int rc = run("pluginval --strictness-level 5 --timeout-ms 30000 --validate \""
                                 + vst3_path + "\" 2>/dev/null");
                    if (rc == 0) {
                        std::cout << "PASSED\n";
                        ++passed;
                        record("pluginval", vst3_path, "vst3", "pass", 0, "");
                    } else {
                        std::cout << "FAILED\n";
                        ++failed;
                        record("pluginval", vst3_path, "vst3", "fail", rc, "");
                    }
                } else {
                    std::cout << "VST3: " << name << " SKIPPED (pluginval not installed)\n";
                    ++skipped;
                    record("pluginval", entry.path().string(), "vst3", "skip", -1,
                           "pluginval not found in PATH");
                }
            }
        }
    }

    // ── vstvalidator (evaluation: run if --all and tool is available) ────

    if (run_all && fs::exists(vst3_dir)) {
        bool has_vstvalidator = !find_executable_in_path("vstvalidator").empty();

        if (has_vstvalidator) {
            for (auto& entry : fs::directory_iterator(vst3_dir)) {
                if (entry.path().extension() == ".vst3") {
                    auto name = entry.path().stem().string();

                    if (!bundle_contains_payload(entry.path())) {
                        std::cout << "VST3: " << name << " SKIPPED (bundle directory exists but plugin binary is missing)\n";
                        ++skipped;
                        record("pluginval", entry.path().string(), "vst3", "skip", -1,
                               "bundle directory exists but plugin binary is missing");
                        continue;
                    }

                    ++total;
                    std::cout << "VST3: validating " << name << " (vstvalidator)... ";
                    auto vst3_path = entry.path().string();
                    int rc = run("vstvalidator \"" + vst3_path + "\" 2>/dev/null");
                    if (rc == 0) {
                        std::cout << "PASSED\n";
                        ++passed;
                        record("vstvalidator", vst3_path, "vst3", "pass", 0, "");
                    } else {
                        std::cout << "FAILED\n";
                        ++failed;
                        record("vstvalidator", vst3_path, "vst3", "fail", rc, "");
                    }
                }
            }
        } else {
            std::cout << "VST3: vstvalidator not found — skipping vstvalidator checks.\n";
            std::cout << "      vstvalidator is the Steinberg VST3 SDK validator.\n";
            std::cout << "      It is not widely distributed; build it from the VST3 SDK if needed.\n";
            std::cout << "      Go/no-go: optional — pluginval covers most VST3 validation needs.\n";
        }
    }

    // ── AU validation (macOS only) ──────────────────────────────────────

#ifdef __APPLE__
    auto au_dir = build_dir / "AU";
    if (fs::exists(au_dir)) {
        for (auto& entry : fs::directory_iterator(au_dir)) {
            if (entry.path().extension() == ".component") {
                auto name = entry.path().stem().string();

                if (!bundle_contains_payload(entry.path())) {
                    std::cout << "AU: " << name << " SKIPPED (bundle directory exists but plugin binary is missing)\n";
                    ++skipped;
                    record("auval", entry.path().string(), "au", "skip", -1,
                           "bundle directory exists but plugin binary is missing");
                    continue;
                }

                ++total;
                std::cout << "AU: " << name << " (auval check)... ";

                // Check if auval exists
                if (!find_executable_in_path("auval").empty()) {
                    // Run auval test from ctest
                    auto test_cmd = "ctest --test-dir " + build_dir.string() + " -R auval-" + name + " --output-on-failure 2>/dev/null";
                    int rc = run(test_cmd);
                    if (rc == 0) {
                        std::cout << "PASSED\n";
                        ++passed;
                        record("auval", entry.path().string(), "au", "pass", 0, "");
                    } else {
                        std::cout << "FAILED\n";
                        ++failed;
                        record("auval", entry.path().string(), "au", "fail", rc, "");
                    }
                } else {
                    std::cout << "SKIPPED (auval not found)\n";
                    ++skipped;
                    record("auval", entry.path().string(), "au", "skip", -1,
                           "auval not found");
                }
            }
        }
    }
#endif

#if defined(__APPLE__) || defined(_WIN32)
    // ── AAX validation (optional validator) ─────────────────────────────

    auto aax_dir = build_dir / "AAX";
    if (fs::exists(aax_dir)) {
        auto validator_root = find_aax_validator_root();
        bool has_aax_validator = !validator_root.empty();
        bool printed_guidance = false;

        for (auto& entry : fs::directory_iterator(aax_dir)) {
            if (entry.path().extension() != ".aaxplugin") continue;

            auto name = entry.path().stem().string();

            if (!bundle_contains_payload(entry.path())) {
                std::cout << "AAX: " << name << " SKIPPED (bundle directory exists but plugin binary is missing)\n";
                ++skipped;
                record("aax-validator", entry.path().string(), "aax", "skip", -1,
                       "bundle directory exists but plugin binary is missing");
                continue;
            }

            ++total;

            if (!has_aax_validator) {
                std::cout << "AAX: " << name << " SKIPPED (AAX validator not installed)\n";
                ++skipped;
                record("aax-validator", entry.path().string(), "aax", "skip", -1,
                       "AAX validator not found");
                if (!printed_guidance) {
                    print_aax_setup_guidance(false, true);
                    printed_guidance = true;
                }
                continue;
            }

            std::cout << "AAX: validating " << name
                      << (run_all ? " (aaxval full)... " : " (aaxval describe)... ");
            auto output = run_aax_validator_command(validator_root, entry.path(), run_all);
            auto summary = truncate_message(output, 400);

            if (aax_validator_passed(output)) {
                std::cout << "PASSED\n";
                ++passed;
                record("aax-validator", entry.path().string(), "aax", "pass", 0, "");
            } else {
                std::cout << "FAILED\n";
                ++failed;
                record("aax-validator", entry.path().string(), "aax", "fail", 1, summary);
            }
        }
    }
#endif

    // ── Summary ─────────────────────────────────────────────────────────

    std::cout << "\nValidation Summary: " << total << " total, "
              << passed << " passed, " << failed << " failed, "
              << skipped << " skipped\n";

    // ── JSON report output ──────────────────────────────────────────────

    if (json_output || !report_path.empty()) {
        // Capture git ref
        auto git_ref = exec_output("git -C \"" + root.string() + "\" rev-parse --short HEAD 2>/dev/null");

        // Timestamp
        auto now = std::time(nullptr);
        char ts[64];
        auto utc = pulp::runtime::gmtime_utc(now);
        std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &utc);

        std::ostringstream report;
        report << "{\n";
        report << "  \"version\": 1,\n";
        report << "  \"timestamp\": \"" << ts << "\",\n";
        if (!git_ref.empty())
            report << "  \"git_ref\": \"" << git_ref << "\",\n";
        report << "  \"reports\": [\n";
        for (size_t i = 0; i < report_entries.size(); ++i) {
            report << report_entries[i];
            if (i + 1 < report_entries.size()) report << ",";
            report << "\n";
        }
        report << "  ]\n";
        report << "}\n";

        if (json_output) {
            std::cout << "\n" << report.str();
        }
        if (!report_path.empty()) {
            std::ofstream f(report_path);
            if (f.good()) {
                f << report.str();
                std::cout << "Report written to " << report_path << "\n";
            } else {
                std::cerr << "Failed to write report to " << report_path << "\n";
            }
        }
    }

    // ── Screenshot capture (--screenshot) ───────────────────────────────────
    if (screenshot) {
        auto screenshots_dir = root / "artifacts" / "screenshots";
        fs::create_directories(screenshots_dir);
        int captured = 0;

        // Capture editor screenshots for each built plugin example
        auto examples_dir = build_dir / "examples";
        if (!fs::exists(examples_dir)) examples_dir = build_dir;

        // Use AutoUi to render a generic parameter editor for each plugin's StateStore
        // This creates the same UI that the plugin would show in a DAW
        for (auto dir_name : {"VST3", "CLAP", "AU"}) {
            auto dir = build_dir / dir_name;
            if (!fs::exists(dir)) continue;

            for (auto& entry : fs::directory_iterator(dir)) {
                auto ext = entry.path().extension().string();
                if (ext != ".vst3" && ext != ".clap" && ext != ".component") continue;

                auto name = entry.path().stem().string();
                auto png_name = name + "-" + dir_name + ".png";
                auto png_path = screenshots_dir / png_name;

                // Build an editor UI from the StateStore (AutoUi)
                pulp::state::StateStore store;
                auto editor_ui = pulp::format::build_editor_ui(store, false);

                if (!editor_ui.root) {
                    std::cout << "  Screenshot: " << name << " (" << dir_name << ") — no editor, skipping\n";
                    continue;
                }

                uint32_t w = 400, h = 300;
                bool ok = pulp::view::render_to_file(*editor_ui.root, w, h, png_path.string());
                if (ok) {
                    std::cout << "  Screenshot: " << png_path.filename().string() << "\n";
                    ++captured;
                } else {
                    std::cerr << "  Screenshot FAILED: " << name << " (" << dir_name << ")\n";
                }
            }
        }

        if (captured > 0)
            std::cout << captured << " screenshot(s) saved to " << screenshots_dir.string() << "\n";
        else
            std::cout << "No plugin screenshots captured.\n";
    }

    return failed > 0 ? 1 : 0;
}

static int cmd_ship(const std::vector<std::string>& args) {
    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto build_dir = root / "build";
    if (!fs::exists(build_dir / "CMakeCache.txt")) {
        std::cerr << "Build directory not found. Run `pulp build` first.\n";
        return 1;
    }

    // Parse subcommand
    std::string sub = args.empty() ? "help" : args[0];

    if (sub == "sign") {
        // Sign all plugin bundles
        std::string identity;
        std::string entitlements = (root / "ship" / "templates" / "entitlements.plist").string();
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--identity" && i + 1 < args.size())
                identity = args[++i];
            else if (args[i] == "--entitlements" && i + 1 < args.size())
                entitlements = args[++i];
        }
        if (identity.empty()) {
            std::cerr << "Usage: pulp ship sign --identity \"Developer ID Application: ...\"\n";
            return 1;
        }

        int signed_count = 0;
        for (auto dir_name : {"VST3", "CLAP", "AU"}) {
            auto dir = build_dir / dir_name;
            if (!fs::exists(dir)) continue;
            for (auto& entry : fs::directory_iterator(dir)) {
                auto ext = entry.path().extension().string();
                if (ext == ".vst3" || ext == ".clap" || ext == ".component") {
                    std::cout << "Signing " << entry.path().filename().string() << "...\n";
                    std::string cmd = "codesign --force --sign \"" + identity
                        + "\" --timestamp --options runtime"
                        + " --entitlements \"" + entitlements + "\""
                        + " \"" + entry.path().string() + "\"";
                    if (run(cmd) == 0) ++signed_count;
                    else std::cerr << "  FAILED\n";
                }
            }
        }
        std::cout << "Signed " << signed_count << " bundles.\n";
        return 0;
    }

    if (sub == "package") {
        auto artifacts = root / "artifacts";
        fs::create_directories(artifacts);

        std::string version = "0.1.0";
        bool per_user = false;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--version" && i + 1 < args.size())
                version = args[++i];
            if (args[i] == "--per-user")
                per_user = true;
        }

#ifdef _WIN32
        // Windows: use NSIS installer
        // Check for makensis
        if (std::system("where makensis >nul 2>&1") != 0) {
            std::cerr << "Error: makensis not found on PATH\n";
            std::cerr << "  Install NSIS from https://nsis.sourceforge.io/\n";
            std::cerr << "  Then add its directory to PATH\n";
            return 1;
        }

        // Discover product name from the first plugin found
        std::string product_name;
        pulp::ship::InstallerConfig config;
        config.version = version;
        config.per_user_install = per_user;

        for (auto dir_name : {"VST3", "CLAP"}) {
            auto dir = build_dir / dir_name;
            if (!fs::exists(dir)) continue;
            std::string format_lower = dir_name;
            for (auto& c : format_lower) c = static_cast<char>(std::tolower(c));

            for (auto& entry : fs::directory_iterator(dir)) {
                auto ext = entry.path().extension().string();
                if (ext == ".vst3" || ext == ".clap") {
                    if (product_name.empty())
                        product_name = entry.path().stem().string();
                    config.plugins.push_back({
                        entry.path().string(), "", format_lower
                    });
                }
            }
        }

        if (config.plugins.empty()) {
            std::cerr << "Error: no plugins found in " << build_dir.string() << "\n";
            return 1;
        }

        config.product_name = product_name;
        config.publisher = "Pulp";
        config.output_path = (artifacts / (product_name + "-" + version + "-setup.exe")).string();

        auto license = root / "LICENSE.md";
        if (fs::exists(license)) config.license_path = license.string();

        std::cout << "Creating NSIS installer for " << product_name << "...\n";
        if (pulp::ship::create_nsis_installer(config)) {
            std::cout << "  Created " << config.output_path << "\n";
        } else {
            std::cerr << "  FAILED to create installer\n";
            return 1;
        }
        return 0;
#else
        // macOS/Linux: use pkgbuild (macOS) or deb (Linux)
        int pkg_count = 0;
        for (auto dir_name : {"VST3", "CLAP", "AU"}) {
            auto dir = build_dir / dir_name;
            if (!fs::exists(dir)) continue;
            std::string format_lower = dir_name;
            for (auto& c : format_lower) c = static_cast<char>(std::tolower(c));

            for (auto& entry : fs::directory_iterator(dir)) {
                auto ext = entry.path().extension().string();
                if (ext == ".vst3" || ext == ".clap" || ext == ".component") {
                    auto name = entry.path().stem().string();
                    auto pkg_name = name + "-" + dir_name + "-" + version + ".pkg";
                    auto pkg_path = artifacts / pkg_name;

                    std::string install_loc = "/Library/Audio/Plug-Ins/";
                    if (ext == ".vst3") install_loc += "VST3/";
                    else if (ext == ".clap") install_loc += "CLAP/";
                    else install_loc = pulp::runtime::get_env("HOME").value_or("~")
                                     + "/Library/Audio/Plug-Ins/Components/";

                    std::cout << "Packaging " << name << " (" << dir_name << ")...\n";
                    std::string cmd = "pkgbuild --component \"" + entry.path().string() + "\""
                        + " --identifier \"com.pulp." + name + "." + format_lower + "\""
                        + " --version \"" + version + "\""
                        + " --install-location \"" + install_loc + "\""
                        + " \"" + pkg_path.string() + "\" 2>/dev/null";
                    if (run(cmd) == 0) ++pkg_count;
                    else std::cerr << "  FAILED\n";
                }
            }
        }
        std::cout << "Created " << pkg_count << " packages in " << artifacts.string() << "\n";
        return 0;
#endif
    }

    if (sub == "check") {
        // Check signing status of all built plugins
        for (auto dir_name : {"VST3", "CLAP", "AU"}) {
            auto dir = build_dir / dir_name;
            if (!fs::exists(dir)) continue;
            for (auto& entry : fs::directory_iterator(dir)) {
                auto ext = entry.path().extension().string();
                if (ext == ".vst3" || ext == ".clap" || ext == ".component") {
                    std::cout << entry.path().filename().string() << ": ";
                    int rc = run("codesign --verify --deep --strict \""
                                 + entry.path().string() + "\" 2>/dev/null");
                    std::cout << (rc == 0 ? "signed" : "unsigned") << "\n";
                }
            }
        }
        return 0;
    }

    // Help
    std::cout << "pulp ship — signing and packaging commands\n\n";
    std::cout << "Subcommands:\n";
    std::cout << "  sign     Sign all plugin bundles\n";
    std::cout << "           --identity \"Developer ID Application: ...\"\n";
    std::cout << "           --entitlements path/to/entitlements.plist\n";
    std::cout << "  package  Create .pkg installers for all built plugins\n";
    std::cout << "           --version 1.0.0\n";
    std::cout << "  check    Check signing status of built plugins\n";
    return 0;
}

// ── Doctor ───────────────────────────────────────────────────────────────────

struct DoctorCheck {
    std::string name;
    bool passed;
    std::string detail;
    std::string fix;
};

static bool sdk_config_ready(const fs::path& sdk_dir) {
    if (sdk_dir.empty()) return false;
    return fs::exists(sdk_dir / "lib" / "cmake" / "Pulp" / "PulpConfig.cmake");
}

static std::vector<DoctorCheck> run_doctor_checks(const fs::path& active_root, bool standalone_mode) {
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

        // Check: lock file freshness
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
                // Count packages
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

        // Check: package/platform alignment
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

    // Cmajor CLI check — only if project has .cmajorpatch files
    if (!active_root.empty()) {
        bool has_patches = false;
        for (auto it = fs::recursive_directory_iterator(active_root,
                 fs::directory_options::skip_permission_denied);
             it != fs::recursive_directory_iterator(); ++it) {
            if (it->is_directory()) {
                auto name = it->path().filename().string();
                if (name == "build" || name == "external" || name == ".git" || name == "node_modules") {
                    it.disable_recursion_pending();
                    continue;
                }
            }
            if (it->path().extension() == ".cmajorpatch") {
                has_patches = true;
                break;
            }
        }
        if (has_patches) {
            DoctorCheck c{"Cmajor CLI (cmaj)", false, {}, {}};
            auto cmaj_path = find_executable_in_path("cmaj");
            if (!cmaj_path.empty()) {
                c.passed = true;
                c.detail = cmaj_path;
            } else if (auto env = std::getenv("CMAJ_BIN"); env && fs::exists(env)) {
                c.passed = true;
                c.detail = std::string(env) + " (via CMAJ_BIN)";
            } else {
                c.detail = "Project has .cmajorpatch files but cmaj is not installed";
                c.fix = "Download from https://cmajor.dev or set CMAJ_BIN=/path/to/cmaj";
            }
            checks.push_back(c);
        }
    }

    return checks;
}

static int cmd_doctor(const std::vector<std::string>& args) {
    bool standalone_mode = false;
    auto active_root = resolve_active_project_root(&standalone_mode);
    auto root = standalone_mode ? fs::path{} : active_root;

    bool fix_mode = false;
    bool ci_mode = false;
    bool dry_run = false;
    for (auto& arg : args) {
        if (arg == "--fix") fix_mode = true;
        if (arg == "--ci") ci_mode = true;
        if (arg == "--dry-run") dry_run = true;
    }

    if (!ci_mode) {
        std::cout << color::bold() << "Pulp Doctor" << color::reset() << "\n";
        std::cout << "===========\n\n";
        if (standalone_mode && !active_root.empty()) {
            std::cout << color::dim() << "(SDK mode project — checking system tools for an installed SDK workflow)" << color::reset() << "\n\n";
        } else if (root.empty()) {
            std::cout << color::dim() << "(Not in a Pulp project — checking system tools only)" << color::reset() << "\n\n";
        } else {
            std::cout << color::dim() << "(Source-tree mode — checking the active Pulp checkout)" << color::reset() << "\n\n";
        }
    }

    auto checks = run_doctor_checks(active_root, standalone_mode);

    int pass_count = 0, fail_count = 0;
    for (auto& c : checks) {
        if (c.passed) {
            ++pass_count;
            if (!ci_mode) {
                std::string msg = c.name;
                if (!c.detail.empty()) msg += " — " + c.detail;
                print_ok(msg);
            }
        } else {
            ++fail_count;
            if (ci_mode) {
                std::cerr << "FAIL: " << c.name;
                if (!c.detail.empty()) std::cerr << " — " << c.detail;
                if (!c.fix.empty()) std::cerr << " [fix: " << c.fix << "]";
                std::cerr << "\n";
            } else {
                std::string msg = c.name;
                if (!c.detail.empty()) msg += " — " + c.detail;
                print_fail(msg);
                if (!c.fix.empty()) {
                    if (fix_mode && !dry_run) {
                        std::cout << "    " << color::cyan() << "Fixing:" << color::reset() << " " << c.fix << "\n";
                        int rc = std::system(c.fix.c_str());
                        if (rc == 0) {
                            print_ok("Fixed");
                            --fail_count;
                            ++pass_count;
                        } else {
                            std::cout << "    Fix failed (exit " << rc << "). Run manually:\n";
                            std::cout << "      " << color::yellow() << c.fix << color::reset() << "\n";
                        }
                    } else if (dry_run) {
                        std::cout << "    " << color::dim() << "[dry-run] Would run: " << c.fix << color::reset() << "\n";
                    } else {
                        std::cout << "    Fix: " << color::yellow() << c.fix << color::reset() << "\n";
                    }
                }
            }
        }
    }

    if (!ci_mode) {
        std::cout << "\n  " << color::bold() << pass_count << "/" << (pass_count + fail_count)
                  << " checks passed" << color::reset();
        if (fail_count > 0) {
            std::cout << " — run " << color::cyan() << "`pulp doctor --fix`" << color::reset() << " to resolve";
        }
        std::cout << "\n";
    }

    return fail_count > 0 ? 1 : 0;
}

// Forward declarations for helpers defined later
static std::string read_file_contents(const fs::path& path);
static std::string trim(const std::string& s);
static std::string strip_quotes(const std::string& s);
static std::string read_user_config_value(const std::string& section, const std::string& key);
static bool path_is_within(const fs::path& path, const fs::path& root);
static fs::path resolve_active_project_root(bool* is_standalone = nullptr);
static fs::path resolve_create_projects_base_dir(const fs::path& repo_root);

// ── New / Create ────────────────────────────────────────────────────────────

static std::string replace_all_str(const std::string& str,
                                    const std::string& from,
                                    const std::string& to) {
    std::string result = str;
    size_t pos = 0;
    while ((pos = result.find(from, pos)) != std::string::npos) {
        result.replace(pos, from.length(), to);
        pos += to.length();
    }
    return result;
}

static std::string to_class_name(const std::string& name) {
    std::string result;
    bool capitalize = true;
    for (char c : name) {
        if (c == ' ' || c == '-' || c == '_') { capitalize = true; continue; }
        if (capitalize) { result += static_cast<char>(std::toupper(c)); capitalize = false; }
        else result += c;
    }
    return result;
}

static std::string to_lower_name(const std::string& name) {
    std::string result;
    for (char c : name) {
        if (c == ' ' || c == '_') result += '-';
        else if (std::isalnum(c)) result += static_cast<char>(std::tolower(c));
    }
    return result;
}

static std::string to_namespace_name(const std::string& name) {
    std::string result;
    for (char c : name) {
        if (c == ' ' || c == '-') result += '_';
        else if (std::isalnum(c)) result += static_cast<char>(std::tolower(c));
    }
    return result;
}

static std::string make_plugin_code(const std::string& class_name) {
    std::string clean;
    for (char c : class_name) if (std::isalpha(c)) clean += c;
    if (clean.size() >= 4) return clean.substr(0, 4);
    return (clean + "xxxx").substr(0, 4);
}

static std::string make_aax_product_code(const std::string& class_name) {
    auto clean = make_plugin_code(class_name);
    if (clean.size() < 4) clean = (clean + "xxxx").substr(0, 4);
    clean[3] = 'P';
    return clean;
}

static std::string make_mfr_code(const std::string& mfr) {
    std::string clean;
    for (char c : mfr) if (std::isalpha(c)) clean += c;
    if (clean.size() >= 4) return clean.substr(0, 4);
    return (clean + "xxxx").substr(0, 4);
}

static std::string make_vst3_uid() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;

    std::array<uint32_t, 4> words = {
        dist(gen), dist(gen), dist(gen), dist(gen)
    };

    std::ostringstream out;
    out << "Steinberg::FUID(";
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i > 0) out << ", ";
        out << "0x"
            << std::uppercase
            << std::hex
            << std::setw(8)
            << std::setfill('0')
            << words[i];
    }
    out << ")";
    return out.str();
}

static std::string expand_template_str(const std::string& tmpl,
                                        const std::vector<std::pair<std::string,std::string>>& vars) {
    std::string result = tmpl;
    for (auto& [key, val] : vars) {
        result = replace_all_str(result, "{{" + key + "}}", val);
    }
    return result;
}

static int cmd_create(const std::vector<std::string>& args) {
    auto root = find_project_root();
    if (root.empty()) {
        root = source_checkout_root_from_current_binary();
    }

    // Parse args
    std::string name, type = "effect", manufacturer = "Pulp", output_path, tmpl;
    bool no_build = false;
    bool ci_mode = false;
    bool in_tree_mode = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--type" && i + 1 < args.size()) { type = args[++i]; continue; }
        if (args[i] == "--template" && i + 1 < args.size()) { tmpl = args[++i]; continue; }
        if (args[i] == "--manufacturer" && i + 1 < args.size()) { manufacturer = args[++i]; continue; }
        if (args[i] == "--output" && i + 1 < args.size()) { output_path = args[++i]; continue; }
        if (args[i] == "--in-tree" || args[i] == "--example") { in_tree_mode = true; continue; }
        if (args[i] == "--no-build") { no_build = true; continue; }
        if (args[i] == "--no-interactive" || args[i] == "--ci") { ci_mode = true; continue; }
        if (args[i] == "--help" || args[i] == "-h") {
            std::cout << "pulp create — create a new plugin project\n\n";
            std::cout << "Usage: pulp create <name> [options]\n\n";
            std::cout << "Options:\n";
            std::cout << "  --type <effect|instrument|app|bare>  Plugin type (default: effect)\n";
            std::cout << "  --template <name>                    Use named template (e.g. gain)\n";
            std::cout << "  --manufacturer <name>                Manufacturer (default: Pulp)\n";
            std::cout << "  --output <dir>                       Override output directory\n";
            std::cout << "  --in-tree, --example                 Add the project to examples/ using the local Pulp checkout\n";
            std::cout << "  --no-build                           Skip build after scaffolding\n";
            std::cout << "  --no-interactive, --ci               Non-interactive mode (use defaults)\n";
            std::cout << "\nDefault behavior: create a standalone product project, even inside the Pulp repo.\n";
            std::cout << "Inside the repo, the default location is next to the repo root unless\n";
            std::cout << "PULP_PROJECTS_DIR or ~/.pulp/config.toml overrides it.\n";
            return 0;
        }
        if (name.empty() && !args[i].empty() && args[i][0] != '-') { name = args[i]; continue; }
    }

    if (name.empty()) {
        std::cerr << "Usage: pulp create <name> [--type effect|instrument|app|bare] [options]\n";
        return 1;
    }

    // --template overrides --type for template directory lookup
    std::string template_key = tmpl.empty() ? type : tmpl;

    if (tmpl.empty() && type != "effect" && type != "instrument" && type != "app" && type != "bare") {
        std::cerr << "Error: --type must be 'effect', 'instrument', 'app', or 'bare'\n";
        return 1;
    }

    if (in_tree_mode && root.empty()) {
        std::cerr << "Error: --in-tree/--example can only be used from inside the Pulp repo\n";
        return 1;
    }

    bool standalone_mode = !in_tree_mode;

    // In CI mode, suppress progress output (errors still go to stderr)
    auto log = [&](const std::string& msg) {
        if (!ci_mode) std::cout << msg;
    };

    // Compute names
    std::string class_name = to_class_name(name);
    std::string lower_name = to_lower_name(name);
    std::string ns = to_namespace_name(name);
    std::string factory = ns;
    std::string plugin_code = make_plugin_code(class_name);
    std::string aax_product_code = make_aax_product_code(class_name);
    std::string mfr_code = make_mfr_code(manufacturer);
    std::string bundle_id = "com." + to_namespace_name(manufacturer) + "." + ns;
    std::string header_name = replace_all_str(lower_name, "-", "_") + ".hpp";

    // Output directory
    fs::path out_dir;
    if (!output_path.empty()) {
        out_dir = fs::path(output_path);
        if (!out_dir.is_absolute()) out_dir = fs::current_path() / out_dir;
    } else if (in_tree_mode) {
        out_dir = root / "examples" / lower_name;
    } else {
        out_dir = resolve_create_projects_base_dir(root) / lower_name;
    }

    if (in_tree_mode) {
        auto examples_root = root / "examples";
        if (!path_is_within(out_dir, examples_root)) {
            std::cerr << "Error: --in-tree projects must live under " << examples_root.string() << "\n";
            return 1;
        }
    } else if (!root.empty() && path_is_within(out_dir, root)) {
        std::cerr << "Error: standalone product projects must live outside the Pulp repo\n";
        std::cerr << "  Use --in-tree to scaffold under examples/, or choose --output outside\n";
        std::cerr << "  " << root.string() << "\n";
        return 1;
    }

    if (fs::exists(out_dir)) {
        std::cerr << "Error: " << out_dir.string() << " already exists\n";
        return 1;
    }

    // Quick doctor check (system-level only for standalone)
    log("Checking environment...\n");
    auto checks = run_doctor_checks(standalone_mode ? fs::path{} : root, standalone_mode);
    bool env_ok = true;
    for (auto& c : checks) {
        if (!c.passed) {
            std::cout << "  \xe2\x9c\x97 " << c.name;
            if (!c.fix.empty()) std::cout << " — fix: " << c.fix;
            std::cout << "\n";
            env_ok = false;
        }
    }
    if (!env_ok) {
        std::cerr << "\nEnvironment issues found. Run `pulp doctor --fix` first.\n";
        return 1;
    }
    log("  \xe2\x9c\x93 Environment OK\n\n");

    if (standalone_mode && !root.empty()) {
        const bool needs_vst3 = type != "app" && type != "bare" && !checkout_supports_vst3(root);
#ifdef __APPLE__
        const bool needs_au = type != "app" && type != "bare" && !checkout_supports_au(root);
#else
        const bool needs_au = false;
#endif
        if (needs_vst3 || needs_au) {
            log("Preparing current checkout dependencies for standalone plugin formats...\n");
            if (ensure_checkout_dependencies(root) != 0) {
                std::cerr << "Error: could not prepare checkout dependencies.\n";
                return 1;
            }
            log("\n");
        }
    }

    auto formats = default_create_formats(root, type);
    if (standalone_mode && !root.empty() && type != "app" && type != "bare") {
        if (formats.find("VST3") == std::string::npos) {
            print_warn("VST3 SDK unavailable in current checkout — generating without VST3 support");
        }
#ifdef __APPLE__
        if (formats.find("AU") == std::string::npos) {
            print_warn("AudioUnitSDK unavailable in current checkout — generating without AU support");
        }
#endif
    }
#if defined(__APPLE__) || defined(_WIN32)
    if (formats.find("AAX") != std::string::npos && find_aax_sdk_root().empty()) {
        log("AAX is optional. Build the other formats now, or install the AAX SDK later and set PULP_AAX_SDK_DIR.\n");
        log("See https://developer.avid.com/aax/ for the AAX SDK and DigiShell/AAX Validator downloads.\n\n");
    }
#endif

    fs::path sdk_dir;
    std::string sdk_version = PULP_SDK_VERSION;
    fs::path templates_base = root.empty() ? fs::path{} : root / "tools" / "templates";

    if (standalone_mode && root.empty()) {
        log("Standalone mode — fetching SDK...\n");
        sdk_dir = ensure_sdk(sdk_version);
        if (sdk_dir.empty()) {
            std::cerr << "Error: could not obtain Pulp SDK. Check your internet connection.\n";
            return 1;
        }
        templates_base = sdk_dir / "templates";
    } else if (standalone_mode) {
        log("Standalone mode — using templates from the current checkout.\n");
    }

    // Use underscored lower name for C++ filenames (hyphens illegal in identifiers)
    std::string lower_name_underscored = replace_all_str(lower_name, "-", "_");

    // Template variables
    std::vector<std::pair<std::string,std::string>> vars = {
        {"PLUGIN_NAME", name},
        {"CLASS_NAME", class_name},
        {"LOWER_NAME", lower_name_underscored},
        {"PLUGIN_URI", "http://pulp.audio/plugins/" + lower_name},
        {"NAMESPACE", ns},
        {"FACTORY_NAME", factory},
        {"HEADER_NAME", header_name},
        {"TARGET_NAME", class_name},
        {"MANUFACTURER", manufacturer},
        {"MANUFACTURER_CODE", mfr_code},
        {"BUNDLE_ID", bundle_id},
        {"VERSION", "1.0.0"},
        {"PLUGIN_CODE", plugin_code},
        {"AAX_PRODUCT_CODE", aax_product_code},
        {"AAX_NATIVE_CODE", plugin_code},
        {"FORMATS", formats},
        {"DESCRIPTION", type == "app" ? "A standalone Pulp audio application" :
                        type == "bare" ? "A minimal Pulp project" :
                        "A Pulp audio " + type},
        {"VST3_UID", make_vst3_uid()},
        {"SDK_VERSION", sdk_version},
    };

    // Determine template directories
    // For standalone: CMakeLists.txt comes from standalone/<type>, other files from <type>
    // For in-repo: all files come from <type> as before
    auto source_template_dir = templates_base / template_key;
    auto cmake_template_dir = standalone_mode
        ? templates_base / "standalone" / template_key
        : source_template_dir;

    if (standalone_mode && !root.empty()) {
        auto local_templates_base = root / "tools" / "templates";
        if (!fs::exists(source_template_dir) && fs::exists(local_templates_base / template_key)) {
            source_template_dir = local_templates_base / template_key;
        }
        if (!fs::exists(cmake_template_dir) &&
            fs::exists(local_templates_base / "standalone" / template_key)) {
            cmake_template_dir = local_templates_base / "standalone" / template_key;
        }
    }

    // Fall back: if standalone templates don't ship a standalone-specific CMakeLists yet,
    // use the source template variant.
    if (standalone_mode && !fs::exists(cmake_template_dir)) {
        cmake_template_dir = source_template_dir;
    }

    if (!fs::exists(source_template_dir)) {
        std::cerr << "Error: template directory not found at " << source_template_dir.string() << "\n";
        if (!tmpl.empty())
            std::cerr << "Available templates: effect, instrument, gain\n";
        return 1;
    }

    fs::create_directories(out_dir);
    if (standalone_mode) {
        log("Mode: standalone product project (default)\n");
        if (!root.empty()) {
            log("  Creating outside the repo so the generated project behaves like an end-user install.\n");
            log("  Use --in-tree to add an example under examples/ instead.\n");
        }
    } else {
        log("Mode: in-tree example project\n");
        log("  Using the local checkout and adding the project to examples/.\n");
    }
    log("Creating " + name + " (" + type + ") at " + out_dir.string() + "\n\n");

    struct FileMapping { std::string tmpl; std::string output; };
    std::string test_name = "test_" + replace_all_str(lower_name, "-", "_") + ".cpp";
    std::vector<FileMapping> file_map = {
        {"processor.hpp.template", header_name},
        {"CMakeLists.txt.template", "CMakeLists.txt"},
        {"clap_entry.cpp.template", "clap_entry.cpp"},
        {"vst3_entry.cpp.template", "vst3_entry.cpp"},
        {"lv2_entry.cpp.template", "lv2_entry.cpp"},
        {"au_v2_entry.cpp.template", "au_v2_entry.cpp"},
        {"aax_entry.cpp.template", "aax_entry.cpp"},
        {"test.cpp.template", test_name},
    };

    for (auto& [tmpl_file, outfile] : file_map) {
        if (tmpl_file == "clap_entry.cpp.template" && formats.find("CLAP") == std::string::npos) continue;
        if (tmpl_file == "vst3_entry.cpp.template" && formats.find("VST3") == std::string::npos) continue;
        if (tmpl_file == "lv2_entry.cpp.template" && formats.find("LV2") == std::string::npos) continue;
        if (tmpl_file == "au_v2_entry.cpp.template" && formats.find("AU") == std::string::npos) continue;
        if (tmpl_file == "aax_entry.cpp.template" && formats.find("AAX") == std::string::npos) continue;
        // CMakeLists.txt comes from cmake_template_dir, others from source_template_dir
        auto tmpl_path = (tmpl_file == "CMakeLists.txt.template")
            ? cmake_template_dir / tmpl_file
            : source_template_dir / tmpl_file;
        if (!fs::exists(tmpl_path)) continue;
        auto content = read_file_contents(tmpl_path);
        auto expanded = expand_template_str(content, vars);
        std::ofstream f(out_dir / outfile);
        f << expanded;
        log("  Created " + outfile + "\n");
    }

    // Standalone main.cpp
    if (formats.find("Standalone") != std::string::npos) {
        std::ofstream f(out_dir / "main.cpp");
        f << "#include \"" << header_name << "\"\n";
        f << "#include <pulp/format/standalone.hpp>\n\n";
        f << "int main() {\n";
        f << "    pulp::format::StandaloneApp app(" << ns << "::create_" << factory << ");\n";
        f << "    pulp::format::StandaloneConfig config;\n";
        if (type == "instrument") {
            f << "    config.input_channels = 0;\n";
        } else {
            f << "    config.input_channels = 2;\n";
        }
        f << "    config.output_channels = 2;\n";
        f << "    app.set_config(config);\n";
        f << "    return app.run_with_editor(false) ? 0 : 1;\n";
        f << "}\n";
        log("  Created main.cpp\n");
    }

    // Copy UI script directory if template includes one
    auto ui_template_dir = source_template_dir / "ui";
    if (fs::exists(ui_template_dir)) {
        auto ui_out_dir = out_dir / "ui";
        fs::create_directories(ui_out_dir);
        for (auto& entry : fs::directory_iterator(ui_template_dir)) {
            auto content = read_file_contents(entry.path());
            auto expanded = expand_template_str(content, vars);
            auto out_path = ui_out_dir / entry.path().filename();
            std::ofstream f(out_path);
            f << expanded;
            std::cout << "  Created ui/" << entry.path().filename().string() << "\n";
        }
    }

    // Generate pulp.toml for standalone projects
    if (standalone_mode) {
        std::ofstream f(out_dir / "pulp.toml");
        f << "[pulp]\n";
        f << "sdk_version = \"" << sdk_version << "\"\n";
        if (!root.empty()) {
            auto local_sdk_dir = local_sdk_cache_path(sdk_version);
            f << "sdk_path = \"" << local_sdk_dir.generic_string() << "\"\n";
            f << "sdk_checkout = \"" << root.generic_string() << "\"\n";
        }
        log("  Created pulp.toml\n");
    }

    // Add to examples/CMakeLists.txt (in-tree mode only)
    if (in_tree_mode) {
        auto examples_cmake = root / "examples" / "CMakeLists.txt";
        auto rel_dir = fs::relative(out_dir, root / "examples").generic_string();

        if (fs::exists(examples_cmake)) {
            std::string cmake_content = read_file_contents(examples_cmake);
            std::string add_line = "add_subdirectory(" + rel_dir + ")";
            if (cmake_content.find(add_line) == std::string::npos) {
                std::ofstream f(examples_cmake, std::ios::app);
                f << "\n# " << name << " (generated by pulp create)\n"
                  << add_line << "\n";
                log("  Added to examples/CMakeLists.txt\n");
            }
        }
    }

    log("\n");

    if (no_build) {
        log("Scaffolding complete. Run `pulp build` to build.\n");
        return 0;
    }

    auto build_targets = pulp::cli::create_default_build_targets(class_name, type, formats);
    auto append_build_targets = [&](std::string& cmd) {
        cmd += " --target";
        for (const auto& target : build_targets) {
            cmd += " " + target;
        }
    };

    // Build
    log("Building...\n");
    if (standalone_mode) {
        if (sdk_dir.empty()) {
            if (!root.empty()) {
                log("Preparing local SDK from the current checkout...\n");
                sdk_dir = ensure_checkout_sdk(root, sdk_version);
            } else {
                log("Fetching SDK for standalone build...\n");
                sdk_dir = ensure_sdk(sdk_version);
            }
            if (sdk_dir.empty()) {
                std::cerr << "Error: could not obtain Pulp SDK. ";
                if (!root.empty()) {
                    std::cerr << "Run with --no-build to scaffold only, or use --in-tree while developing Pulp.\n";
                } else {
                    std::cerr << "Check your internet connection.\n";
                }
                return 1;
            }
        }

        // Standalone: configure with CMAKE_PREFIX_PATH pointing to SDK
        std::string configure_cmd = "cmake -S " + out_dir.string()
            + " -B " + (out_dir / "build").string()
            + " -DCMAKE_BUILD_TYPE=Debug"
            + " -DCMAKE_PREFIX_PATH=" + sdk_dir.string();
        append_windows_visual_studio_generator_args(configure_cmd);
        int rc = run_with_spinner(configure_cmd, "Configuring");
        if (rc != 0) {
            std::cerr << "Configure failed.\n";
            return rc;
        }

        std::string build_cmd = "cmake --build " + shell_quote(out_dir / "build");
        append_build_targets(build_cmd);
        rc = run_with_spinner(build_cmd, "Building");
        if (rc != 0) {
            std::cerr << "Build failed.\n";
            return rc;
        }

        // Test
        log("\nRunning tests...\n");
        rc = run("ctest --test-dir " + (out_dir / "build").string() + " --output-on-failure");
        if (rc != 0) {
            std::cerr << "Tests failed.\n";
            return rc;
        }
    } else {
        auto example_rel_dir = fs::relative(out_dir, root / "examples");
        std::string configure_cmd = "cmake -S " + shell_quote(root) + " -B "
                                  + shell_quote(root / "build")
                                  + " -DCMAKE_BUILD_TYPE=Debug";
        append_windows_visual_studio_generator_args(configure_cmd);
        int rc = run_with_spinner(configure_cmd, "Configuring");
        if (rc != 0) {
            std::cerr << "Configure failed.\n";
            return rc;
        }

        std::string build_cmd = "cmake --build " + shell_quote(root / "build");
        append_build_targets(build_cmd);
        rc = run_with_spinner(build_cmd, "Building");
        if (rc != 0) {
            std::cerr << "Build failed.\n";
            return rc;
        }

        // Test — run the test binary directly since ctest may not have discovered new tests yet
        log("\nRunning tests...\n");
        auto test_binary = root / "build" / "examples" / example_rel_dir / (class_name + "-test");
        if (fs::exists(test_binary)) {
            rc = run(test_binary.string());
        } else {
            rc = run("ctest --test-dir " + (root / "build").string() + " -R \"" + name + "\" --output-on-failure");
        }
        if (rc != 0) {
            std::cerr << "Tests failed.\n";
            return rc;
        }
    }

    std::string test_filter = replace_all_str(lower_name, "-", "_");

    // Success report
    std::cout << "\n  \xe2\x9c\x93 " << name << " is ready!\n\n";
    std::cout << "  Source:     " << out_dir.string() << "\n";

    if (standalone_mode) {
        std::cout << "  SDK:        " << sdk_dir.string() << "\n";
    }

    auto build_dir = standalone_mode ? (out_dir / "build") : (root / "build");
    auto matches_format_artifact = [&](const fs::path& path, const std::string& fmt) {
        const auto filename = path.filename().string();
        if (fmt == "VST3") {
            return filename == class_name + ".vst3"
                || filename == class_name + ".dll"
                || filename == "lib" + class_name + ".so";
        }
        if (fmt == "CLAP") return filename == class_name + ".clap";
        if (fmt == "LV2") return filename == class_name + ".lv2";
        if (fmt == "AU") return filename == class_name + ".component";
        if (fmt == "AAX") return filename == class_name + ".aaxplugin";
        return false;
    };
    auto find_format_artifact = [&](const std::string& fmt) -> fs::path {
        auto fmt_dir = build_dir / fmt;
        if (!fs::exists(fmt_dir)) return {};
        for (auto& entry : fs::recursive_directory_iterator(fmt_dir)) {
            if (matches_format_artifact(entry.path(), fmt)) {
                return entry.path();
            }
        }
        return {};
    };
    for (auto fmt : {"VST3", "CLAP", "LV2", "AU", "AAX"}) {
        if (auto artifact = find_format_artifact(fmt); !artifact.empty()) {
            std::cout << "  " << fmt << ":       " << artifact.string() << "\n";
        }
    }
    auto find_standalone_artifact = [&]() -> fs::path {
        auto standalone_app = build_dir / (class_name + ".app");
        if (fs::exists(standalone_app)) return standalone_app;
        auto standalone_bin = build_dir / class_name;
        if (fs::exists(standalone_bin)) return standalone_bin;
        for (auto& entry : fs::recursive_directory_iterator(build_dir)) {
            const auto filename = entry.path().filename().string();
            if (filename == class_name + ".exe" || filename == class_name || filename == class_name + ".app") {
                return entry.path();
            }
        }
        return {};
    };
    if (auto standalone = find_standalone_artifact(); !standalone.empty()) {
        std::cout << "  Standalone: " << standalone.string() << "\n";
    }

    std::cout << "\n  Next steps:\n";
    if (standalone_mode) {
        std::cout << "    cd " << out_dir.string() << "\n";
    }
    std::cout << "    pulp build              # rebuild after changes\n";
    std::cout << "    pulp test -R " << test_filter << "  # run tests\n";
    std::cout << "    pulp validate           # validate plugin formats\n";
    return 0;
}

// ── SDK ────────────────────────────────────────────────────────────────────

static int cmd_sdk(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cout << "pulp sdk — manage the Pulp SDK installation\n\n";
        std::cout << "Subcommands:\n";
        std::cout << "  install [--version X.Y.Z]   Download and cache the SDK from GitHub releases\n";
        std::cout << "  install --local             Build and install the SDK from the current checkout\n";
        std::cout << "  status                      Show installed SDK versions\n";
        std::cout << "  clean                       Remove all cached SDK versions\n";
        return 0;
    }

    auto home = pulp_home();
    if (home.empty()) {
        std::cerr << "Error: could not determine home directory.\n";
        return 1;
    }

    std::string sub = args[0];

    if (sub == "install") {
        bool from_local = false;
        std::string version = PULP_SDK_VERSION;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--local") from_local = true;
            else if (args[i] == "--version" && i + 1 < args.size()) version = args[++i];
        }

        if (from_local) {
            auto repo_root = find_project_root();
            if (repo_root.empty()) {
                std::cerr << "Error: --local requires running from inside a Pulp checkout.\n";
                return 1;
            }
            std::cout << "Building SDK from local checkout...\n";
            auto sdk = ensure_checkout_sdk(repo_root, version);
            if (sdk.empty()) {
                std::cerr << "SDK build failed.\n";
                return 1;
            }
            std::cout << "SDK v" << version << " installed at " << sdk.string() << "\n";
        } else {
            std::cout << "Downloading SDK v" << version << "...\n";
            auto sdk = ensure_sdk(version);
            if (sdk.empty()) {
                std::cerr << "SDK download failed.\n";
                return 1;
            }
            std::cout << "SDK v" << version << " installed at " << sdk.string() << "\n";
        }
        return 0;
    }

    if (sub == "status") {
        std::cout << "Pulp SDK Status\n";
        std::cout << "===============\n\n";

        // Downloaded SDKs
        auto sdk_base = home / "sdk";
        bool found = false;
        if (fs::exists(sdk_base)) {
            for (auto& entry : fs::directory_iterator(sdk_base)) {
                if (!entry.is_directory()) continue;
                auto ver = entry.path().filename().string();
                auto vt = entry.path() / "version.txt";
                if (fs::exists(vt)) {
                    std::cout << "  v" << ver << " (downloaded) — " << entry.path().string() << "\n";
                    found = true;
                }
            }
        }

        // Local-built SDKs
        auto local_base = home / "sdk-local";
        if (fs::exists(local_base)) {
            for (auto& plat : fs::directory_iterator(local_base)) {
                if (!plat.is_directory()) continue;
                for (auto& ver_entry : fs::directory_iterator(plat.path())) {
                    if (!ver_entry.is_directory()) continue;
                    auto config = ver_entry.path() / "lib" / "cmake" / "Pulp" / "PulpConfig.cmake";
                    if (fs::exists(config)) {
                        std::cout << "  v" << ver_entry.path().filename().string()
                                  << " (local build, " << plat.path().filename().string()
                                  << ") — " << ver_entry.path().string() << "\n";
                        found = true;
                    }
                }
            }
        }

        if (!found) {
            std::cout << "  No SDK versions installed.\n";
            std::cout << "  Run: pulp sdk install\n";
        }
        return 0;
    }

    if (sub == "clean") {
        auto sdk_base = home / "sdk";
        auto local_base = home / "sdk-local";
        auto build_base = home / "sdk-build";
        int removed = 0;
        for (auto* dir : {&sdk_base, &local_base, &build_base}) {
            if (fs::exists(*dir)) {
                fs::remove_all(*dir);
                ++removed;
            }
        }
        std::cout << "Removed " << removed << " SDK cache directories.\n";
        return 0;
    }

    std::cerr << "Unknown sdk subcommand: " << sub << "\n";
    std::cerr << "Run `pulp sdk` for usage.\n";
    return 1;
}

// ── Cache ───────────────────────────────────────────────────────────────────

static int cmd_cache(const std::vector<std::string>& args) {
    auto home = pulp_home();
    if (home.empty()) {
        std::cerr << "Error: could not determine home directory.\n";
        return 1;
    }

    auto cache_dir = home / "cache";

    if (args.empty()) {
        std::cout << "pulp cache — manage the Pulp SDK and asset cache (~/.pulp/ by default)\n\n";
        std::cout << "Subcommands:\n";
        std::cout << "  status           Show cache contents and sizes\n";
        std::cout << "  fetch skia       Download Skia GPU rendering binaries\n";
        std::cout << "  clean            Remove all cached assets\n";
        return 0;
    }

    std::string sub = args[0];

    if (sub == "status") {
        std::cout << "Pulp Cache\n";
        std::cout << "==========\n\n";
        std::cout << "Location: " << home.string() << "\n\n";

        // SDK versions
        auto sdk_base = home / "sdk";
        if (fs::exists(sdk_base)) {
            std::cout << "SDKs:\n";
            for (auto& entry : fs::directory_iterator(sdk_base)) {
                if (entry.is_directory()) {
                    std::cout << "  v" << entry.path().filename().string();
                    if (fs::exists(entry.path() / "version.txt"))
                        std::cout << " (complete)";
                    else
                        std::cout << " (incomplete)";
                    std::cout << "\n";
                }
            }
        } else {
            std::cout << "SDKs: none cached\n";
        }

        // Cache assets
        if (fs::exists(cache_dir)) {
            std::cout << "\nAssets:\n";
            bool any = false;
            for (auto& entry : fs::directory_iterator(cache_dir)) {
                any = true;
                auto size = fs::file_size(entry.path());
                std::string size_str;
                if (size > 1024 * 1024)
                    size_str = std::to_string(size / (1024 * 1024)) + " MB";
                else if (size > 1024)
                    size_str = std::to_string(size / 1024) + " KB";
                else
                    size_str = std::to_string(size) + " B";
                std::cout << "  " << entry.path().filename().string()
                          << " (" << size_str << ")\n";
            }
            if (!any) std::cout << "  (empty)\n";
        } else {
            std::cout << "\nAssets: none cached\n";
        }

        return 0;
    }

    if (sub == "fetch") {
        if (args.size() < 2) {
            std::cerr << "Usage: pulp cache fetch <asset>\n";
            std::cerr << "Available assets: skia\n";
            return 1;
        }

        std::string asset = args[1];
        if (asset != "skia") {
            std::cerr << "Unknown asset: " << asset << "\n";
            std::cerr << "Available assets: skia\n";
            return 1;
        }

        auto platform = detect_platform();
        std::string sdk_tarball_name = "pulp-sdk-" + platform + ".tar.gz";
        auto sdk_cache = cache_dir / sdk_tarball_name;

        if (fs::exists(sdk_cache)) {
            std::cout << "SDK (includes Skia) already cached at " << sdk_cache.string() << "\n";
            return 0;
        }

        fs::create_directories(cache_dir);
        std::string url = "https://github.com/" + std::string(PULP_GITHUB_REPO)
                        + "/releases/download/v" + std::string(PULP_SDK_VERSION)
                        + "/" + sdk_tarball_name;

        std::cout << "Downloading Skia binaries for " << platform << "...\n";

        std::string download_cmd;
#ifdef _WIN32
        download_cmd = "powershell -Command \"Invoke-WebRequest -Uri '" + url
                     + "' -OutFile '" + sdk_cache.string() + "'\"";
#else
        download_cmd = "curl -fSL -o " + sdk_cache.string() + " " + url;
#endif

        int rc = run_with_spinner(download_cmd, "Downloading Skia");
        if (rc != 0) {
            std::cerr << "Error: failed to download Skia binaries.\n";
            std::cerr << "  URL: " << url << "\n";
            std::cerr << "  Skia may not be available for this platform/version.\n";
            fs::remove(sdk_cache);
            return 1;
        }

        print_ok("Skia binaries cached at " + sdk_cache.string());
        return 0;
    }

    if (sub == "clean") {
        if (fs::exists(cache_dir)) {
            fs::remove_all(cache_dir);
            std::cout << "Cache cleared.\n";
        } else {
            std::cout << "Cache already empty.\n";
        }
        return 0;
    }

    std::cerr << "Unknown cache subcommand: " << sub << "\n";
    std::cerr << "Run `pulp cache` for usage.\n";
    return 1;
}

// ── Upgrade ─────────────────────────────────────────────────────────────────

static int cmd_upgrade(const std::vector<std::string>& args) {
    std::string target_version;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help" || args[i] == "-h") {
            std::cout << "pulp upgrade — update the Pulp CLI to the latest version\n\n";
            std::cout << "Usage: pulp upgrade [version]\n\n";
            std::cout << "If no version is specified, upgrades to the latest release.\n";
            std::cout << "Requires curl (macOS/Linux) or Invoke-WebRequest (Windows).\n";
            return 0;
        }
        if (args[i][0] != '-') target_version = args[i];
    }

    // Check current version
    std::cout << "Checking for updates...\n";

    // Fetch latest version from GitHub
    std::string version_cmd = "curl -fsSL https://api.github.com/repos/danielraffel/pulp/releases/latest 2>/dev/null"
                              " | grep '\"tag_name\"' | sed 's/.*\"v\\(.*\\)\".*/\\1/'";
    std::string latest = exec_output(version_cmd);

    if (latest.empty() && target_version.empty()) {
        std::cerr << "Error: could not fetch latest version from GitHub.\n";
        std::cerr << "  Check your internet connection, or specify a version:\n";
        std::cerr << "    pulp upgrade 0.2.0\n";
        return 1;
    }

    std::string version = target_version.empty() ? latest : target_version;
    std::cout << "  Target version: " << version << "\n";

    // Detect platform
    std::string platform, arch;
#ifdef __APPLE__
    platform = "darwin";
#elif defined(_WIN32)
    platform = "windows";
#else
    platform = "linux";
#endif

#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    arch = "x86_64";
#else
    arch = "unknown";
#endif

    // Determine install location (where is the current binary?)
    std::string self_path;
#ifdef __APPLE__
    {
        char buf[1024];
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) == 0) {
            self_path = fs::canonical(buf).string();
        }
    }
#elif defined(__linux__)
    self_path = fs::canonical("/proc/self/exe").string();
#elif defined(_WIN32)
    {
        char buf[MAX_PATH];
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        self_path = fs::canonical(buf).string();
    }
#endif

    if (self_path.empty()) {
        std::cerr << "Error: could not determine current binary path.\n";
        std::cerr << "  Download manually from: https://github.com/danielraffel/pulp/releases\n";
        return 1;
    }

    auto install_dir = fs::path(self_path).parent_path();

    std::string ext = (platform == "windows") ? "zip" : "tar.gz";
    std::string tarball = "pulp-" + version + "-" + platform + "-" + arch + "." + ext;
    std::string url = "https://github.com/danielraffel/pulp/releases/download/v" + version + "/" + tarball;

    std::cout << "  Downloading " << tarball << "...\n";

    // Download and extract
    std::string tmp_dir = "/tmp/pulp-upgrade-" + version;
    int rc = run("mkdir -p " + tmp_dir + " && curl -fSL -o " + tmp_dir + "/" + tarball + " " + url);
    if (rc != 0) {
        std::cerr << "Download failed. Check: https://github.com/danielraffel/pulp/releases/tag/v" << version << "\n";
        run("rm -rf " + tmp_dir);
        return 1;
    }

    rc = run("tar -xzf " + tmp_dir + "/" + tarball + " -C " + tmp_dir);
    if (rc != 0) {
        std::cerr << "Extraction failed.\n";
        run("rm -rf " + tmp_dir);
        return 1;
    }

    // Replace binary
    std::string new_binary = tmp_dir + "/pulp";
    if (!fs::exists(new_binary)) {
        std::cerr << "Error: extracted archive does not contain 'pulp' binary.\n";
        run("rm -rf " + tmp_dir);
        return 1;
    }

    // Backup current binary, then replace
    std::string backup = self_path + ".bak";
    try {
        if (fs::exists(backup)) fs::remove(backup);
        fs::rename(self_path, backup);
        fs::copy_file(new_binary, self_path);
        fs::permissions(self_path, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write
                                 | fs::perms::group_exec | fs::perms::group_read
                                 | fs::perms::others_exec | fs::perms::others_read);
        fs::remove(backup);
    } catch (const std::exception& e) {
        std::cerr << "Error replacing binary: " << e.what() << "\n";
        // Try to restore backup
        if (fs::exists(backup) && !fs::exists(self_path)) {
            fs::rename(backup, self_path);
        }
        run("rm -rf " + tmp_dir);
        return 1;
    }

    run("rm -rf " + tmp_dir);

    std::cout << "\n  \xe2\x9c\x93 Pulp CLI upgraded to v" << version << "\n";
    return 0;
}

// ── Run ─────────────────────────────────────────────────────────────────────

static int cmd_run(const std::vector<std::string>& args) {
    bool standalone_mode = false;
    auto root = resolve_active_project_root(&standalone_mode);
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    std::string target_name;
    std::vector<std::string> pass_through;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help" || args[i] == "-h") {
            std::cout << "pulp run — launch a standalone Pulp application\n\n";
            std::cout << "Usage: pulp run [target] [-- args...]\n\n";
            std::cout << "If no target is specified, finds the first standalone binary in the active project build.\n";
            std::cout << "Arguments after -- are passed to the launched application.\n";
            return 0;
        }
        if (args[i] == "--") {
            for (size_t j = i + 1; j < args.size(); ++j)
                pass_through.push_back(args[j]);
            break;
        }
        if (target_name.empty() && args[i][0] != '-') {
            target_name = args[i];
        }
    }

    auto build_dir = root / "build";
    if (!fs::exists(build_dir / "CMakeCache.txt")) {
        std::cerr << "Error: project not built yet. Run `pulp build` first.\n";
        return 1;
    }

    // Find standalone binary
    fs::path binary;
    auto app_search_root = standalone_mode ? (build_dir / "bin") : (build_dir / "examples");

    // On macOS, also check for .app bundles in the build directory
#ifdef __APPLE__
    auto find_app_bundle = [&](const fs::path& search_dir) -> fs::path {
        if (!fs::exists(search_dir)) return {};
        for (auto& entry : fs::directory_iterator(search_dir)) {
            if (!entry.is_directory()) continue;
            auto name = entry.path().filename().string();
            if (name.size() > 4 && name.substr(name.size() - 4) == ".app") {
                auto macos_dir = entry.path() / "Contents" / "MacOS";
                if (fs::exists(macos_dir)) {
                    for (auto& exec_entry : fs::directory_iterator(macos_dir)) {
                        if (!exec_entry.is_regular_file()) continue;
                        auto st = fs::status(exec_entry.path());
                        if ((st.permissions() & fs::perms::owner_exec) != fs::perms::none) {
                            return exec_entry.path();
                        }
                    }
                }
            }
        }
        return {};
    };
#endif

    if (!target_name.empty()) {
        // Search for the named target
        if (standalone_mode) {
            if (fs::exists(app_search_root)) {
                for (auto& file : fs::directory_iterator(app_search_root)) {
                    if (!file.is_regular_file()) continue;
                    auto fname = file.path().filename().string();
                    if (fname.find("-test") != std::string::npos) continue;
                    if (fname == target_name || file.path().stem().string() == target_name) {
                        auto st = fs::status(file.path());
                        if ((st.permissions() & fs::perms::owner_exec) != fs::perms::none) {
                            binary = file.path();
                            break;
                        }
                    }
                }
            }
        } else if (fs::exists(app_search_root)) {
            for (auto& dir_entry : fs::directory_iterator(app_search_root)) {
                if (!dir_entry.is_directory()) continue;
                for (auto& file : fs::directory_iterator(dir_entry.path())) {
                    if (!file.is_regular_file()) continue;
                    auto fname = file.path().filename().string();
                    // Skip test binaries
                    if (fname.find("-test") != std::string::npos) continue;
                    if (fname == target_name || file.path().stem().string() == target_name) {
                        auto st = fs::status(file.path());
                        if ((st.permissions() & fs::perms::owner_exec) != fs::perms::none) {
                            binary = file.path();
                            break;
                        }
                    }
                }
                if (!binary.empty()) break;
            }
        }
        if (binary.empty()) {
            std::cerr << "Error: could not find standalone binary '" << target_name
                      << "' in " << app_search_root.string() << "\n";
            std::cerr << "  Run `pulp build` to build, then try again.\n";
            return 1;
        }
    } else {
        if (standalone_mode) {
            if (fs::exists(app_search_root)) {
                for (auto& file : fs::directory_iterator(app_search_root)) {
                    if (!file.is_regular_file()) continue;
                    auto fname = file.path().filename().string();
                    if (fname.find("-test") != std::string::npos) continue;
                    if (fname.find("cmake") != std::string::npos) continue;
                    if (fname.find(".") != std::string::npos) continue;
                    auto st = fs::status(file.path());
                    if ((st.permissions() & fs::perms::owner_exec) != fs::perms::none) {
                        binary = file.path();
                        break;
                    }
                }
            }
        } else if (fs::exists(app_search_root)) {
            for (auto& dir_entry : fs::directory_iterator(app_search_root)) {
                if (!dir_entry.is_directory()) continue;
                for (auto& file : fs::directory_iterator(dir_entry.path())) {
                    if (!file.is_regular_file()) continue;
                    auto fname = file.path().filename().string();
                    // Skip test binaries and cmake artifacts
                    if (fname.find("-test") != std::string::npos) continue;
                    if (fname.find("cmake") != std::string::npos) continue;
                    if (fname.find(".") != std::string::npos) continue;  // skip files with extensions
                    auto st = fs::status(file.path());
                    if ((st.permissions() & fs::perms::owner_exec) != fs::perms::none) {
                        binary = file.path();
                        break;
                    }
                }
                if (!binary.empty()) break;
            }
        }
        // Fallback: search for .app bundles on macOS
#ifdef __APPLE__
        if (binary.empty()) {
            binary = find_app_bundle(build_dir);
            // found .app bundle
        }
        if (binary.empty()) {
            binary = find_app_bundle(app_search_root);
            // found .app bundle
        }
#endif
        if (binary.empty()) {
            std::cerr << "Error: no standalone binary found in " << app_search_root.string() << "\n";
            std::cerr << "  Create one with: pulp create MyApp --type app\n";
            std::cerr << "  Or build an existing project: pulp build\n";
            return 1;
        }
    }

    std::cout << "Launching " << binary.filename().string() << "...\n";

    std::string cmd = binary.string();
    for (auto& arg : pass_through) {
        cmd += " \"" + arg + "\"";
    }

    return run(cmd);
}

// ── Docs helpers ────────────────────────────────────────────────────────────

static std::string read_file_contents(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Trim leading/trailing whitespace from a string
static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string strip_quotes(const std::string& s) {
    if (s.size() >= 2) {
        if ((s.front() == '"' && s.back() == '"') ||
            (s.front() == '\'' && s.back() == '\'')) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}

// Case-insensitive substring search
static bool icontains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    return it != haystack.end();
}

// Simple YAML line parser: returns value after "key: " on a line like "  key: value"
static std::string yaml_value(const std::string& line, const std::string& key) {
    auto pos = line.find(key + ":");
    if (pos == std::string::npos) return {};
    auto val_start = pos + key.size() + 1;
    if (val_start >= line.size()) return {};
    return trim(line.substr(val_start));
}

// ── pulp docs ───────────────────────────────────────────────────────────────

static int docs_index(const fs::path& docs_dir) {
    auto index_path = docs_dir / "status" / "docs-index.yaml";
    std::string content = read_file_contents(index_path);
    if (content.empty()) {
        std::cerr << "Error: docs index not found at " << index_path.string() << "\n";
        std::cerr << "Hint: the docs/ tree may not be set up yet.\n";
        return 1;
    }

    std::cout << "Available Documentation\n";
    std::cout << "=======================\n\n";

    std::istringstream stream(content);
    std::string line;
    std::string slug, path, kind;

    auto flush_entry = [&]() {
        if (!slug.empty()) {
            std::cout << "  " << slug;
            if (!kind.empty()) std::cout << " (" << kind << ")";
            std::cout << "\n";
            if (!path.empty()) std::cout << "    -> docs/" << path << "\n";
        }
        slug.clear(); path.clear(); kind.clear();
    };

    while (std::getline(stream, line)) {
        auto s = yaml_value(line, "slug");
        if (!s.empty()) {
            flush_entry();
            slug = s;
        }
        auto p = yaml_value(line, "path");
        if (!p.empty()) path = p;
        auto k = yaml_value(line, "kind");
        if (!k.empty()) kind = k;
    }
    flush_entry();

    std::cout << "\nUse `pulp docs open <slug>` to read a doc.\n";
    return 0;
}

static int docs_search(const fs::path& docs_dir, const std::string& query) {
    if (query.empty()) {
        std::cerr << "Usage: pulp docs search <query>\n";
        return 1;
    }
    if (!fs::exists(docs_dir)) {
        std::cerr << "Error: docs/ directory not found.\n";
        return 1;
    }

    int match_count = 0;
    for (auto& entry : fs::recursive_directory_iterator(docs_dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".md") continue;

        std::ifstream f(entry.path());
        if (!f.is_open()) continue;

        std::string line;
        int line_num = 0;
        bool file_printed = false;
        int matches_in_file = 0;

        while (std::getline(f, line)) {
            ++line_num;
            if (icontains(line, query)) {
                if (!file_printed) {
                    auto rel = fs::relative(entry.path(), docs_dir);
                    std::cout << "\ndocs/" << rel.string() << ":\n";
                    file_printed = true;
                }
                // Print up to 5 matches per file
                if (matches_in_file < 5) {
                    // Truncate long lines for display
                    std::string display = line;
                    if (display.size() > 120) display = display.substr(0, 117) + "...";
                    std::cout << "  " << line_num << ": " << trim(display) << "\n";
                }
                ++matches_in_file;
                ++match_count;
            }
        }
        if (matches_in_file > 5) {
            std::cout << "  ... and " << (matches_in_file - 5) << " more matches\n";
        }
    }

    if (match_count == 0) {
        std::cout << "No matches for \"" << query << "\" in docs/\n";
    } else {
        std::cout << "\n" << match_count << " match(es) found.\n";
    }
    return 0;
}

static int docs_open(const fs::path& docs_dir, const std::string& slug) {
    if (slug.empty()) {
        std::cerr << "Usage: pulp docs open <slug>\n";
        return 1;
    }

    auto index_path = docs_dir / "status" / "docs-index.yaml";
    std::string index_content = read_file_contents(index_path);
    if (index_content.empty()) {
        std::cerr << "Error: docs index not found at " << index_path.string() << "\n";
        return 1;
    }

    // Find the path for the given slug
    std::istringstream stream(index_content);
    std::string line;
    std::string current_slug, current_path;
    bool found = false;

    while (std::getline(stream, line)) {
        auto s = yaml_value(line, "slug");
        if (!s.empty()) {
            // Check if previous entry matched
            if (current_slug == slug && !current_path.empty()) {
                found = true;
                break;
            }
            current_slug = s;
            current_path.clear();
        }
        auto p = yaml_value(line, "path");
        if (!p.empty()) current_path = p;
    }
    // Check last entry
    if (!found && current_slug == slug && !current_path.empty()) {
        found = true;
    }

    if (!found) {
        std::cerr << "Error: no doc found for slug \"" << slug << "\"\n";
        std::cerr << "Run `pulp docs index` to see available docs.\n";
        return 1;
    }

    auto file_path = docs_dir / current_path;
    std::string content = read_file_contents(file_path);
    if (content.empty()) {
        std::cerr << "Error: file not found at " << file_path.string() << "\n";
        return 1;
    }

    std::cout << content;
    return 0;
}

static int docs_show_support(const fs::path& docs_dir, const std::string& thing) {
    if (thing.empty()) {
        std::cerr << "Usage: pulp docs show support <thing>\n";
        return 1;
    }

    auto matrix_path = docs_dir / "status" / "support-matrix.yaml";
    std::string content = read_file_contents(matrix_path);
    if (content.empty()) {
        std::cerr << "Error: support matrix not found at " << matrix_path.string() << "\n";
        return 1;
    }

    // Structured YAML lookup: parse section > entry > fields
    // The YAML has this structure:
    //   section_name:        (indent 0)
    //     entry_name:        (indent 2) — or "  - name: X" style not used here
    //       field: value     (indent 4)
    //
    // We match `thing` as either a section name or an entry name (exact, case-insensitive).

    std::istringstream stream(content);
    std::string line;
    bool found = false;

    std::string section_name;
    std::string entry_name;

    // Lowercase version of query for matching
    std::string query_lower = thing;
    for (auto& c : query_lower) c = static_cast<char>(std::tolower(c));

    // First pass: try exact match on section or entry names
    while (std::getline(stream, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        auto indent = line.find_first_not_of(' ');
        if (indent == std::string::npos) continue;

        // Top-level section (indent 0, ends with ':')
        if (indent == 0 && !trimmed.empty() && trimmed.back() == ':') {
            section_name = trimmed.substr(0, trimmed.size() - 1);
            continue;
        }

        // Entry-level key (indent 2, ends with ':')
        if (indent == 2 && !trimmed.empty() && trimmed.back() == ':') {
            entry_name = trimmed.substr(0, trimmed.size() - 1);

            std::string entry_lower = entry_name;
            for (auto& c : entry_lower) c = static_cast<char>(std::tolower(c));

            if (entry_lower == query_lower) {
                if (!found) {
                    std::cout << "Support info for \"" << thing << "\":\n\n";
                    found = true;
                }
                std::cout << "  Section:  " << section_name << "\n";
                std::cout << "  Entry:    " << entry_name << "\n";

                // Read child fields (indent > 2)
                while (std::getline(stream, line)) {
                    auto ni = line.find_first_not_of(' ');
                    if (ni == std::string::npos || ni <= 2) {
                        // Push back by re-processing this line next iteration
                        // We can't seek reliably on stringstream, so just break
                        break;
                    }
                    std::string field = trim(line);
                    if (field.empty() || field[0] == '#') continue;
                    auto colon = field.find(':');
                    if (colon != std::string::npos) {
                        auto key = trim(field.substr(0, colon));
                        auto val = trim(field.substr(colon + 1));
                        // Capitalize first letter of key for display
                        if (!key.empty()) key[0] = static_cast<char>(std::toupper(key[0]));
                        std::cout << "  " << key << ": " << val << "\n";
                    }
                }
                std::cout << "\n";
            }
            continue;
        }

        // Check if this is a "key: value" line at indent 2 inside subsystems section
        // (subsystems uses "module_name: status" format at indent 2)
        if (indent == 2 && !trimmed.empty() && trimmed.back() != ':') {
            auto colon = trimmed.find(':');
            if (colon != std::string::npos) {
                auto key = trim(trimmed.substr(0, colon));
                auto val = trim(trimmed.substr(colon + 1));
                std::string key_lower = key;
                for (auto& c : key_lower) c = static_cast<char>(std::tolower(c));

                if (key_lower == query_lower) {
                    if (!found) {
                        std::cout << "Support info for \"" << thing << "\":\n\n";
                        found = true;
                    }
                    std::cout << "  Section: " << section_name << "\n";
                    std::cout << "  " << key << ": " << val << "\n\n";
                }
            }
        }
    }

    // If exact match failed, try section-level match
    if (!found) {
        stream.clear();
        stream.str(content);
        section_name.clear();

        std::string section_lower;
        bool in_matching_section = false;

        while (std::getline(stream, line)) {
            std::string trimmed = trim(line);
            if (trimmed.empty() || trimmed[0] == '#') continue;

            auto indent = line.find_first_not_of(' ');
            if (indent == std::string::npos) continue;

            if (indent == 0 && !trimmed.empty() && trimmed.back() == ':') {
                section_name = trimmed.substr(0, trimmed.size() - 1);
                section_lower = section_name;
                for (auto& c : section_lower) c = static_cast<char>(std::tolower(c));
                in_matching_section = (section_lower == query_lower);
                if (in_matching_section && !found) {
                    std::cout << "Support info for \"" << thing << "\":\n\n";
                    std::cout << "[" << section_name << "]\n";
                    found = true;
                }
                continue;
            }

            if (in_matching_section && indent >= 2) {
                auto colon = trimmed.find(':');
                if (colon != std::string::npos && trimmed[0] != '-') {
                    auto key = trim(trimmed.substr(0, colon));
                    auto val = trim(trimmed.substr(colon + 1));
                    if (!val.empty()) {
                        std::cout << "  " << key << ": " << val << "\n";
                    } else {
                        std::cout << "  " << key << ":\n";
                    }
                }
            }
        }
    }

    if (!found) {
        std::cerr << "No support info found for \"" << thing << "\"\n";
        std::cerr << "Available sections: platforms, formats, audio_io, midi_io, rendering, subsystems\n";
        std::cerr << "Available entries: macos, windows, linux, vst3, au_v2, clap, standalone, etc.\n";
        return 1;
    }
    return 0;
}

static int docs_show_command(const fs::path& docs_dir, const std::string& name) {
    if (name.empty()) {
        std::cerr << "Usage: pulp docs show command <name>\n";
        return 1;
    }

    auto cmd_path = docs_dir / "status" / "cli-commands.yaml";
    std::string content = read_file_contents(cmd_path);
    if (content.empty()) {
        std::cerr << "Error: CLI commands manifest not found at " << cmd_path.string() << "\n";
        return 1;
    }

    // Parse the YAML to find the matching command entry and collect its fields
    std::istringstream stream(content);
    std::string line;
    bool found = false;
    bool in_entry = false;
    bool in_subcommands = false;
    bool in_args = false;
    bool in_sub_args = false;

    std::string cmd_status, cmd_summary, cmd_docs;
    struct SubCmd { std::string name; std::string summary; };
    struct Arg { std::string name; std::string required; std::string description; std::string kind; };
    std::vector<SubCmd> subcommands;
    std::vector<Arg> top_args;

    while (std::getline(stream, line)) {
        std::string trimmed = trim(line);
        auto indent = line.find_first_not_of(' ');
        if (indent == std::string::npos) indent = 0;

        auto n = yaml_value(line, "name");

        // Top-level command entry (indent <= 4, typically "  - name: X")
        if (!n.empty() && indent <= 4) {
            if (in_entry) break;  // We already found our match, stop at next top-level entry
            if (n == name) {
                found = true;
                in_entry = true;
            }
            continue;
        }

        if (!in_entry) continue;

        // Detect section transitions within the entry
        if (trimmed.find("subcommands:") == 0) {
            in_subcommands = true;
            in_args = false;
            in_sub_args = false;
            continue;
        }
        if (trimmed.find("args:") == 0 && !in_subcommands) {
            in_args = true;
            in_subcommands = false;
            in_sub_args = false;
            continue;
        }

        // Inside subcommands list
        if (in_subcommands) {
            // Sub-args within a subcommand -- skip arg entries for display
            if (trimmed.find("args:") == 0) {
                in_sub_args = true;
                continue;
            }
            if (in_sub_args) {
                // A new subcommand entry (- name: at subcommand indent) ends sub-args
                if (!n.empty() && indent <= 6) {
                    in_sub_args = false;
                } else {
                    continue;  // Skip arg detail lines
                }
            }
            if (!n.empty()) {
                subcommands.push_back({n, {}});
                continue;
            }
            auto s = yaml_value(line, "summary");
            if (!s.empty() && !subcommands.empty()) {
                subcommands.back().summary = s;
                continue;
            }
        }

        // Inside top-level args list
        if (in_args) {
            if (!n.empty()) {
                top_args.push_back({n, {}, {}, {}});
                continue;
            }
            auto r = yaml_value(line, "required");
            if (!r.empty() && !top_args.empty()) { top_args.back().required = r; continue; }
            auto d = yaml_value(line, "description");
            if (!d.empty() && !top_args.empty()) { top_args.back().description = d; continue; }
            auto k = yaml_value(line, "kind");
            if (!k.empty() && !top_args.empty()) { top_args.back().kind = k; continue; }
        }

        // Top-level scalar fields
        if (!in_subcommands && !in_args) {
            auto st = yaml_value(line, "status");
            if (!st.empty()) { cmd_status = st; continue; }
            auto su = yaml_value(line, "summary");
            if (!su.empty()) { cmd_summary = su; continue; }
            auto dc = yaml_value(line, "docs");
            if (!dc.empty()) { cmd_docs = dc; continue; }
        }
    }

    if (!found) {
        std::cerr << "No command found for \"" << name << "\"\n";
        std::cerr << "Check docs/status/cli-commands.yaml for available commands.\n";
        return 1;
    }

    // Render the output
    std::cout << "Command: " << name << "\n";
    if (!cmd_status.empty())  std::cout << "  Status:  " << cmd_status << "\n";
    if (!cmd_summary.empty()) std::cout << "  Summary: " << cmd_summary << "\n";

    if (!top_args.empty()) {
        std::cout << "\n  Arguments:\n";
        for (auto& a : top_args) {
            std::cout << "    " << a.name;
            if (!a.kind.empty()) std::cout << " (" << a.kind << ")";
            if (!a.description.empty()) std::cout << " — " << a.description;
            std::cout << "\n";
        }
    }

    if (!subcommands.empty()) {
        std::cout << "\n  Subcommands:\n";
        for (auto& sc : subcommands) {
            std::cout << "    " << sc.name;
            if (!sc.summary.empty()) std::cout << " — " << sc.summary;
            std::cout << "\n";
        }
    }

    std::cout << "\nSee also: docs/reference/cli.md\n";
    return 0;
}

static int docs_show_cmake(const fs::path& docs_dir, const std::string& name) {
    if (name.empty()) {
        std::cerr << "Usage: pulp docs show cmake <name>\n";
        return 1;
    }

    auto cmake_path = docs_dir / "status" / "cmake-functions.yaml";
    std::string content = read_file_contents(cmake_path);
    if (content.empty()) {
        std::cerr << "Error: CMake functions manifest not found at " << cmake_path.string() << "\n";
        return 1;
    }

    std::istringstream stream(content);
    std::string line;
    bool found = false;
    bool in_entry = false;

    while (std::getline(stream, line)) {
        auto n = yaml_value(line, "name");
        if (!n.empty()) {
            if (in_entry) break;
            if (n == name) {
                found = true;
                in_entry = true;
                std::cout << "CMake function: " << n << "\n";
            }
            continue;
        }
        if (in_entry) {
            std::string trimmed = trim(line);
            if (trimmed.empty() || trimmed == "-") continue;
            std::cout << "  " << trimmed << "\n";
        }
    }

    if (!found) {
        std::cerr << "No CMake function found for \"" << name << "\"\n";
        std::cerr << "Check docs/status/cmake-functions.yaml for available functions.\n";
        return 1;
    }

    std::cout << "\nSee also: docs/reference/cmake.md\n";
    return 0;
}

static int docs_show_style(const fs::path& docs_dir) {
    auto style_path = docs_dir / "status" / "style-rules.yaml";
    std::string content = read_file_contents(style_path);
    if (content.empty()) {
        std::cerr << "Error: style rules not found at " << style_path.string() << "\n";
        return 1;
    }

    std::cout << "Style Rules\n";
    std::cout << "===========\n\n";

    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty()) continue;
        // Print rule entries readably
        auto id = yaml_value(line, "id");
        auto rule = yaml_value(line, "rule");
        auto severity = yaml_value(line, "severity");
        if (!id.empty()) {
            std::cout << "\n[" << id << "]\n";
        } else if (!rule.empty()) {
            std::cout << "  Rule: " << rule << "\n";
        } else if (!severity.empty()) {
            std::cout << "  Severity: " << severity << "\n";
        } else {
            // Print other key-value pairs
            auto colon = trimmed.find(':');
            if (colon != std::string::npos && trimmed.front() != '-' && trimmed.front() != '#') {
                auto key = trim(trimmed.substr(0, colon));
                auto val = trim(trimmed.substr(colon + 1));
                if (!key.empty() && !val.empty()) {
                    std::cout << "  " << key << ": " << val << "\n";
                }
            }
        }
    }

    std::cout << "\nFull details:\n";
    std::cout << "  docs/policies/code-style.md\n";
    std::cout << "  docs/policies/agent-contribution-rules.md\n";
    return 0;
}

static int cmd_docs(const std::vector<std::string>& args) {
    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto docs_dir = root / "docs";

    if (args.empty()) {
        std::cout << "pulp docs — local documentation reader\n\n";
        std::cout << "Subcommands:\n";
        std::cout << "  index                    List available docs\n";
        std::cout << "  search <query>           Search docs for a string\n";
        std::cout << "  open <slug>              Print a doc by slug\n";
        std::cout << "  show support <thing>     Look up support status\n";
        std::cout << "  show command <name>      Look up a CLI command\n";
        std::cout << "  show cmake <name>        Look up a CMake function\n";
        std::cout << "  show style               Show code style rules\n";
        std::cout << "  check                    Validate docs consistency\n";
        std::cout << "  build-site               Generate static docs site\n";
        std::cout << "  build-api                Generate API reference (Doxygen)\n";
        return 0;
    }

    std::string sub = args[0];

    if (sub == "build-site") {
        auto script = root / "tools" / "build-docs.py";
        if (!fs::exists(script)) {
            std::cerr << "Error: build script not found at " << script.string() << "\n";
            return 1;
        }
        std::string cmd = "python3 \"" + script.string() + "\"";
        // Pass through extra args
        for (size_t i = 1; i < args.size(); ++i) {
            cmd += " " + args[i];
        }
        return run(cmd);
    }

    if (sub == "build-api") {
        auto script = root / "tools" / "build-api-docs.sh";
        if (!fs::exists(script)) {
            std::cerr << "Error: build script not found at " << script.string() << "\n";
            return 1;
        }
        return run("bash \"" + script.string() + "\"");
    }

    if (sub == "check") {
        // Run the docs consistency check script
        auto script = root / "tools" / "check-docs.sh";
        if (!fs::exists(script)) {
            std::cerr << "Error: check script not found at " << script.string() << "\n";
            return 1;
        }
        return run("bash \"" + script.string() + "\"");
    }

    if (sub == "index") {
        return docs_index(docs_dir);
    }

    if (sub == "search") {
        std::string query;
        for (size_t i = 1; i < args.size(); ++i) {
            if (!query.empty()) query += " ";
            query += args[i];
        }
        return docs_search(docs_dir, query);
    }

    if (sub == "open") {
        if (args.size() < 2) {
            std::cerr << "Usage: pulp docs open <slug>\n";
            return 1;
        }
        return docs_open(docs_dir, args[1]);
    }

    if (sub == "show") {
        if (args.size() < 2) {
            std::cerr << "Usage: pulp docs show <support|command|cmake|style> [name]\n";
            return 1;
        }
        std::string show_sub = args[1];

        if (show_sub == "support") {
            if (args.size() < 3) {
                std::cerr << "Usage: pulp docs show support <thing>\n";
                return 1;
            }
            return docs_show_support(docs_dir, args[2]);
        }
        if (show_sub == "command") {
            if (args.size() < 3) {
                std::cerr << "Usage: pulp docs show command <name>\n";
                return 1;
            }
            return docs_show_command(docs_dir, args[2]);
        }
        if (show_sub == "cmake") {
            if (args.size() < 3) {
                std::cerr << "Usage: pulp docs show cmake <name>\n";
                return 1;
            }
            return docs_show_cmake(docs_dir, args[2]);
        }
        if (show_sub == "style") {
            return docs_show_style(docs_dir);
        }

        std::cerr << "Unknown show topic: " << show_sub << "\n";
        std::cerr << "Available: support, command, cmake, style\n";
        return 1;
    }

    std::cerr << "Unknown docs subcommand: " << sub << "\n";
    std::cerr << "Run `pulp docs` for usage.\n";
    return 1;
}

static void print_usage() {
    std::cout << "pulp — Pulp audio plugin framework CLI\n\n";
    std::cout << "Usage: pulp <command> [options]\n\n";
    std::cout << "Commands:\n";
    std::cout << "  create   Create a new plugin project (scaffold + build + test)\n";
    std::cout << "  build    Build the project (configure + compile)\n";
    std::cout << "  run      Launch a standalone Pulp application\n";
    std::cout << "  test     Run the test suite\n";
    std::cout << "  status   Show project status and info\n";
    std::cout << "  validate Run plugin format validators (CLAP, VST3, AU, optional AAX)\n";
    std::cout << "  ship     Sign, package, and check plugins\n";
    std::cout << "  install  Download and install the Pulp SDK (--version x.y.z)\n";
    std::cout << "  cache    Manage SDK and asset cache (~/.pulp/)\n";
    std::cout << "  audio    Repo-level audio model and bundle tooling\n";
    std::cout << "  docs     Browse local documentation\n";
    std::cout << "  doctor   Diagnose environment issues (--fix, --ci, --dry-run)\n";
    std::cout << "  ci-local Run local-first CI across this Mac and configured hosts\n";
    std::cout << "  upgrade  Update the Pulp CLI to the latest version\n";
    std::cout << "  clean    Remove build directory\n";
    std::cout << "  inspect  Launch the component inspector\n";
    std::cout << "  design          AI-powered style design (natural language -> token diffs)\n";
    std::cout << "  design-debug    Headless before/after/diff runner for design chat prompts\n";
    std::cout << "  import-design   Import designs from Figma/Stitch/v0/Pencil\n";
    std::cout << "  export-tokens   Export theme as W3C Design Tokens\n";
    std::cout << "  audit           License and audit\n";
    std::cout << "  help     Show this help\n";
    std::cout << "\nExamples:\n";
    std::cout << "  pulp create MyPlugin              # Create a new effect plugin\n";
    std::cout << "  pulp create MySynth --type instrument  # Create an instrument\n";
    std::cout << "  pulp create DebugKnob --in-tree   # Add an example under examples/\n";
    std::cout << "  pulp doctor             # Check environment for issues\n";
    std::cout << "  pulp doctor --fix       # Auto-fix issues where possible\n";
    std::cout << "  pulp build              # Build all targets\n";
    std::cout << "  pulp build --target X   # Build specific target\n";
    std::cout << "  pulp test               # Run all tests\n";
    std::cout << "  pulp test -R Knob       # Run tests matching 'Knob'\n";
    std::cout << "  pulp validate           # Validate built plugins\n";
    std::cout << "  pulp cache status       # Show cached SDKs and assets\n";
    std::cout << "  pulp audio model list   # Show registered audio models and install state\n";
    std::cout << "  pulp audio model status # Show configured audio model state\n";
    std::cout << "  pulp audio model activate <model-id>\n";
    std::cout << "  pulp audio read-bundle path/to/bundle --json\n";
    std::cout << "  pulp cache fetch skia   # Download Skia GPU binaries\n";
    std::cout << "  pulp docs index         # List available docs\n";
    std::cout << "  pulp status             # Show project info\n";
    std::cout << "  pulp ci-local cloud workflows\n";
    std::cout << "  pulp ci-local cloud run build feature/my-branch\n";
    std::cout << "  pulp design             # Build and launch the design tool\n";
    std::cout << "  pulp design --script path/to/design-tool.js\n";
    std::cout << "  pulp design --build-dir /tmp/pulp-design-parity-build\n";
}

static void print_audio_usage() {
    std::cout << "pulp audio — repo-level audio analysis tooling\n\n";
    std::cout << "Usage:\n";
    std::cout << "  pulp audio model list [--json]\n";
    std::cout << "  pulp audio model status [--json]\n";
    std::cout << "  pulp audio model activate <model-id> [--json]\n";
    std::cout << "  pulp audio excerpt-find --text <query> --input <path> [options]\n";
    std::cout << "  pulp audio read-bundle <path> [--json]\n";
}

static int cmd_audio(const std::vector<std::string>& args) {
    if (args.empty()) {
        print_audio_usage();
        return 0;
    }

    if (args[0] == "model") {
        if (args.size() < 2) {
            std::cerr << "Unknown audio model subcommand.\n";
            print_audio_usage();
            return 1;
        }

        if (args[1] == "list") {
            bool json_output = false;
            for (std::size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--json") json_output = true;
                else {
                    std::cerr << "Unknown option: " << args[i] << "\n";
                    return 1;
                }
            }

            auto result = pulp::tools::audio::list_models();
            if (json_output) {
                std::cout << pulp::tools::audio::to_json(result) << "\n";
                return result.error.empty() ? 0 : 1;
            }

            std::cout << "Audio Models\n";
            std::cout << "============\n";
            std::cout << "Active: " << (result.active_model_id.empty() ? "(none)" : result.active_model_id) << "\n";
            for (const auto& item : result.models) {
                std::cout << (item.active ? "* " : "  ")
                          << item.model.model_id
                          << " [" << item.status << "]"
                          << " backend=" << item.model.backend;
                if (!item.model.task_tags.empty()) {
                    std::cout << " tags=";
                    for (std::size_t i = 0; i < item.model.task_tags.size(); ++i) {
                        if (i > 0) std::cout << ",";
                        std::cout << item.model.task_tags[i];
                    }
                }
                std::cout << "\n";
            }
            if (!result.error.empty()) {
                std::cerr << "Error: " << result.error << "\n";
                return 1;
            }
            return 0;
        }

        if (args[1] == "activate") {
            if (args.size() < 3) {
                std::cerr << "Error: model id is required.\n";
                print_audio_usage();
                return 1;
            }

            std::string model_id;
            bool json_output = false;
            for (std::size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--json") json_output = true;
                else if (model_id.empty()) model_id = args[i];
                else {
                    std::cerr << "Unknown argument: " << args[i] << "\n";
                    return 1;
                }
            }

            auto result = pulp::tools::audio::activate_model(model_id);
            if (json_output) {
                std::cout << pulp::tools::audio::to_json(result) << "\n";
                return result.ok ? 0 : 1;
            }

            if (!result.ok) {
                std::cerr << "Error: " << result.error << "\n";
                return 1;
            }

            std::cout << "Activated audio model: " << result.active_model_id << "\n";
            std::cout << "State file: " << result.state_path.string() << "\n";
            return 0;
        }

        if (args[1] != "status") {
            std::cerr << "Unknown audio model subcommand.\n";
            print_audio_usage();
            return 1;
        }

        bool json_output = false;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--json") json_output = true;
            else {
                std::cerr << "Unknown option: " << args[i] << "\n";
                return 1;
            }
        }

        auto status = pulp::tools::audio::query_model_status();
        if (json_output) {
            std::cout << pulp::tools::audio::to_json(status) << "\n";
            return 0;
        }

        std::cout << "Audio Model Status\n";
        std::cout << "==================\n";
        std::cout << "State file: " << (status.state_path.empty() ? "(unresolved)" : status.state_path.string()) << "\n";
        std::cout << "State file found: " << (status.state_file_found ? "yes" : "no") << "\n";
        std::cout << "Configured model: "
                  << (status.configured_model_id.empty() ? "(none)" : status.configured_model_id) << "\n";
        std::cout << "Backend: " << (status.backend.empty() ? "(unknown)" : status.backend) << "\n";
        std::cout << "Resolved checkpoint: "
                  << (status.resolved_checkpoint_path.empty() ? "(none)" : status.resolved_checkpoint_path.string())
                  << "\n";
        std::cout << "Loadable: " << (status.loadable() ? "yes" : "no") << "\n";
        std::cout << "Message: " << status.message << "\n";
        return 0;
    }

    if (args[0] == "excerpt-find") {
        pulp::tools::audio::ExcerptFindRequest request;
        bool json_output = false;

        for (std::size_t i = 1; i < args.size(); ++i) {
            const auto& arg = args[i];
            auto require_value = [&](const char* flag) -> const std::string* {
                if (i + 1 >= args.size()) {
                    std::cerr << "Error: " << flag << " requires a value.\n";
                    return nullptr;
                }
                return &args[++i];
            };

            if (arg == "--text") {
                auto* value = require_value("--text");
                if (!value) return 1;
                request.text = *value;
            } else if (arg == "--input") {
                auto* value = require_value("--input");
                if (!value) return 1;
                request.input_path = *value;
            } else if (arg == "--model") {
                auto* value = require_value("--model");
                if (!value) return 1;
                request.model_id = *value;
            } else if (arg == "--recursive") {
                request.recursive = true;
            } else if (arg == "--top") {
                auto* value = require_value("--top");
                if (!value) return 1;
                if (!parse_size_arg(*value, "--top", request.top_k)) return 1;
            } else if (arg == "--window-ms") {
                auto* value = require_value("--window-ms");
                if (!value) return 1;
                if (!parse_uint64_arg(*value, "--window-ms", request.window_ms)) return 1;
            } else if (arg == "--hop-ms") {
                auto* value = require_value("--hop-ms");
                if (!value) return 1;
                if (!parse_uint64_arg(*value, "--hop-ms", request.hop_ms)) return 1;
            } else if (arg == "--min-score") {
                auto* value = require_value("--min-score");
                if (!value) return 1;
                if (!parse_double_arg(*value, "--min-score", request.min_score)) return 1;
            } else if (arg == "--max-candidates-per-file") {
                auto* value = require_value("--max-candidates-per-file");
                if (!value) return 1;
                if (!parse_size_arg(*value, "--max-candidates-per-file", request.max_candidates_per_file)) return 1;
            } else if (arg == "--bundle-out") {
                auto* value = require_value("--bundle-out");
                if (!value) return 1;
                request.bundle_out = *value;
            } else if (arg == "--json") {
                json_output = true;
            } else if (arg == "--dry-run") {
                request.dry_run = true;
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                return 1;
            }
        }

        auto result = pulp::tools::audio::run_excerpt_find(request);
        if (json_output) {
            std::cout << pulp::tools::audio::to_json(result) << "\n";
            return result.ok ? 0 : 1;
        }

        if (!result.ok) {
            std::cerr << "Error: " << result.error << "\n";
            return 1;
        }

        std::cout << "Audio Excerpt Search\n";
        std::cout << "====================\n";
        if (!result.bundle_path.empty())
            std::cout << "Bundle: " << result.bundle_path.string() << "\n";
        std::cout << "Query: " << result.query << "\n";
        std::cout << "Requested model: " << result.requested_model_id << "\n";
        std::cout << "Loaded model: " << result.loaded_model_id << "\n";
        std::cout << "Backend: " << result.backend << " (WAV-first deterministic stub)\n";
        std::cout << "Scanned files: " << result.scanned_file_count << "\n";
        for (const auto& item : result.results) {
            std::cout << "  #" << item.rank
                      << " score=" << std::fixed << std::setprecision(4) << item.score
                      << " source=" << item.source_file
                      << " [" << item.start_ms << "ms, " << item.end_ms << "ms]"
                      << "\n";
        }
        return 0;
    }

    if (args[0] == "read-bundle") {
        if (args.size() < 2) {
            std::cerr << "Error: bundle path is required.\n";
            print_audio_usage();
            return 1;
        }

        fs::path bundle_path;
        bool json_output = false;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--json") {
                json_output = true;
                continue;
            }
            if (bundle_path.empty()) {
                bundle_path = args[i];
                continue;
            }
            std::cerr << "Unknown argument: " << args[i] << "\n";
            return 1;
        }

        auto bundle = pulp::tools::audio::read_excerpt_bundle(bundle_path);
        if (json_output) {
            std::cout << pulp::tools::audio::to_json(bundle) << "\n";
            return bundle.ok ? 0 : 1;
        }

        if (!bundle.ok) {
            std::cerr << "Error: " << bundle.error << "\n";
            return 1;
        }

        std::cout << "Audio Excerpt Bundle\n";
        std::cout << "====================\n";
        std::cout << "Bundle: " << bundle.bundle_path.string() << "\n";
        std::cout << "Tool: " << (bundle.tool.empty() ? "(unknown)" : bundle.tool) << "\n";
        std::cout << "Requested model: "
                  << (bundle.requested_model_id.empty() ? "(unknown)" : bundle.requested_model_id) << "\n";
        std::cout << "Loaded model: "
                  << (bundle.loaded_model_id.empty() ? "(unknown)" : bundle.loaded_model_id) << "\n";
        std::cout << "Backend: " << (bundle.backend.empty() ? "(unknown)" : bundle.backend) << "\n";
        std::cout << "Results: " << bundle.result_count << "\n";
        for (const auto& item : bundle.results) {
            std::cout << "  #" << item.rank
                      << " score=" << std::fixed << std::setprecision(4) << item.score
                      << " source=" << item.source_file
                      << " [" << item.start_ms << "ms, " << item.end_ms << "ms]"
                      << "\n";
        }
        return 0;
    }

    std::cerr << "Unknown audio subcommand.\n";
    print_audio_usage();
    return 1;
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Check for --no-color before anything else
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-color") == 0) {
            g_no_color = true;
        }
    }
    init_color();

    if (argc < 2) {
        print_usage();
        return 0;
    }

    std::string command = argv[1];
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-color") == 0) continue;  // already handled
        args.push_back(argv[i]);
    }

    if (command == "build")    return cmd_build(args);
    if (command == "run")      return cmd_run(args);
    if (command == "test")     return cmd_test(args);
    if (command == "status")   return cmd_status(args);
    if (command == "audio")    return cmd_audio(args);
    if (command == "validate") return cmd_validate(args);
    if (command == "doctor")   return cmd_doctor(args);
    if (command == "upgrade")  return cmd_upgrade(args);
    if (command == "ship")     return cmd_ship(args);
    if (command == "docs")     return cmd_docs(args);
    if (command == "clean")    return cmd_clean(args);
    if (command == "add")      return pulp::cli::pkg::cmd_add(args);
    if (command == "remove")   return pulp::cli::pkg::cmd_remove(args);
    if (command == "list")     return pulp::cli::pkg::cmd_list(args);
    if (command == "search")   return pulp::cli::pkg::cmd_search(args);
    if (command == "update")   return pulp::cli::pkg::cmd_update(args);
    if (command == "suggest")  return pulp::cli::pkg::cmd_suggest(args);
    if (command == "target")   return pulp::cli::pkg::cmd_target(args);
    if (command == "tool")     return pulp::cli::tools::cmd_tool(args);
    if (command == "add-component") {
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        auto script = root / "tools" / "add-component.py";
        if (!fs::exists(script)) {
            std::cerr << "Error: add-component script not found\n";
            return 1;
        }
        std::string cmd = "python3 \"" + script.string() + "\"";
        for (auto& arg : args) cmd += " \"" + arg + "\"";
        return run(cmd);
    }
    if (command == "audit") {
        // Check for package-manager-specific flags
        bool pkg_flag = false, plat_flag = false, lic_flag = false;
        for (auto& a : args) {
            if (a == "--packages") pkg_flag = true;
            if (a == "--platforms") plat_flag = true;
            if (a == "--licenses") lic_flag = true;
        }
        if (pkg_flag || plat_flag || lic_flag) {
            auto root = find_project_root();
            if (root.empty()) {
                std::cerr << "Error: not in a Pulp project directory\n";
                return 1;
            }
            int rc = 0;
            if (pkg_flag) rc |= pulp::cli::pkg::audit_packages(root);
            if (plat_flag) rc |= pulp::cli::pkg::audit_platforms(root);
            if (lic_flag) rc |= pulp::cli::pkg::audit_licenses(root);
            return rc;
        }
        // Fall through to existing Python audit
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        auto script = root / "tools" / "audit.py";
        if (!fs::exists(script)) {
            std::cerr << "Error: audit script not found at " << script.string() << "\n";
            return 1;
        }
        std::string cmd = "python3 \"" + script.string() + "\"";
        for (auto& arg : args) cmd += " \"" + arg + "\"";
        return run(cmd);
    }
    if (command == "ci-local") {
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        auto script = root / "tools" / "local-ci" / "local_ci.py";
        if (!fs::exists(script)) {
            std::cerr << "Error: local-ci script not found at " << script.string() << "\n";
            return 1;
        }
        std::string cmd = "python3 \"" + script.string() + "\"";
        for (auto& arg : args) cmd += " \"" + arg + "\"";
        return run(cmd);
    }
    if (command == "design")   return cmd_design(args);
    if (command == "design-debug") {
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        auto debug_bin = root / "build" / "tools" / "design" / "pulp-design-debug";
        if (!fs::exists(debug_bin)) {
            std::cerr << "Error: pulp-design-debug not built. Run `pulp build` first.\n";
            return 1;
        }
        std::string cmd = debug_bin.string();
        for (auto& arg : args) cmd += " \"" + arg + "\"";
        return run(cmd);
    }
    if (command == "inspect") return cmd_inspect(args);
    if (command == "import-design" || command == "export-tokens") {
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        auto import_bin = root / "build" / "tools" / "import-design" / "pulp-import-design";
        if (!fs::exists(import_bin)) {
            std::cerr << "Error: pulp-import-design not built. Run `pulp build` first.\n";
            return 1;
        }
        std::string cmd = import_bin.string();
        if (command == "export-tokens") cmd += " --export-tokens";
        for (auto& arg : args) cmd += " \"" + arg + "\"";
        return run(cmd);
    }
    if (command == "cache")    return cmd_cache(args);
    if (command == "sdk")      return cmd_sdk(args);
    if (command == "install") {
        // pulp install — download the SDK via the cache subsystem
        std::cout << "Installing Pulp SDK...\n";
        std::vector<std::string> cache_args = {"fetch", "skia"};
        return cmd_cache(cache_args);
    }
    if (command == "create") return cmd_create(args);
    if (command == "help" || command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    std::cerr << "Run `pulp help` for usage\n";
    return 1;
}
