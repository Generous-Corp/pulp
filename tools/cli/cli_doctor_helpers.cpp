// cli_doctor_helpers.cpp — `pulp doctor` check implementations.
//
// Extracted from cli_common.cpp in the 2026-05 Phase 2 (R2-4) batch.
// The 971-line doctor block is the single largest concern in the
// cli_common monolith; pulling it into its own TU drops cli_common
// to ~1700 lines and isolates doctor-specific edits.
//
// Public API stays declared in `cli_common.hpp` (DoctorCheck struct,
// run_doctor_checks, run_doctor_android_checks, run_doctor_ios_checks)
// — extracted code here is the implementation, not new API. Per the
// Codex R2-4 risk callout, no new public header is introduced.
//
// Phase 2 follow-up (R2-8) will refactor the bodies of run_doctor_checks
// here into a DoctorCheck registry.

#include "cli_common.hpp"
#include "package_registry.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// File-local helper, lifted from cli_common.cpp in the R2-4 extraction
// since only the doctor checks below use it.
namespace {
std::string first_line(std::string text) {
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
}  // namespace

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

        auto sdk_resolution = resolve_standalone_sdk(active_root, false);
        auto version = sdk_resolution.requested_version;
        auto sdk_hint = sdk_resolution.sdk_path_hint;
        auto checkout_hint = sdk_resolution.sdk_checkout_hint;

