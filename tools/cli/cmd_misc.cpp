// cmd_misc.cpp — pulp test, status, clean, cache commands
//
// Note: `pulp upgrade` moved to cmd_upgrade.cpp in release-discovery
// Slice 2 (#547) so the update-check surface can grow independently.

#include "cli_common.hpp"
#include "sdk_cache_paths.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef _WIN32
#include <pulp/platform/win32_sane.hpp>
#endif

namespace {

void print_pr_workflow_status(const fs::path& root, bool source_tree_mode) {
    auto workflow = resolve_pr_workflow();
    if (!workflow.error.empty()) {
        std::cout << "PR workflow: invalid (" << workflow.source << ")\n";
        std::cout << "PR workflow detail: " << workflow.error << "\n";
        return;
    }

    if (!source_tree_mode) {
        std::cout << "PR workflow: project-owned (Pulp source ship flow not active)\n";
        return;
    }

    std::cout << "PR workflow: " << workflow.workflow << " (" << workflow.source << ")\n";

    if (workflow.workflow == "shipyard") {
        auto shipyard = find_executable_in_path("shipyard");
        auto pinned = read_pinned_shipyard_version(root);
        if (shipyard.empty()) {
            std::cout << "Shipyard: missing (run `./tools/install-shipyard.sh` in this checkout)\n";
            if (!pinned.empty()) {
                std::cout << "Shipyard pinned: " << pinned << "\n";
            }
            return;
        }

        auto actual = capture_shipyard_version(shipyard);
        std::cout << "Shipyard: " << shipyard;
        if (!actual.empty()) std::cout << " (" << actual << ")";
        if (!pinned.empty()) {
            std::cout << " pinned " << pinned;
            if (!actual.empty() && actual != pinned) std::cout << " [pin mismatch]";
        }
        std::cout << "\n";
        return;
    }

    if (workflow.workflow == "github") {
        auto gh = find_executable_in_path("gh");
        if (gh.empty()) {
            std::cout << "GitHub CLI: missing (`gh` required for github workflow)\n";
        } else {
            std::cout << "GitHub CLI: " << gh << "\n";
        }
        std::cout << "Shipyard tracking: disabled by pr.workflow=github\n";
        return;
    }

    std::cout << "PR automation: manual (no push or PR creation by `pulp pr`)\n";
    std::cout << "Shipyard tracking: disabled by pr.workflow=manual\n";
}

}  // namespace

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
        // FetchContent cache preflight (issue #744) — same gate cmd_build
        // applies. Catching it here too means `pulp test` cold-starts
        // produce the same fail-fast remediation message instead of
        // tunneling through cmd_build's stdout. cmd_build runs its own
        // preflight on the configure path, so a healthy cache makes the
        // second call a no-op.
        if (!cache_preflight_check(project_root, "pulp test")) {
            return 1;
        }
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
    print_pr_workflow_status(root, !standalone_mode);

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
        const std::string version = PULP_SDK_VERSION;

        // Versioned cache filename — `pulp-sdk-v<version>-<platform>.tar.gz`.
        // The version pin in the filename is the structural fix for pulp
        // #1814: a stale tarball from a prior release cannot silently
        // shadow a fresh download because the filename lookup misses.
        std::string sdk_tarball_name = pulp::cli::sdk_tarball_filename(version, platform);
        auto sdk_cache = cache_dir / sdk_tarball_name;

        // Best-effort cleanup of the pre-#1814 unversioned tarball so it
        // doesn't sit in ~/.pulp/cache/ forever eating ~20 MB. Skipped
        // silently on any error — this is purely housekeeping.
        {
            auto legacy_cache = cache_dir / pulp::cli::legacy_unversioned_sdk_tarball_filename(platform);
            if (fs::exists(legacy_cache)) {
                std::error_code ec;
                fs::remove(legacy_cache, ec);
                if (!ec) {
                    std::cout << "Removed legacy unversioned cache: "
                              << legacy_cache.string() << "\n";
                }
            }
        }

        if (fs::exists(sdk_cache)) {
            std::cout << "SDK v" << version << " (includes Skia) already cached at "
                      << sdk_cache.string() << "\n";
            return 0;
        }

        fs::create_directories(cache_dir);
        // GitHub release assets are uploaded with the unversioned filename;
        // the version lives in the path segment. Local filename is
        // versioned so consumers can tell from `ls ~/.pulp/cache/` which
        // SDK is actually downloaded.
        std::string remote_filename = pulp::cli::legacy_unversioned_sdk_tarball_filename(platform);
        std::string url = "https://github.com/" + std::string(PULP_GITHUB_REPO)
                        + "/releases/download/v" + version
                        + "/" + remote_filename;

        std::cout << "Downloading SDK v" << version << " (Skia binaries for "
                  << platform << ")...\n";

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

        print_ok("SDK v" + version + " cached at " + sdk_cache.string());
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
