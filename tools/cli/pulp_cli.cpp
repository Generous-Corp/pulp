// pulp — CLI tool for the Pulp audio plugin framework
// Wraps common build/test/status operations

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>  // _NSGetExecutablePath
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>  // GetModuleFileNameA, MAX_PATH
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

static const char* PULP_SDK_VERSION = "0.1.0";
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
    if (std::getenv("NO_COLOR")) { g_color_enabled = false; return; }
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
static void print_step(const std::string& msg) {
    std::cout << "\n" << color::bold() << color::cyan() << "\xe2\x94\x80\xe2\x94\x80 " << msg << color::reset() << "\n";
}

// ── Helpers ──────────────────────────────────────────────────────────────────

static int run(const std::string& cmd) {
    return std::system(cmd.c_str());
}

// Run a shell command with an animated spinner and elapsed time.
// Output is captured to a temp file and shown only on failure.
static int run_with_spinner(const std::string& cmd, const std::string& label) {
    if (!is_tty() || g_no_color) {
        std::cout << label << "...\n";
        return run(cmd);
    }

    // Redirect output to temp file so it doesn't interleave with spinner
    std::string tmp = "/tmp/pulp-spinner-" + std::to_string(getpid()) + ".log";
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
        std::string tail_cmd = "tail -20 " + tmp;
        run(tail_cmd);
    }

    std::remove(tmp.c_str());
    return result;
}

static std::string exec_output(const std::string& cmd) {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
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

static fs::path find_project_root() {
    auto dir = fs::current_path();
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
    const char* pulp_home_env = std::getenv("PULP_HOME");
    if (pulp_home_env && *pulp_home_env) {
        return fs::path(pulp_home_env);
    }

    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home) home = std::getenv("USERPROFILE");
#endif
    if (!home) return {};
    return fs::path(home) / ".pulp";
}

static fs::path sdk_cache_path(const std::string& version) {
    return pulp_home() / "sdk" / version;
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
    tmp_dir = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "\\pulp-sdk-download-" + version;
#endif

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
        run("rm -rf " + tmp_dir);
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
        run("rm -rf " + tmp_dir);
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
        run("rm -rf " + tmp_dir);
        return {};
    }

    run("rm -rf " + tmp_dir);

    if (!fs::exists(sdk_dir / "version.txt")) {
        std::cerr << "Error: SDK installation incomplete — version.txt not found.\n";
        fs::remove_all(sdk_dir);
        return {};
    }

    print_ok("SDK v" + version + " cached at " + sdk_dir.string());
    return sdk_dir;
}

static int ensure_checkout_dependencies(const fs::path& repo_root) {
    auto script = repo_root / "setup.sh";
    if (!fs::exists(script)) {
        std::cerr << "Error: setup.sh not found in checkout at " << repo_root.string() << "\n";
        return 1;
    }

    std::string cmd = "cd " + repo_root.string() + " && ./setup.sh --deps-only";
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
    return "VST3 AU CLAP Standalone";
#elif defined(_WIN32)
    return "VST3 CLAP Standalone";
#else
    return "VST3 CLAP LV2 Standalone";
#endif
}

static bool checkout_supports_vst3(const fs::path& repo_root) {
    return fs::exists(repo_root / "external" / "vst3sdk" / "pluginterfaces");
}

static bool checkout_supports_au(const fs::path& repo_root) {
#ifdef __APPLE__
    return fs::exists(repo_root / "external" / "AudioUnitSDK");
#else
    (void) repo_root;
    return false;
#endif
}

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
    const char* home_env = std::getenv("HOME");
#ifdef _WIN32
    if (!home_env) home_env = std::getenv("USERPROFILE");
