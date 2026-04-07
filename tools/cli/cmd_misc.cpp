// cmd_misc.cpp — pulp test, status, clean, cache, upgrade commands

#include "cli_common.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef _WIN32
#include <pulp/platform/win32_sane.hpp>
#endif

// ── cmd_test ────────────────────────────────────────────────────────────────

int cmd_test(const std::vector<std::string>& args) {
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

// ── cmd_status ──────────────────────────────────────────────────────────────

int cmd_status([[maybe_unused]] const std::vector<std::string>& args) {
    bool standalone_mode = false;
    auto root = resolve_active_project_root(&standalone_mode);
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

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

    auto branch = exec_output("git -C " + root.string() + " branch --show-current 2>/dev/null");
    auto commit = exec_output("git -C " + root.string() + " log --oneline -1 2>/dev/null");
    if (!branch.empty()) std::cout << "Branch: " << branch << "\n";
    if (!commit.empty()) std::cout << "Commit: " << commit << "\n";

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

    int example_count = 0;
    if (fs::exists(root / "examples")) {
        for (auto& entry : fs::directory_iterator(root / "examples")) {
            if (entry.is_directory()) ++example_count;
        }
    }
    std::cout << "Examples: " << example_count << "\n";

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

// ── cmd_clean ───────────────────────────────────────────────────────────────

int cmd_clean([[maybe_unused]] const std::vector<std::string>& args) {
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

// ── cmd_cache ───────────────────────────────────────────────────────────────

int cmd_cache(const std::vector<std::string>& args) {
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

// ── cmd_upgrade ─────────────────────────────────────────────────────────────

int cmd_upgrade(const std::vector<std::string>& args) {
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

    std::cout << "Checking for updates...\n";

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

    std::string self_path = current_executable_path().string();

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

    std::string new_binary = tmp_dir + "/pulp";
    if (!fs::exists(new_binary)) {
        std::cerr << "Error: extracted archive does not contain 'pulp' binary.\n";
        run("rm -rf " + tmp_dir);
        return 1;
    }

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