        DoctorCheck sdk{"Installed SDK", false, {}, {}};
        if (!sdk_hint.empty() &&
            sdk_resolution.sdk_path_version_known &&
            !sdk_resolution.sdk_path_version_matches) {
            sdk.detail = sdk_resolution.warning;
            sdk.fix = "pulp build";
        } else if (sdk_resolution.used_sdk_path_hint &&
                   sdk_resolution.sdk_path_custom_unverifiable) {
            sdk.passed = true;
            sdk.detail = sdk_hint.string() + " (custom sdk_path; version unverifiable)";
        } else if (sdk_resolution.used_sdk_path_hint) {
            sdk.passed = true;
            sdk.detail = sdk_hint.string();
        } else if (!sdk_resolution.resolved_sdk_dir.empty()) {
            sdk.passed = true;
            auto local_sdk = local_sdk_cache_path(version);
            auto downloaded_sdk = sdk_cache_path(version);
            if (sdk_resolution.resolved_sdk_dir == local_sdk) {
                sdk.detail = sdk_resolution.resolved_sdk_dir.string() + " (local cache)";
            } else if (sdk_resolution.resolved_sdk_dir == downloaded_sdk) {
                sdk.detail = sdk_resolution.resolved_sdk_dir.string() + " (download cache)";
            } else {
                sdk.detail = sdk_resolution.resolved_sdk_dir.string();
            }
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

    // Cmajor CLI check — only if project has .cmajorpatch files outside
    // examples/ and test/ (those are Pulp's own bundled patches, not the
    // developer's). Standalone projects always check.
    if (!active_root.empty()) {
        bool has_patches = false;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(active_root,
                 fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (it->is_directory()) {
                auto name = it->path().filename().string();
                // Always skip build artifacts and VCS dirs.
                // In source-tree mode, also skip examples/ and test/ (Pulp's own bundled patches).
                // In standalone mode, those are user-owned and should be scanned.
                if (name == "build" || name == "external" || name == ".git" || name == "node_modules" ||
                    (!standalone_mode && (name == "examples" || name == "test")))
                    it.disable_recursion_pending();
                continue;
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
            } else if (auto env = std::getenv("CMAJ_BIN"); env &&
                       fs::exists(env) && fs::is_regular_file(env)) {
                c.passed = true;
                c.detail = std::string(env) + " (via CMAJ_BIN)";
            } else {
                c.detail = "Project has .cmajorpatch files but cmaj is not installed";
                c.fix = "Download from https://cmajor.dev or set CMAJ_BIN=/path/to/cmaj";
            }
            checks.push_back(c);
        }
    }

    // Release-bot token check (best-effort, repo-aware).
    // The auto-release workflow falls back to GITHUB_TOKEN when
    // RELEASE_BOT_TOKEN isn't set, but tags pushed by GITHUB_TOKEN
    // don't trigger downstream workflows (GitHub anti-infinite-loop
    // safety), so the binary release pipeline silently never fires.
    // Surfacing this in doctor is the cheapest way to keep contributors
    // out of that trap. Skipped silently when:
    //   - we can't detect the GitHub repo (not a checkout, no remote)
    //   - `gh` is unavailable or unauthenticated
    //   - the user lacks `actions:read` for the repo
    // because none of those mean the user did anything wrong; the
    // existing `gh` row already reports the gh tool's health.
    {
        auto repo_slug = first_line(exec_output(
            "gh repo view --json nameWithOwner -q .nameWithOwner 2>/dev/null"));
        if (!repo_slug.empty() && repo_slug.find('/') != std::string::npos) {
            // Probe WITHOUT --jq so we can distinguish:
            //   - empty-response-from-gh (means gh errored: no auth, no
            //     actions:read, network fail) → skip the check silently.
            //   - non-empty JSON with zero secrets → repo genuinely has no
            //     secrets yet, which is exactly the bootstrap scenario this
            //     check needs to flag. The previous version used --jq
            //     '.secrets[].name' and gated on the output being non-empty,
            //     which made the bootstrap case (zero secrets) look the
            //     same as the "gh failed" case and skipped silently. Codex
            //     P1 on #149.
            auto raw = exec_output(
                "gh api 'repos/" + repo_slug + "/actions/secrets' --paginate 2>/dev/null");
            if (!raw.empty()) {
                DoctorCheck c{"RELEASE_BOT_TOKEN secret", false, {}, {}};
                // Any occurrence of "name":"RELEASE_BOT_TOKEN" across all
                // pages means it's configured. --paginate concatenates
                // page bodies so the substring check catches it whether
                // it's on page 1 or page N.
                bool present = raw.find("\"name\":\"RELEASE_BOT_TOKEN\"") != std::string::npos
                            || raw.find("\"name\": \"RELEASE_BOT_TOKEN\"") != std::string::npos;
                if (present) {
                    c.passed = true;
                    c.detail = "configured on " + repo_slug
                             + " — auto-release tags will trigger release-cli.yml + sign-and-release.yml";
                } else {
                    c.detail = "missing on " + repo_slug
                             + " — auto-release tags will fall back to GITHUB_TOKEN, "
                               "which does NOT trigger the binary release workflows";
                    c.fix =
                        "Create a fine-grained PAT and store as RELEASE_BOT_TOKEN:\n"
                        "    1. github.com -> Settings -> Developer settings -> Personal access tokens\n"
                        "       -> Fine-grained tokens -> Generate new token\n"
                        "    2. Repo access: only " + repo_slug + "\n"
                        "    3. Permission: Contents = Read and write\n"
                        "    4. github.com/" + repo_slug + "/settings/secrets/actions\n"
                        "       -> New repository secret named RELEASE_BOT_TOKEN\n"
                        "    See docs/guides/versioning.md for the full walkthrough.";
                }
                checks.push_back(c);
            }
        }
    }

    // pulp-mcp presence (#2067). Optional: the binary is only needed by
    // users running the Claude Code plugin (or another MCP client), so
    // never gate the doctor exit code on it. Just tell the user where
    // the gap is so the plugin's "/mcp" doesn't fail mysteriously.
    //
    // Probe in this order:
    //   1. `pulp-mcp` on $PATH (the steady-state after `curl install.sh`).
    //   2. `~/.pulp/bin/pulp-mcp` (the install location, in case PATH
    //      didn't get reloaded yet in this shell).
    //   3. `<repo_root>/build/tools/mcp/pulp-mcp[.exe]` (single-config
    //      source builds — Ninja, Make on Unix).
    //   4. `<repo_root>/build/tools/mcp/{Release,Debug}/pulp-mcp.exe`
    //      (multi-config Windows source builds — MSBuild). Without this
    //      contributor Windows checkouts that have NOT installed via
    //      install.ps1 would see "not found" even though the binary
    //      exists alongside their pulp.exe build.
    //
    // We require `<binary> --version` to actually run AND emit the
    // expected `pulp-mcp <semver>` prefix before marking the check
    // passed — a stale / unrunnable binary on the resolved path must
    // not lie to the user as "✓ pulp-mcp" when /mcp will still fail.
    {
        DoctorCheck c{"pulp-mcp", false, {}, {}, true};

        auto trim = [](std::string s) {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                                  s.back() == ' '  || s.back() == '\t')) {
                s.pop_back();
            }
            return s;
        };

