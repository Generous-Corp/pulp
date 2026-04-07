// cmd_sdk.cpp — pulp sdk command

#include "cli_common.hpp"

#include <iostream>

int cmd_sdk(const std::vector<std::string>& args) {
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
