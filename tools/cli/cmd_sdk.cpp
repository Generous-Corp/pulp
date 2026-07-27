// cmd_sdk.cpp — pulp sdk command

#include "cli_common.hpp"
#include "local_sdk_install.hpp"
#include "local_sdk_profile.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <regex>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

class ScopedStdoutRedirect {
public:
    explicit ScopedStdoutRedirect(bool redirect) {
        if (!redirect)
            return;
        std::cout.flush();
        std::fflush(stdout);
#if defined(_WIN32)
        saved_fd_ = _dup(_fileno(stdout));
        active_ = saved_fd_ >= 0 && _dup2(_fileno(stderr), _fileno(stdout)) == 0;
#else
        saved_fd_ = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);
        active_ = saved_fd_ >= 0 && dup2(STDERR_FILENO, STDOUT_FILENO) >= 0;
#endif
        if (!active_ && saved_fd_ >= 0)
            close_saved_fd();
    }

    ~ScopedStdoutRedirect() {
        if (!active_)
            return;
        std::cout.flush();
        std::fflush(stdout);
#if defined(_WIN32)
        (void)_dup2(saved_fd_, _fileno(stdout));
#else
        (void)dup2(saved_fd_, STDOUT_FILENO);
#endif
        close_saved_fd();
    }

    bool ready() const { return active_; }

    bool write_original(const std::string& text) const {
        if (!active_)
            return false;
        std::size_t written = 0;
        while (written < text.size()) {
#if defined(_WIN32)
            const auto count =
                _write(saved_fd_, text.data() + written,
                       static_cast<unsigned int>(text.size() - written));
#else
            const auto count = write(saved_fd_, text.data() + written, text.size() - written);
#endif
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return false;
            written += static_cast<std::size_t>(count);
        }
        return true;
    }

    ScopedStdoutRedirect(const ScopedStdoutRedirect&) = delete;
    ScopedStdoutRedirect& operator=(const ScopedStdoutRedirect&) = delete;

private:
    void close_saved_fd() {
#if defined(_WIN32)
        (void)_close(saved_fd_);
#else
        (void)close(saved_fd_);
#endif
        saved_fd_ = -1;
    }

    int saved_fd_ = -1;
    bool active_ = false;
};

} // namespace

int cmd_sdk(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cout << "pulp sdk — manage the Pulp SDK installation\n\n";
        std::cout << "Subcommands:\n";
        std::cout << "  install [--version X.Y.Z]   Download and cache the SDK from GitHub releases\n";
        std::cout << "  install --local             Build and install the SDK from the current checkout\n";
        std::cout << "  install --local --profile forge-dev [--print-path]\n";
        std::cout << "                              Build an immutable arm64 Forge development SDK\n";
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
        std::vector<std::string> install_args(args.begin() + 1, args.end());
        const auto request =
            pulp::cli::local_sdk::parse_install_arguments(install_args, PULP_SDK_VERSION);
        if (!request.ok) {
            std::cerr << "pulp sdk install: " << request.error << "\n";
            return 2;
        }

        if (request.from_local) {
            auto repo_root = find_project_root();
            if (repo_root.empty()) {
                std::cerr << "Error: --local requires running from inside a Pulp checkout.\n";
                return 1;
            }
            ScopedStdoutRedirect redirect_progress(request.print_path);
            if (request.print_path && !redirect_progress.ready()) {
                std::cerr << "Error: could not reserve stdout for --print-path.\n";
                return 1;
            }
            if (!request.print_path)
                std::cout << "Building SDK from local checkout...\n";
            auto sdk = request.profile == "forge-dev"
                           ? ensure_forge_dev_sdk(repo_root)
                           : ensure_checkout_sdk(repo_root, request.version);
            if (sdk.empty()) {
                std::cerr << "SDK build failed.\n";
                return 1;
            }
            if (request.print_path) {
                if (!redirect_progress.write_original(sdk.string() + "\n")) {
                    std::cerr << "Error: could not write the SDK path to stdout.\n";
                    return 1;
                }
            } else {
                std::cout << "SDK v" << request.version << " installed at " << sdk.string() << "\n";
            }
        } else {
            std::cout << "Downloading SDK v" << request.version << "...\n";
            auto sdk = ensure_sdk(request.version);
            if (sdk.empty()) {
                std::cerr << "SDK download failed.\n";
                return 1;
            }
            std::cout << "SDK v" << request.version << " installed at " << sdk.string() << "\n";
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

        auto dev_base = home / "sdk-dev";
        if (fs::exists(dev_base)) {
            for (auto& entry : fs::recursive_directory_iterator(dev_base)) {
                if (!entry.is_regular_file() || entry.path().filename() != "sdk-provenance.json")
                    continue;
                std::cout << "  forge-dev (development-only) — "
                          << entry.path().parent_path().string() << "\n";
                found = true;
            }
        }

        if (!found) {
            std::cout << "  No SDK versions installed.\n";
            std::cout << "  Run: pulp sdk install\n";
        }
        // Print a one-line banner if a newer SDK is available on GitHub
        // Releases. Cached at ~/.pulp/cache/latest_release.txt with a 24h TTL
        // — no network call in the hot path most of the time.
        //
        // Only fire the banner against an actually-installed SDK
        // version. Falling back to
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
        // List SDK versions available on GitHub Releases. Shells out to `curl`
        // and parses the JSON response manually (no JSON dep needed — we only
        // want the `tag_name` values). Network failures degrade to a clear
        // message; ad-blockers / proxies are the common failure mode and we
        // don't want to mask them.
        std::string installed_pinned = PULP_SDK_VERSION;
        std::cout << "Pulp SDK — available releases\n";
        std::cout << "==============================\n\n";

        std::string url = "https://api.github.com/repos/"
                          + std::string(PULP_GITHUB_REPO)
                          + "/releases?per_page=30";
        std::string cmd = "curl -fsSL -H 'Accept: application/vnd.github+json' "
                          + shell_quote(url) + " 2>/dev/null";
        // Mirror the _WIN32 popen/pclose mapping used elsewhere in
        // tools/cli/ so this builds on the Windows
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
        auto dev_base = home / "sdk-dev";
        auto dev_build_base = home / "sdk-build-dev";
        auto dev_source_base = home / "sdk-source-dev";
        int removed = 0;
        for (auto* dir :
             {&sdk_base, &local_base, &build_base, &dev_base, &dev_build_base, &dev_source_base}) {
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