#endif
    fs::path user_home = home_env ? fs::path(home_env) : fs::path{};

    if (const char* env_projects_dir = std::getenv("PULP_PROJECTS_DIR")) {
        auto configured = strip_quotes(trim(env_projects_dir));
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

    if (needs_configure) {
        std::string configure_cmd = "cmake -B " + build_dir.string() + " -S " + project_root.string();

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

        int rc = run_with_spinner(configure_cmd, "Configuring");
        if (rc != 0) return rc;
    }

    std::string build_cmd = "cmake --build " + build_dir.string();

    // Pass through extra args (e.g., --target, -j)
    for (auto& arg : args) {
        build_cmd += " " + arg;
    }

    return run_with_spinner(build_cmd, "Building");
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
        } else if (auto sdk_dir = local_sdk_cache_path(version); fs::exists(sdk_dir)) {
            std::cout << "SDK local cache: " << sdk_dir.string() << "\n";
        } else if (auto sdk_dir = sdk_cache_path(version); fs::exists(sdk_dir)) {
            std::cout << "SDK download cache: " << sdk_dir.string() << "\n";
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

    int total = 0, passed = 0, failed = 0, skipped = 0;

    // CLAP validation
    auto clap_dir = build_dir / "CLAP";
    if (fs::exists(clap_dir)) {
        bool has_clap_validator = !exec_output("which clap-validator 2>/dev/null").empty();

        for (auto& entry : fs::directory_iterator(clap_dir)) {
            if (entry.path().extension() == ".clap") {
                auto name = entry.path().stem().string();
                ++total;

                if (has_clap_validator) {
                    std::cout << "CLAP: validating " << name << "... ";
                    auto clap_path = entry.path().string();
                    int rc = run("clap-validator validate \"" + clap_path + "\" 2>/dev/null");
                    if (rc == 0) {
                        std::cout << "PASSED\n";
                        ++passed;
                    } else {
                        std::cout << "FAILED\n";
                        ++failed;
                    }
                } else {
                    // Fallback: dlopen test
                    std::cout << "CLAP: " << name << " (dlopen check only, clap-validator not installed)... ";
                    auto test_cmd = "ctest --test-dir " + build_dir.string() + " -R clap-dlopen-" + name + " --output-on-failure 2>/dev/null";
                    int rc = run(test_cmd);
                    if (rc == 0) {
                        std::cout << "PASSED\n";
                        ++passed;
                    } else {
                        std::cout << "FAILED\n";
                        ++failed;
                    }
                }
            }
        }
    }

    // AU validation (macOS only)
#ifdef __APPLE__
    auto au_dir = build_dir / "AU";
    if (fs::exists(au_dir)) {
        for (auto& entry : fs::directory_iterator(au_dir)) {
            if (entry.path().extension() == ".component") {
                auto name = entry.path().stem().string();
                ++total;
                std::cout << "AU: " << name << " (auval check)... ";

                // Check if auval exists
                if (!exec_output("which auval 2>/dev/null").empty()) {
                    // Run auval test from ctest
                    auto test_cmd = "ctest --test-dir " + build_dir.string() + " -R auval-" + name + " --output-on-failure 2>/dev/null";
                    int rc = run(test_cmd);
                    if (rc == 0) {
                        std::cout << "PASSED\n";
                        ++passed;
                    } else {
                        std::cout << "FAILED\n";
                        ++failed;
                    }
                } else {
                    std::cout << "SKIPPED (auval not found)\n";
                    ++skipped;
                }
            }
        }
    }
#endif

    std::cout << "\nValidation Summary: " << total << " total, "
              << passed << " passed, " << failed << " failed, "
              << skipped << " skipped\n";

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
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--version" && i + 1 < args.size())
                version = args[++i];
        }

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
                    else install_loc = std::string(getenv("HOME") ? getenv("HOME") : "~")
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
        auto ver = exec_output("clang++ --version 2>&1 | head -1");
        if (!ver.empty()) {
            c.passed = true;
            c.detail = ver;
        } else {
            c.fix = "xcode-select --install";
        }
#elif defined(_WIN32)
        auto ver = exec_output("cl 2>&1 | head -1");
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
                c.detail = vswhere + " (run from Developer Command Prompt for cl.exe)";
            } else {
                c.fix = "Install Visual Studio Build Tools 2022+:\n"
                        "    https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022\n"
                        "    Select workload: 'Desktop development with C++'\n"
                        "    Or: winget install Microsoft.VisualStudio.2022.BuildTools";
            }
        }
