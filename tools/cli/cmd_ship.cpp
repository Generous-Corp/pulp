// cmd_ship.cpp — pulp ship command

#include "cli_common.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

#include <pulp/ship/installer.hpp>

int cmd_ship(const std::vector<std::string>& args) {
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

        // Read version from CMakeLists.txt project(VERSION), falling back to SDK version
        auto cmake_ver = read_project_cmake_version(root);
        std::string version = cmake_ver.empty() ? std::string(PULP_SDK_VERSION) : cmake_ver;
        bool per_user = false;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--version" && i + 1 < args.size())
                version = args[++i];
            if (args[i] == "--per-user")
                per_user = true;
        }

#ifdef _WIN32
        // Windows: use NSIS installer
        if (std::system("where makensis >nul 2>&1") != 0) {
            std::cerr << "Error: makensis not found on PATH\n";
            std::cerr << "  Install NSIS from https://nsis.sourceforge.io/\n";
            std::cerr << "  Then add its directory to PATH\n";
            return 1;
        }

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