        std::vector<fs::path> candidates;
#ifdef _WIN32
        const char* mcp_basename = "pulp-mcp.exe";
#else
        const char* mcp_basename = "pulp-mcp";
#endif

        auto path_resolved = trim(first_line(exec_output(
#ifdef _WIN32
            "where pulp-mcp 2>nul"
#else
            "command -v pulp-mcp 2>/dev/null"
#endif
        )));
        if (!path_resolved.empty() && fs::exists(path_resolved)) {
            candidates.push_back(path_resolved);
        }
        if (const char* home = std::getenv(
#ifdef _WIN32
                       "USERPROFILE"
#else
                       "HOME"
#endif
                   )) {
            fs::path installed = fs::path(home) / ".pulp" / "bin"
                               / mcp_basename;
            if (fs::exists(installed)) candidates.push_back(installed);
        }
        if (!repo_root.empty()) {
            fs::path mcp_dir = repo_root / "build" / "tools" / "mcp";
            candidates.push_back(mcp_dir / mcp_basename);
#ifdef _WIN32
            // Multi-config MSBuild emits to a config subdir; release-cli.yml
            // packages from `Release/`, so look there first.
            candidates.push_back(mcp_dir / "Release" / mcp_basename);
            candidates.push_back(mcp_dir / "Debug" / mcp_basename);
#endif
        }

        // Walk candidates in priority order; the first one that exists
        // AND produces the expected version line wins. A path that
        // exists but exec()'s to a bad version (stale binary, missing
        // dylib, no execute bit) is treated as "no match" so we don't
        // claim success on a binary that won't actually run.
        fs::path found;
        std::string version_line;
        std::string last_exec_attempt;
        for (const auto& cand : candidates) {
            if (!fs::exists(cand)) continue;
            last_exec_attempt = cand.string();
            auto ver = trim(first_line(exec_output(
                shell_quote(cand.string()) + " --version 2>&1")));
            // The expected output is `pulp-mcp <semver>`; reject empty
            // strings, error messages, and anything that doesn't match
            // the contract. `--version` is a short-circuit before the
            // JSON-RPC loop, so it should always emit cleanly if the
            // binary runs at all.
            if (ver.rfind("pulp-mcp ", 0) == 0) {
                found = cand;
                version_line = std::move(ver);
                break;
            }
        }

        if (!found.empty()) {
            c.passed = true;
            c.detail = found.string()
                     + " (" + version_line + ")"
                     + "  [CLI " + std::string(PULP_SDK_VERSION) + "]";
        } else if (!last_exec_attempt.empty()) {
            // A binary exists but won't run cleanly. Tell the user
            // exactly which path failed so they can investigate
            // (permissions, codesign, missing dylib, etc.) rather
            // than chasing a "missing" diagnostic.
            c.detail = last_exec_attempt
                     + " found but `--version` did not return the "
                       "expected `pulp-mcp <semver>` line — the binary "
                       "is likely stale, unsigned, or missing a runtime "
                       "dependency";
            c.fix = "Refresh it:\n"
                    "      curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh\n"
                    "    or rebuild in a source checkout:\n"
                    "      cmake --build build --target pulp-mcp";
        } else {
            c.detail = "not found — the Claude Code plugin's MCP server "
                       "will fail with 'cannot locate pulp-mcp binary'";
            c.fix = "Install or refresh the CLI tarball:\n"
                    "      curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh\n"
                    "    or, in a source checkout:\n"
                    "      cmake --build build --target pulp-mcp\n"
                    "    Run the CLI install BEFORE `claude plugin install pulp` so\n"
                    "    pulp-mcp is on $PATH when the plugin's launcher resolves it.";
        }
        checks.push_back(c);
    }

    return checks;
}

// ── pulp doctor android (#8 / #355) ─────────────────────────────────────────
//
// Detects: ANDROID_HOME / ANDROID_SDK_ROOT, the SDK layout under the
// per-host default if env vars aren't set, NDK, platform-tools (adb),
// emulator + at least one configured AVD, and the optional Google
// Android CLI (#355) — treated as an accelerator, NOT a hard
// requirement. Per-host install hints fall through OS detection so
// `pulp doctor android` is the single place a contributor goes to
// figure out what they're missing.

