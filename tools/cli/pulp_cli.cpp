// pulp — CLI tool for the Pulp audio plugin framework
// Wraps common build/test/status operations

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
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

// ── Commands ─────────────────────────────────────────────────────────────────

static int cmd_build(const std::vector<std::string>& args) {
    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto build_dir = root / "build";
    bool needs_configure = !fs::exists(build_dir / "CMakeCache.txt");

    // Check if CMakeLists.txt is newer than CMakeCache
    if (!needs_configure && fs::exists(build_dir / "CMakeCache.txt")) {
        auto cmake_time = fs::last_write_time(root / "CMakeLists.txt");
        auto cache_time = fs::last_write_time(build_dir / "CMakeCache.txt");
        if (cmake_time > cache_time) needs_configure = true;
    }

    if (needs_configure) {
        int rc = run_with_spinner("cmake -B " + build_dir.string() + " -S " + root.string(), "Configuring");
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
    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto build_dir = root / "build";
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
    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    // Read project info from CMakeLists.txt
    std::cout << "Pulp Project Status\n";
    std::cout << "====================\n";
    std::cout << "Root: " << root.string() << "\n";

    // Git info
    auto branch = exec_output("git -C " + root.string() + " branch --show-current");
    auto commit = exec_output("git -C " + root.string() + " log --oneline -1");
    if (!branch.empty()) std::cout << "Branch: " << branch << "\n";
    if (!commit.empty()) std::cout << "Commit: " << commit << "\n";

    // Build state
    auto build_dir = root / "build";
    if (fs::exists(build_dir / "CMakeCache.txt")) {
        std::cout << "Build: configured\n";
    } else {
        std::cout << "Build: not configured (run `pulp build`)\n";
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
    auto root = find_project_root();
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
    auto root = find_project_root();
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

static std::vector<DoctorCheck> run_doctor_checks(const fs::path& root) {
    std::vector<DoctorCheck> checks;

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
        // Try cl.exe first, then check for VS Build Tools via vswhere
        auto ver = exec_output("cl 2>&1 | head -1");
        if (!ver.empty()) {
            c.passed = true; c.detail = ver;
        } else {
            // Check if vswhere can find any VS installation
            auto vswhere = exec_output(
                "\"%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe\""
                " -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
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
        // Linux: detect distro for better fix suggestions
        auto ver = exec_output("g++ --version 2>&1 | head -1");
        if (ver.empty()) ver = exec_output("clang++ --version 2>&1 | head -1");
        if (!ver.empty()) {
            c.passed = true; c.detail = ver;
        } else {
            // Detect distro for package manager command
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
            // Extract version number
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

    // 4. git-lfs files pulled (check if Skia files are pointers)
    if (!root.empty()) {
        DoctorCheck c{"LFS files pulled", false, {}, {}};
        bool found_pointer = false;
        bool found_any = false;
        auto skia_dir = root / "external" / "skia-build";
        if (fs::exists(skia_dir)) {
            for (auto& entry : fs::recursive_directory_iterator(skia_dir)) {
                auto ext = entry.path().extension().string();
                if (ext == ".a" || ext == ".lib") {
                    found_any = true;
                    // Read first bytes to check if it's an LFS pointer
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

    // 5. VST3 SDK
    if (!root.empty()) {
        DoctorCheck c{"VST3 SDK", false, {}, {}};
        auto vst3_dir = root / "external" / "vst3sdk";
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

    // 6. AudioUnitSDK (macOS only)
#ifdef __APPLE__
    if (!root.empty()) {
        DoctorCheck c{"AudioUnitSDK", false, {}, {}};
        auto au_dir = root / "external" / "AudioUnitSDK";
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

    // 7. Linux: ALSA dev headers
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

    // 8. Build state
    if (!root.empty()) {
        DoctorCheck c{"Build configured", false, {}, {}};
        if (fs::exists(root / "build" / "CMakeCache.txt")) {
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
    auto root = find_project_root();

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
        if (root.empty()) {
            std::cout << color::dim() << "(Not in a Pulp project — checking system tools only)" << color::reset() << "\n\n";
        }
    }

    auto checks = run_doctor_checks(root);

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
    std::string result;
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    for (int i = 0; i < 16; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "0x%02X", std::rand() % 256);
        if (i > 0) result += ", ";
        result += buf;
    }
    return result;
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
        std::cerr << "Error: not in a Pulp project directory.\n";
        std::cerr << "Run this from within a Pulp checkout.\n";
        return 1;
    }

    // Parse args
    std::string name, type = "effect", manufacturer = "Pulp", output_path;
    bool no_build = false;
    bool ci_mode = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--type" && i + 1 < args.size()) { type = args[++i]; continue; }
        if (args[i] == "--manufacturer" && i + 1 < args.size()) { manufacturer = args[++i]; continue; }
        if (args[i] == "--output" && i + 1 < args.size()) { output_path = args[++i]; continue; }
        if (args[i] == "--no-build") { no_build = true; continue; }
        if (args[i] == "--no-interactive" || args[i] == "--ci") { ci_mode = true; continue; }
        if (args[i] == "--help" || args[i] == "-h") {
            std::cout << "pulp new — create a new plugin project\n\n";
            std::cout << "Usage: pulp new <name> [options]\n\n";
            std::cout << "Options:\n";
            std::cout << "  --type <effect|instrument|app|bare>  Plugin type (default: effect)\n";
            std::cout << "  --manufacturer <name>       Manufacturer (default: Pulp)\n";
            std::cout << "  --output <dir>              Output directory\n";
            std::cout << "  --no-build                  Skip build after scaffolding\n";
            std::cout << "  --no-interactive, --ci      Non-interactive mode (use defaults)\n";
            return 0;
        }
        if (name.empty() && args[i][0] != '-') { name = args[i]; continue; }
    }

    if (name.empty()) {
        std::cerr << "Usage: pulp new <name> [--type effect|instrument] [--manufacturer Name]\n";
        return 1;
    }

    if (type != "effect" && type != "instrument" && type != "app" && type != "bare") {
        std::cerr << "Error: --type must be 'effect', 'instrument', 'app', or 'bare'\n";
        return 1;
    }

    // In CI mode, suppress progress output (errors still go to stderr)
    auto log = [&](const std::string& msg) {
        if (!ci_mode) std::cout << msg;
    };

    // Quick doctor check
    log("Checking environment...\n");
    auto checks = run_doctor_checks(root);
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

    // Compute names
    std::string class_name = to_class_name(name);
    std::string lower_name = to_lower_name(name);
    std::string ns = to_namespace_name(name);
    std::string factory = ns;
    std::string plugin_code = make_plugin_code(class_name);
    std::string mfr_code = make_mfr_code(manufacturer);
    std::string bundle_id = "com." + to_namespace_name(manufacturer) + "." + ns;
    std::string header_name = replace_all_str(lower_name, "-", "_") + ".hpp";

    // Determine formats based on platform and type
    std::string formats;
    if (type == "app" || type == "bare") {
        formats = "Standalone";
    } else {
#ifdef __APPLE__
        formats = "VST3 AU CLAP Standalone";
#elif defined(_WIN32)
        formats = "VST3 CLAP Standalone";
#else
        formats = "VST3 CLAP LV2 Standalone";
#endif
    }

    // Output directory
    fs::path out_dir;
    if (!output_path.empty()) {
        out_dir = fs::path(output_path);
        if (!out_dir.is_absolute()) out_dir = fs::current_path() / out_dir;
    } else {
        out_dir = root / "examples" / lower_name;
    }

    if (fs::exists(out_dir)) {
        std::cerr << "Error: " << out_dir.string() << " already exists\n";
        return 1;
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
    };

    // Read and expand templates
    auto template_dir = root / "tools" / "templates" / type;
    if (!fs::exists(template_dir)) {
        std::cerr << "Error: template directory not found at " << template_dir.string() << "\n";
        return 1;
    }

    fs::create_directories(out_dir);
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

    for (auto& [tmpl, outfile] : file_map) {
        auto tmpl_path = template_dir / tmpl;
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
        f << "#include <pulp/format/format.hpp>\n\n";
        f << "int main(int argc, char* argv[]) {\n";
        f << "    return pulp::format::run_standalone(" << ns << "::create_" << factory << ", argc, argv);\n";
        f << "}\n";
        log("  Created main.cpp\n");
    }

    // Add to examples/CMakeLists.txt
    auto examples_cmake = root / "examples" / "CMakeLists.txt";
    auto rel_dir = lower_name;
    bool in_examples = (out_dir.parent_path() == root / "examples");

    if (in_examples && fs::exists(examples_cmake)) {
        std::string cmake_content = read_file_contents(examples_cmake);
        std::string add_line = "\n# " + name + " (generated by pulp new)\n"
                               "add_subdirectory(" + rel_dir + ")\n";
        std::ofstream f(examples_cmake, std::ios::app);
        f << add_line;
        log("  Added to examples/CMakeLists.txt\n");
    }

    log("\n");

    if (no_build) {
        log("Scaffolding complete. Run `pulp build` to build.\n");
        return 0;
    }

    // Build
    log("Building...\n");
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
    auto test_binary = root / "build" / "examples" / lower_name / (class_name + "-test");
    if (fs::exists(test_binary)) {
        rc = run(test_binary.string());
    } else {
        rc = run("ctest --test-dir " + (root / "build").string() + " -R \"" + name + "\" --output-on-failure");
    }
    if (rc != 0) {
        std::cerr << "Tests failed.\n";
        return rc;
    }

    std::string test_filter = replace_all_str(lower_name, "-", "_");

    // Success report
    std::cout << "\n  \xe2\x9c\x93 " << name << " is ready!\n\n";
    std::cout << "  Source:     " << out_dir.string() << "\n";

    auto build_dir = root / "build";
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
    std::cout << "    pulp build              # rebuild after changes\n";
    std::cout << "    pulp test -R " << test_filter << "  # run tests\n";
    std::cout << "    pulp validate           # validate plugin formats\n";
    return 0;
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

#if defined(__aarch64__) || defined(__arm64__)
    arch = "arm64";
#else
    arch = "x86_64";
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
    auto root = find_project_root();
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
            std::cout << "If no target is specified, finds the first standalone binary in build/examples/.\n";
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

    if (!target_name.empty()) {
        // Search for the named target
        auto examples_dir = build_dir / "examples";
        if (fs::exists(examples_dir)) {
            for (auto& dir_entry : fs::directory_iterator(examples_dir)) {
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
            std::cerr << "Error: could not find standalone binary '" << target_name << "' in build/examples/\n";
            std::cerr << "  Run `pulp build` to build, then try again.\n";
            return 1;
        }
    } else {
        // Find first standalone executable in build/examples/
        auto examples_dir = build_dir / "examples";
        if (fs::exists(examples_dir)) {
            for (auto& dir_entry : fs::directory_iterator(examples_dir)) {
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
            std::cerr << "Error: no standalone binary found in build/examples/\n";
            std::cerr << "  Create one with: pulp new MyApp --type app\n";
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
    std::cout << "  new      Create a new plugin project (scaffold + build + test)\n";
    std::cout << "  inspect  Launch the component inspector\n";
    std::cout << "  design   AI-powered style design (natural language -> token diffs)\n";
    std::cout << "  audit    License and clean-room audit\n";
    std::cout << "  build    Build the project (configure + compile)\n";
    std::cout << "  run      Launch a standalone Pulp application\n";
    std::cout << "  test     Run the test suite\n";
    std::cout << "  status   Show project status and info\n";
    std::cout << "  validate Run plugin format validators (clap-validator, auval)\n";
    std::cout << "  ship     Sign, package, and check plugins\n";
    std::cout << "  docs     Browse local documentation\n";
    std::cout << "  doctor   Diagnose environment issues (--fix, --ci, --dry-run)\n";
    std::cout << "  upgrade  Update the Pulp CLI to the latest version\n";
    std::cout << "  clean    Remove build directory\n";
    std::cout << "  help     Show this help\n";
    std::cout << "\nExamples:\n";
    std::cout << "  pulp new MyPlugin          # Create a new effect plugin\n";
    std::cout << "  pulp new MySynth --type instrument  # Create an instrument\n";
    std::cout << "  pulp doctor             # Check environment for issues\n";
    std::cout << "  pulp doctor --fix       # Auto-fix issues where possible\n";
    std::cout << "  pulp build              # Build all targets\n";
    std::cout << "  pulp build --target X   # Build specific target\n";
    std::cout << "  pulp test               # Run all tests\n";
    std::cout << "  pulp test -R Knob       # Run tests matching 'Knob'\n";
    std::cout << "  pulp validate           # Validate built plugins\n";
    std::cout << "  pulp ship sign --identity \"Developer ID Application: Foo\"\n";
    std::cout << "  pulp ship package --version 1.0.0\n";
    std::cout << "  pulp docs index         # List available docs\n";
    std::cout << "  pulp docs open cli      # Read a doc by slug\n";
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
    if (command == "design") {
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        auto design_bin = root / "build" / "tools" / "design" / "pulp-design";
        if (!fs::exists(design_bin)) {
            std::cerr << "Error: pulp-design not built. Run `pulp build` first.\n";
            return 1;
        }
        std::string cmd = design_bin.string();
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
    if (command == "create" || command == "new") return cmd_create(args);
    if (command == "help" || command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    std::cerr << "Run `pulp help` for usage\n";
    return 1;
}
