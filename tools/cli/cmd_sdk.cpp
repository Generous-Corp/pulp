// cmd_sdk.cpp — pulp sdk command

#include "cli_common.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <regex>
#include <vector>

int cmd_sdk(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cout << "pulp sdk — manage the Pulp SDK installation\n\n";
        std::cout << "Subcommands:\n";
        std::cout << "  install [--version X.Y.Z]   Download and cache the SDK from GitHub releases\n";
        std::cout << "  install --local             Build and install the SDK from the current checkout\n";
        std::cout << "  available                   List SDK versions available on GitHub releases\n";
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
        // Pulp #2087 follow-up (#22): print a one-line banner if a
        // newer SDK is available on GitHub Releases. Cached at
        // ~/.pulp/cache/latest_release.txt with a 24h TTL — no
        // network call in the hot path most of the time.
        //
        // Codex P2 on PR #2138: only fire the banner against an
        // actually-installed SDK version. Falling back to
        // PULP_SDK_VERSION (the CLI's compile-time pin) here would
        // print contradictory output on a fresh machine: "No SDK
        // versions installed" followed by "installed: v..." against
        // the pin. Skip cleanly when nothing is installed.
        std::string installed = newest_installed_sdk();
        if (!installed.empty()) {
            maybe_print_newer_sdk_banner(installed);
        }
        return 0;
    }

    if (sub == "available") {
        // pulp #2087 follow-up (#23): list SDK versions available on
        // GitHub Releases. Shells out to `curl` and parses the JSON
        // response manually (no JSON dep needed — we only want the
        // `tag_name` values). Network failures degrade to a clear
        // message; ad-blockers / proxies are the common failure mode
        // and we don't want to mask them.
        std::string installed_pinned = PULP_SDK_VERSION;
        std::cout << "Pulp SDK — available releases\n";
        std::cout << "==============================\n\n";

        std::string url = "https://api.github.com/repos/"
                          + std::string(PULP_GITHUB_REPO)
                          + "/releases?per_page=30";
        std::string cmd = "curl -fsSL -H 'Accept: application/vnd.github+json' "
                          + shell_quote(url) + " 2>/dev/null";
        // Codex P1 on PR #2138: mirror the _WIN32 popen/pclose mapping
        // used elsewhere in tools/cli/ so this builds on the Windows
        // CLI lane. Other call sites (cmd_overflow.cpp, cmd_macos.cpp,
        // update_check.cpp) carry the same pattern.
#if defined(_WIN32)
        FILE* pipe = _popen(cmd.c_str(), "r");
#else
        FILE* pipe = popen(cmd.c_str(), "r");
#endif
        if (!pipe) {
            std::cerr << "Error: could not invoke curl.\n";
            return 1;
        }
        std::string body;
        char buf[4096];
        while (size_t n = fread(buf, 1, sizeof(buf), pipe)) {
            body.append(buf, n);
        }
#if defined(_WIN32)
        int rc = _pclose(pipe);
#else
        int rc = pclose(pipe);
#endif
        if (rc != 0 || body.empty()) {
            std::cerr << "Error: GitHub releases query failed";
            if (rc != 0) std::cerr << " (curl exit " << rc << ")";
            std::cerr << ".\n";
            std::cerr << "  Check: curl -fsSL " << url << "\n";
            return 1;
        }

        // Parse `"tag_name": "vX.Y.Z"` occurrences. Releases come
        // newest-first from the API, so the order is preserved.
        // Custom raw-string delimiter `RE` — the default `R"(...)"` form
        // closes prematurely at the first `)"` inside the regex.
        std::regex tag_re(R"RE("tag_name"\s*:\s*"v?([0-9]+\.[0-9]+\.[0-9]+)")RE");
        auto begin = std::sregex_iterator(body.begin(), body.end(), tag_re);
        auto end   = std::sregex_iterator{};
        std::vector<std::string> tags;
        for (auto it = begin; it != end; ++it) {
            tags.push_back((*it)[1].str());
        }
        if (tags.empty()) {
            std::cout << "  (no releases found)\n";
            return 0;
        }
        for (const auto& v : tags) {
            std::cout << "  v" << v;
            if (v == installed_pinned) std::cout << "  (CLI's pinned SDK version)";
            std::cout << "\n";
        }
        std::cout << "\nInstall with: pulp sdk install --version <X.Y.Z>\n";
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