static fs::path detect_android_sdk_root() {
    if (const char* env = std::getenv("ANDROID_HOME"); env && *env) {
        if (fs::exists(env)) return env;
    }
    if (const char* env = std::getenv("ANDROID_SDK_ROOT"); env && *env) {
        if (fs::exists(env)) return env;
    }
#ifdef __APPLE__
    if (const char* home = std::getenv("HOME")) {
        fs::path mac_path = fs::path(home) / "Library" / "Android" / "sdk";
        if (fs::exists(mac_path)) return mac_path;
    }
#elif defined(__linux__)
    if (const char* home = std::getenv("HOME")) {
        fs::path linux_path = fs::path(home) / "Android" / "Sdk";
        if (fs::exists(linux_path)) return linux_path;
    }
#elif defined(_WIN32)
    if (const char* localapp = std::getenv("LOCALAPPDATA")) {
        fs::path win_path = fs::path(localapp) / "Android" / "Sdk";
        if (fs::exists(win_path)) return win_path;
    }
#endif
    return {};
}

static fs::path detect_android_cli() {
    std::string found = find_executable_in_path("android");
    if (!found.empty()) return fs::path(found);
    if (const char* home = std::getenv("HOME")) {
        fs::path local = fs::path(home) / ".android-cli" / "bin" / "android";
        if (fs::exists(local)) return local;
    }
    return {};
}