#else
        auto ver = exec_output("g++ --version 2>&1 | head -1");
        if (ver.empty()) ver = exec_output("clang++ --version 2>&1 | head -1");
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
        auto ver_str = exec_output("cmake --version 2>&1 | head -1");
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

    // 3. git-lfs
    {
        DoctorCheck c{"git-lfs", false, {}, {}};
        auto ver = exec_output("git lfs version 2>&1 | head -1");
        if (!ver.empty() && ver.find("git-lfs") != std::string::npos) {
            c.passed = true;
            c.detail = ver;
        } else {
#ifdef __APPLE__
            c.fix = "brew install git-lfs && git lfs install";
#elif defined(_WIN32)
            c.fix = "winget install git-lfs";
#else
            c.fix = "sudo apt install git-lfs && git lfs install";
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
        {"NAMESPACE", ns},
        {"FACTORY_NAME", factory},
        {"HEADER_NAME", header_name},
        {"TARGET_NAME", class_name},
        {"MANUFACTURER", manufacturer},
        {"MANUFACTURER_CODE", mfr_code},
        {"BUNDLE_ID", bundle_id},
        {"VERSION", "1.0.0"},
        {"PLUGIN_CODE", plugin_code},
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
        {"au_v2_entry.cpp.template", "au_v2_entry.cpp"},
        {"test.cpp.template", test_name},
    };

    for (auto& [tmpl_file, outfile] : file_map) {
        if (tmpl_file == "clap_entry.cpp.template" && formats.find("CLAP") == std::string::npos) continue;
        if (tmpl_file == "vst3_entry.cpp.template" && formats.find("VST3") == std::string::npos) continue;
        if (tmpl_file == "au_v2_entry.cpp.template" && formats.find("AU") == std::string::npos) continue;
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
        int rc = run_with_spinner(configure_cmd, "Configuring");
        if (rc != 0) {
            std::cerr << "Configure failed.\n";
            return rc;
        }

        rc = run_with_spinner("cmake --build " + (out_dir / "build").string()
                              + " --target " + class_name + "-test", "Building");
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
        int rc = run("cmake -S " + root.string() + " -B " + (root / "build").string()
                     + " -DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -5");
        if (rc != 0) {
            std::cerr << "Configure failed.\n";
            return rc;
        }

        rc = run("cmake --build " + (root / "build").string() + " --target " + class_name + "-test 2>&1 | tail -10");
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
    for (auto fmt : {"VST3", "CLAP", "AU"}) {
        auto fmt_dir = build_dir / fmt;
        if (!fs::exists(fmt_dir)) continue;
        for (auto& entry : fs::directory_iterator(fmt_dir)) {
            if (entry.path().stem().string() == class_name) {
                std::cout << "  " << fmt << ":       " << entry.path().string() << "\n";
            }
        }
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
        std::string skia_tarball = "skia-" + platform + ".tar.gz";
        auto skia_cache = cache_dir / skia_tarball;

        if (fs::exists(skia_cache)) {
            std::cout << "Skia binaries already cached at " << skia_cache.string() << "\n";
            return 0;
        }

        fs::create_directories(cache_dir);

        // Download Skia binaries from the Pulp release
        std::string url = "https://github.com/" + std::string(PULP_GITHUB_REPO)
                        + "/releases/download/v" + std::string(PULP_SDK_VERSION)
                        + "/" + skia_tarball;

        std::cout << "Downloading Skia binaries for " << platform << "...\n";

        std::string download_cmd;
#ifdef _WIN32
        download_cmd = "powershell -Command \"Invoke-WebRequest -Uri '" + url
                     + "' -OutFile '" + skia_cache.string() + "'\"";
#else
        download_cmd = "curl -fSL -o " + skia_cache.string() + " " + url;
#endif

        int rc = run_with_spinner(download_cmd, "Downloading Skia");
        if (rc != 0) {
            std::cerr << "Error: failed to download Skia binaries.\n";
            std::cerr << "  URL: " << url << "\n";
            std::cerr << "  Skia may not be available for this platform/version.\n";
            fs::remove(skia_cache);
            return 1;
        }

        print_ok("Skia binaries cached at " + skia_cache.string());
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
    std::cout << "  validate Run plugin format validators (clap-validator, auval)\n";
    std::cout << "  ship     Sign, package, and check plugins\n";
    std::cout << "  cache    Manage SDK and asset cache (~/.pulp/)\n";
    std::cout << "  docs     Browse local documentation\n";
    std::cout << "  doctor   Diagnose environment issues (--fix, --ci, --dry-run)\n";
    std::cout << "  upgrade  Update the Pulp CLI to the latest version\n";
    std::cout << "  clean    Remove build directory\n";
    std::cout << "  inspect  Launch the component inspector\n";
    std::cout << "  design          AI-powered style design (natural language -> token diffs)\n";
    std::cout << "  design-debug    Headless before/after/diff runner for design chat prompts\n";
    std::cout << "  import-design   Import designs from Figma/Stitch/v0/Pencil\n";
    std::cout << "  export-tokens   Export theme as W3C Design Tokens\n";
    std::cout << "  audit           License and clean-room audit\n";
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
    std::cout << "  pulp cache fetch skia   # Download Skia GPU binaries\n";
    std::cout << "  pulp docs index         # List available docs\n";
    std::cout << "  pulp status             # Show project info\n";
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
    if (command == "validate") return cmd_validate(args);
    if (command == "doctor")   return cmd_doctor(args);
    if (command == "upgrade")  return cmd_upgrade(args);
    if (command == "ship")     return cmd_ship(args);
    if (command == "docs")     return cmd_docs(args);
    if (command == "clean")    return cmd_clean(args);
    if (command == "add") {
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
    if (command == "design") {
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        std::vector<fs::path> candidates = {
            root / "build" / "tools" / "design" / "pulp-design",
            root / "build" / "examples" / "design-tool" / "pulp-design-tool",
        };

        fs::path design_bin;
        for (const auto& candidate : candidates) {
            if (fs::exists(candidate)) {
                design_bin = candidate;
                break;
            }
        }

        if (design_bin.empty()) {
            std::cerr << "Error: design tool not built. Run `pulp build` first.\n";
            return 1;
        }
        std::string cmd = design_bin.string();
        for (auto& arg : args) cmd += " \"" + arg + "\"";
        return run(cmd);
    }
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
    if (command == "inspect") {
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        auto screenshot_bin = root / "build" / "tools" / "screenshot" / "pulp-screenshot";
        if (!fs::exists(screenshot_bin)) {
            std::cerr << "Error: pulp-screenshot not built. Run `pulp build` first.\n";
            return 1;
        }
        std::string cmd = screenshot_bin.string() + " --demo";
        for (auto& arg : args) cmd += " " + arg;
        return run(cmd);
    }
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
    if (command == "create") return cmd_create(args);
    if (command == "help" || command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    std::cerr << "Run `pulp help` for usage\n";
    return 1;
}