std::vector<DoctorCheck> run_doctor_android_checks() {
    std::vector<DoctorCheck> checks;
    auto sdk = detect_android_sdk_root();

    {
        DoctorCheck c{"Android SDK", false, {}, {}};
        if (!sdk.empty()) {
            c.passed = true;
            c.detail = sdk.string();
        } else {
#ifdef __APPLE__
            c.fix = "Install via Android Studio or:\n"
                    "    brew install --cask android-commandlinetools\n"
                    "    export ANDROID_HOME=$HOME/Library/Android/sdk\n"
                    "    Then sdkmanager 'platform-tools' 'platforms;android-34' 'ndk;27.0.12077973'";
#elif defined(__linux__)
            c.fix = "Install Android Studio or commandline-tools:\n"
                    "    https://developer.android.com/studio#command-line-tools-only\n"
                    "    Then export ANDROID_HOME=$HOME/Android/Sdk and add platform-tools to PATH";
#elif defined(_WIN32)
            c.fix = "Install Android Studio (preferred) or:\n"
                    "    winget install Google.AndroidStudio\n"
                    "    Then set ANDROID_HOME=%LOCALAPPDATA%\\Android\\Sdk";
#else
            c.fix = "Install Android Studio + Android SDK from https://developer.android.com/studio";
#endif
        }
        checks.push_back(c);
    }

    {
        DoctorCheck c{"Android NDK", false, {}, {}};
        if (!sdk.empty()) {
            fs::path ndk_root = sdk / "ndk";
            if (fs::exists(ndk_root)) {
                std::string versions;
                for (auto& entry : fs::directory_iterator(ndk_root)) {
                    if (entry.is_directory()) {
                        if (!versions.empty()) versions += ", ";
                        versions += entry.path().filename().string();
                    }
                }
                if (!versions.empty()) {
                    c.passed = true;
                    c.detail = versions;
                }
            }
        }
        if (!c.passed) {
            c.fix = "Install NDK r27 or newer via Android Studio's SDK Manager,"
                    " or: sdkmanager 'ndk;27.0.12077973'";
        }
        checks.push_back(c);
    }

    {
        DoctorCheck c{"adb (platform-tools)", false, {}, {}};
        std::string adb = find_executable_in_path("adb");
        if (adb.empty() && !sdk.empty()) {
            // Default Android SDK installs ship adb.exe on Windows,
            // adb on macOS / Linux — probe both so the fallback
            // doesn't miss a perfectly valid SDK. #438 P2 for #389.
            auto candidate = sdk / "platform-tools" / "adb";
            auto candidate_exe = sdk / "platform-tools" / "adb.exe";
            if (fs::exists(candidate_exe)) adb = candidate_exe.string();
            else if (fs::exists(candidate))  adb = candidate.string();
        }
        if (!adb.empty()) {
            // Shell-quote the path — SDK installs under directories
            // with spaces (e.g. Windows `Program Files`, macOS
            // `Application Support`) would otherwise split on
            // whitespace and the probe would silently fail while
            // c.passed stayed true, producing a false positive.
            // See #438 P2 Codex review on #442.
            auto detail = first_line(
                exec_output(shell_quote(adb) + " version 2>&1"));
            // If the probe failed (empty output), don't claim pass —
            // surface the missing version detail so the user can see
            // something is off even though we found the binary.
            c.passed = !detail.empty();
            c.detail = detail.empty()
                ? "found at " + adb + " but `version` probe returned no output"
                : detail;
        } else {
            c.fix = "sdkmanager 'platform-tools' or install via Android Studio.\n"
                    "    Then add $ANDROID_HOME/platform-tools to PATH.";
        }
        checks.push_back(c);
    }

    {
        DoctorCheck c{"Android emulator + AVD", false, {}, {}};
        std::string emu = find_executable_in_path("emulator");
        if (emu.empty() && !sdk.empty()) {
            auto candidate = sdk / "emulator" / "emulator";
            auto candidate_exe = sdk / "emulator" / "emulator.exe";
            if (fs::exists(candidate_exe)) emu = candidate_exe.string();
            else if (fs::exists(candidate))  emu = candidate.string();
        }
        if (!emu.empty()) {
            // Shell-quote per #438 P2 Codex review on #442 — same
            // argv-split risk as the adb fallback above. SDK paths
            // with spaces would otherwise produce a misleading
            // "No AVDs configured" failure.
            auto avds = exec_output(shell_quote(emu) + " -list-avds 2>/dev/null");
            avds.erase(0, avds.find_first_not_of(" \t\r\n"));
            if (!avds.empty()) {
                c.passed = true;
                std::string first;
                for (char ch : avds) {
                    if (ch == '\n') break;
                    first += ch;
                }
                c.detail = first.empty() ? "AVDs configured" : ("first: " + first);
            } else {
                c.fix = "No AVDs configured. Create one via Android Studio's Device Manager,"
                        " or: avdmanager create avd -n pulp_test"
                        " -k 'system-images;android-34;google_apis;arm64-v8a'";
            }
        } else {
            c.fix = "Install the emulator package: sdkmanager 'emulator'"
                    " or via Android Studio's SDK Manager.";
        }
        checks.push_back(c);
    }

    // Google Android CLI (#355) — OPTIONAL accelerator. Per Google's
    // published support matrix: macOS arm64, Linux x86_64, Windows
    // x86_64 are supported. Linux arm64, Windows arm64, and macOS
    // Intel are NOT (no published binaries). Detail-only when missing
    // or unsupported; never the cause of overall doctor failure on
    // its own.
    {
        DoctorCheck c{"Google Android CLI (optional accelerator, #355)",
                      false, {}, {}, /*optional=*/true};
        auto cli = detect_android_cli();

        // Detect host platform support. macOS we assume arm64 because
        // x86_64 macOS isn't in Google's support matrix; arch detect
        // would be more rigorous but every supported macOS host is
        // arm64 by 2026.
#if defined(__APPLE__)
        const bool platform_supported = true;
        const char* platform_label    = "macOS arm64 (supported)";
#elif defined(__linux__) && defined(__x86_64__)
        const bool platform_supported = true;
        const char* platform_label    = "Linux x86_64 (supported)";
#elif defined(__linux__) && defined(__aarch64__)
        const bool platform_supported = false;
        const char* platform_label    = "Linux arm64 (NOT supported by Google)";
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
        const bool platform_supported = true;
        const char* platform_label    = "Windows x86_64 (supported — note: `android emulator` subcommand is currently disabled on Windows; use `emulator` from $ANDROID_HOME/emulator instead)";
#elif defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
        const bool platform_supported = false;
        const char* platform_label    = "Windows arm64 (NOT supported by Google)";
#else
        const bool platform_supported = false;
        const char* platform_label    = "host arch not in Google's support matrix";
#endif

        if (!cli.empty()) {
            c.passed = true;
            c.detail = std::string(platform_label) + " — installed at " + cli.string()
                + " (Gradle stays the authoritative build path; the CLI is for"
                  " fast inner-loop iteration only — see android skill for"
                  " when to reach for it)";
        } else if (!platform_supported) {
            // Treat as PASSED (it's optional and we can't install it
            // here anyway). Detail explains why.
            c.passed = true;
            c.detail = std::string(platform_label)
                + " — Google does not publish a binary for this arch."
                  " Use Gradle (the authoritative path) on this host."
                  " Stay on a supported host (macOS arm64, Linux x86_64, Windows x86_64)"
                  " when you want CLI-accelerated iteration.";
        } else {
            c.detail = std::string(platform_label) + " — not installed (optional)";
            c.fix =
#ifdef __APPLE__
                "Install (macOS arm64 — supported):\n"
                "    mkdir -p ~/.android-cli/bin\n"
                "    curl -fsSL -o ~/.android-cli/bin/android \\\n"
                "        https://dl.google.com/android/cli/latest/darwin_arm64/android\n"
                "    chmod +x ~/.android-cli/bin/android\n"
                "    export PATH=\"$HOME/.android-cli/bin:$PATH\"\n"
                "  Then accept the ToS on first run: `android --version`.\n"
                "  See .agents/skills/android/SKILL.md for when to use it."
#elif defined(__linux__) && defined(__x86_64__)
                "Install (Linux x86_64 — supported):\n"
                "    curl -fsSL https://dl.google.com/android/cli/latest/linux_x86_64/install.sh | bash\n"
                "  See .agents/skills/android/SKILL.md for when to use it."
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
                "Install (Windows x86_64 — supported):\n"
                "    curl.exe -fsSL https://dl.google.com/android/cli/latest/windows_x86_64/install.cmd"
                " -o \"%TEMP%\\i.cmd\" && \"%TEMP%\\i.cmd\"\n"
                "  See .agents/skills/android/SKILL.md for when to use it."
#else
                "See https://developer.android.com/tools/agents for install"
                " instructions on supported platforms (macOS arm64,"
                " Linux x86_64, Windows x86_64)."
#endif
            ;
        }
        checks.push_back(c);
    }

    return checks;
}

// ── pulp doctor ios (#60 follow-up) ─────────────────────────────────────────

std::vector<DoctorCheck> run_doctor_ios_checks() {
    std::vector<DoctorCheck> checks;

#ifndef __APPLE__
    DoctorCheck c{"iOS development", false, "macOS-only",
        "iOS development requires macOS + Xcode. Use a Mac for iOS work;"
        " Pulp's other targets are cross-platform."};
    checks.push_back(c);
    return checks;
#else
    // Fan out the four independent probes concurrently. xcodebuild -showsdks
    // and `xcrun simctl list devices available` each take ~20-40s on cold
    // github-hosted ARM64 runners under CI load; serially that crossed the
    // test harness's 90s ceiling (#684). They share no data, so running
    // them in parallel collapses total wall time to max() of the slowest.
    auto xc_path_fut = std::async(std::launch::async, [] {
        return first_line(exec_output("xcode-select -p 2>/dev/null"));
    });
    auto xcrun_ver_fut = std::async(std::launch::async, [] {
        return first_line(exec_output("xcrun --version 2>&1"));
    });
    auto sdks_fut = std::async(std::launch::async, [] {
        return exec_output("xcodebuild -showsdks 2>/dev/null");
    });
    auto sims_fut = std::async(std::launch::async, [] {
        return exec_output("xcrun simctl list devices available 2>/dev/null");
    });

    {
        DoctorCheck c{"Xcode", false, {}, {}};
        auto xc_path = xc_path_fut.get();
        auto xcrun_ver = xcrun_ver_fut.get();
        if (!xc_path.empty()) {
            c.passed = true;
            c.detail = xc_path
                + (xcrun_ver.empty() ? "" : " (" + xcrun_ver + ")");
        } else {
            c.fix = "Install Xcode from the App Store, then:\n"
                    "    sudo xcode-select -s /Applications/Xcode.app\n"
                    "    sudo xcodebuild -license accept";
        }
        checks.push_back(c);
    }

    {
        DoctorCheck c{"iOS SDK installed", false, {}, {}};
        auto sdks = sdks_fut.get();
        if (sdks.find("iphoneos") != std::string::npos
         || sdks.find("iphonesimulator") != std::string::npos) {
            c.passed = true;
            c.detail = "iphoneos / iphonesimulator SDK present";
        } else {
            c.fix = "Open Xcode > Settings > Components and install the iOS SDK.";
        }
        checks.push_back(c);
    }

    {
        DoctorCheck c{"iOS Simulator runtime + at least one device",
                      false, {}, {}};
        auto sims = sims_fut.get();
        if (sims.find("iPhone") != std::string::npos
         || sims.find("iPad") != std::string::npos) {
            c.passed = true;
            c.detail = "at least one iOS Simulator device available";
        } else {
            c.fix = "Open Xcode > Settings > Components > Simulators, install a runtime,"
                    " then add an iOS device from Window > Devices and Simulators.";
        }
        checks.push_back(c);
    }

    return checks;
#endif
}

