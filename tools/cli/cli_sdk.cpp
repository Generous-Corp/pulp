// cli_sdk.cpp — SDK resolution, project-config, and version-banner
// helpers for the pulp CLI. Extracted from cli_common.cpp in the
// 2026-05 Phase 2 (R2-4) refactor.
//
// This is the "SDK / Config" + "#2087 newer-SDK-available banner"
// cluster of cli_common.cpp — SDK cache/checkout resolution, pulp.toml
// + project-CMake version reads, the newer-SDK banner, CLI/project
// compatibility enforcement, PR-workflow + user-config accessors, and
// the pinned-Shipyard-version probe.
//
// All public functions stay declared in cli_common.hpp (global scope,
// matching cli_common.cpp); this TU only relocates their definitions
// (plus the file-local anonymous-namespace helpers used nowhere else)
// so SDK/config work no longer recompiles the 2k-line cli_common.cpp.

#include "cli_common.hpp"
#include "fetchcontent_cache.hpp"
#include "version_diag.hpp"
#include "update_check.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <thread>

#include <pulp/runtime/system.hpp>

// ── SDK / Config ──────────────��────────────────────────────���────────────────

fs::path pulp_home() {
    if (auto pulp_home_env = pulp::runtime::get_env("PULP_HOME"))
        return fs::path(*pulp_home_env);

    auto home = pulp::runtime::get_env("HOME");
#ifdef _WIN32
    if (!home) home = pulp::runtime::get_env("USERPROFILE");
#endif
    if (!home) return {};
    return fs::path(*home) / ".pulp";
}

fs::path sdk_cache_path(const std::string& version) {
    return pulp_home() / "sdk" / version;
}

static void remove_tree(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

fs::path local_sdk_cache_path(const std::string& version) {
    return pulp_home() / "sdk-local" / detect_platform() / version;
}

// pulp #1814 — sdk_tarball_filename / legacy_unversioned_sdk_tarball_filename
// live in tools/cli/sdk_cache_paths.cpp so the matching unit test can
// compile + link them standalone.

std::string detect_platform() {
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

// ── #2087 follow-up: newer-SDK-available banner ─────────────────────────
//
// Pulp #2087 follow-up (#22): emit a one-line banner when a newer SDK
// is available on GitHub Releases. To keep CLI invocations fast and
// avoid hitting GitHub on every command, we cache the result at
// `~/.pulp/cache/latest_release.txt` with a 24h TTL. Cache is plain
// text — first line is the version string (no leading `v`), second
// line is the Unix timestamp of the fetch. Cache miss / stale →
// opportunistic refresh via curl with a hard 2s timeout so a slow
// network never blocks the user.

namespace {

constexpr int kLatestReleaseCacheTtlSeconds = 24 * 60 * 60;

fs::path latest_release_cache_path() {
    auto home = pulp_home();
    return home.empty() ? fs::path{} : (home / "cache" / "latest_release.txt");
}

bool semver_strictly_greater(const std::string& a, const std::string& b) {
    // Compare two `X.Y.Z` strings. Returns true iff a > b. Tolerates
    // a leading `v` on either side and any pre-release suffix (which
    // we ignore — pre-releases are intentionally not surfaced by the
    // banner, the user opted into them by installing).
    auto parse = [](std::string s) -> std::array<int, 3> {
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
        auto dash = s.find('-');
        if (dash != std::string::npos) s.resize(dash);
        std::array<int, 3> out{0, 0, 0};
        std::sscanf(s.c_str(), "%d.%d.%d", &out[0], &out[1], &out[2]);
        return out;
    };
    auto pa = parse(a);
    auto pb = parse(b);
    return pa > pb;
}

}  // namespace

std::string latest_available_sdk_version() {
    auto cache = latest_release_cache_path();
    if (cache.empty()) return {};

    // Try cache first.
    if (fs::exists(cache)) {
        std::ifstream in(cache);
        std::string version_line, timestamp_line;
        std::getline(in, version_line);
        std::getline(in, timestamp_line);
        try {
            auto ts = std::stoll(timestamp_line);
            auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (!version_line.empty()
                && (now - ts) < kLatestReleaseCacheTtlSeconds) {
                return version_line;
            }
        } catch (...) {
            // Bad cache — fall through to refresh.
        }
    }

    // Refresh — 2s timeout so a slow network never costs the user
    // more than ~2 seconds. Failure is silent (returns empty); the
    // banner just doesn't print this run.
    std::string url = "https://api.github.com/repos/"
                      + std::string(PULP_GITHUB_REPO)
                      + "/releases/latest";
    std::string cmd = "curl -fsSL --max-time 2 -H 'Accept: application/vnd.github+json' "
                      + shell_quote(url) + " 2>/dev/null";
    // Codex P1 on PR #2138: mirror the _WIN32 popen/pclose mapping used
    // elsewhere in tools/cli/ so this builds on Windows.
#if defined(_WIN32)
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return {};
    std::string body;
    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof(buf), pipe)) body.append(buf, n);
#if defined(_WIN32)
    if (_pclose(pipe) != 0 || body.empty()) return {};
#else
    if (pclose(pipe) != 0 || body.empty()) return {};
#endif

    // Custom raw-string delimiter `RE` — the default `R"(...)"` form
    // would close at the first `)"` inside the regex (after the
    // version-capture group), splitting the literal. CI on GCC caught
    // this. The delimiter eliminates the ambiguity.
    std::regex tag_re(R"RE("tag_name"\s*:\s*"v?([0-9]+\.[0-9]+\.[0-9]+)")RE");
    std::smatch m;
    if (!std::regex_search(body, m, tag_re)) return {};
    std::string version = m[1].str();

    // Write cache (best-effort; ignore failures).
    std::error_code ec;
    fs::create_directories(cache.parent_path(), ec);
    if (!ec) {
        std::ofstream out(cache);
        if (out) {
            auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            out << version << "\n" << now << "\n";
        }
    }
    return version;
}

void maybe_print_newer_sdk_banner(const std::string& installed) {
    if (installed.empty()) return;
    auto latest = latest_available_sdk_version();
    if (latest.empty()) return;
    if (!semver_strictly_greater(latest, installed)) return;
    std::cout << "  (Note: Pulp SDK v" << latest
              << " is available — installed: v" << installed
              << ". Run `pulp upgrade` or `pulp sdk install --version "
              << latest << "` to update.)\n";
}

fs::path ensure_sdk(const std::string& version) {
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
    auto extracted = fs::path(tmp_dir) / "pulp-sdk";
    if (!fs::exists(extracted)) {
        extracted = fs::path(tmp_dir);
    }

    try {
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

int ensure_checkout_dependencies(const fs::path& repo_root) {
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

fs::path ensure_checkout_sdk(const fs::path& repo_root, const std::string& version) {
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
        std::cerr << "Error: local SDK installation incomplete �� " << config.string() << " not found.\n";
        return {};
    }

    print_ok("Local SDK cached at " + sdk_dir.string());
    return sdk_dir;
}

std::string read_pulp_toml_value(const fs::path& project_root, const std::string& key) {
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

// Return the newest semver triple under ~/.pulp/sdk/<x.y.z>/. Skips
// directories whose name doesn't parse as semver, and skips entries
// without a version.txt (those are interrupted installs). Used by
// the "latest" floating-SDK resolution in read_sdk_version.
std::string newest_installed_sdk() {
    auto home = pulp_home();
    if (home.empty()) return {};
    auto sdk_base = home / "sdk";
    if (!fs::exists(sdk_base)) return {};

    auto parse_triple = [](const std::string& s, int& maj, int& min, int& patch) -> bool {
        maj = min = patch = -1;
        std::size_t i = 0;
        auto eat = [&](int& out) {
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
            int v = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                v = v * 10 + (s[i] - '0'); ++i;
            }
            out = v; return true;
        };
        if (!eat(maj)) return false;
        if (i >= s.size() || s[i] != '.') return false; ++i;
        if (!eat(min)) return false;
        if (i >= s.size() || s[i] != '.') return false; ++i;
        if (!eat(patch)) return false;
        return true;
    };

    std::string best;
    int bM = -1, bm = -1, bp = -1;
    for (auto& entry : fs::directory_iterator(sdk_base)) {
        if (!entry.is_directory()) continue;
        auto ver = entry.path().filename().string();
        int M, m, p;
        if (!parse_triple(ver, M, m, p)) continue;
        if (!fs::exists(entry.path() / "version.txt")) continue;
        bool newer = best.empty() || M > bM ||
                     (M == bM && m > bm) ||
                     (M == bM && m == bm && p > bp);
        if (newer) { best = ver; bM = M; bm = m; bp = p; }
    }
    return best;
}

std::string read_raw_sdk_version(const fs::path& project_root) {
    return read_pulp_toml_value(project_root, "sdk_version");
}

bool is_floating_sdk(const fs::path& project_root) {
    auto raw = read_raw_sdk_version(project_root);
    if (raw.empty()) return false;  // no project / no pin = not floating
    // Case-insensitive compare against "latest"
    std::string lower;
    lower.reserve(raw.size());
    for (char c : raw) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower == "latest";
}

std::string read_sdk_version(const fs::path& project_root) {
    auto version = read_raw_sdk_version(project_root);
    // Empty (no pulp.toml or no sdk_version key) preserves pre-#2087
    // behavior: fall back to the CLI's own SDK version. This is the
    // "no project at all" path — read_sdk_version is called from
    // contexts that have no project root, and returning newest-installed
    // there would surprise downstream code that expects PULP_SDK_VERSION.
    if (version.empty()) return PULP_SDK_VERSION;
    // Pulp #2087 floating mode: ONLY an explicit `sdk_version = "latest"`
    // resolves to the newest installed SDK at command time. The CLI's
    // own SDK version is the final fallback when no SDKs are installed
    // under ~/.pulp/sdk/ yet — common on a fresh `curl install.sh`
    // machine before the user has run `pulp sdk install`.
    std::string lower;
    lower.reserve(version.size());
    for (char c : version) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "latest") {
        auto resolved = newest_installed_sdk();
        if (!resolved.empty()) return resolved;
        return PULP_SDK_VERSION;
    }
    return version;
}

fs::path read_sdk_path_hint(const fs::path& project_root) {
    auto value = read_pulp_toml_value(project_root, "sdk_path");
    return value.empty() ? fs::path{} : fs::path(value);
}

fs::path read_sdk_checkout_hint(const fs::path& project_root) {
    auto value = read_pulp_toml_value(project_root, "sdk_checkout");
    return value.empty() ? fs::path{} : fs::path(value);
}

static bool sdk_config_exists(const fs::path& sdk_dir) {
    if (sdk_dir.empty()) return false;
    return fs::exists(sdk_dir / "lib" / "cmake" / "Pulp" / "PulpConfig.cmake");
}

static std::string read_sdk_dir_version(const fs::path& sdk_dir) {
    auto version_file = sdk_dir / "version.txt";
    if (!fs::exists(version_file)) return {};
    std::ifstream f(version_file);
    if (!f.is_open()) return {};
    std::string line;
    std::getline(f, line);
    return trim(line);
}

StandaloneSdkResolution resolve_standalone_sdk(const fs::path& project_root,
                                               bool materialize) {
    StandaloneSdkResolution out;
    out.requested_version = read_sdk_version(project_root);
    out.sdk_path_hint = read_sdk_path_hint(project_root);
    out.sdk_checkout_hint = read_sdk_checkout_hint(project_root);

    if (!out.sdk_path_hint.empty()) {
        out.sdk_path_config_ready = sdk_config_exists(out.sdk_path_hint);
        if (out.sdk_path_config_ready) {
            out.sdk_path_version = read_sdk_dir_version(out.sdk_path_hint);
            out.sdk_path_version_known = !out.sdk_path_version.empty();
            if (out.sdk_path_version_known) {
                auto actual = out.sdk_path_version;
                if (!actual.empty() && (actual.front() == 'v' || actual.front() == 'V')) {
                    actual.erase(0, 1);
                }
                out.sdk_path_version_matches = actual == out.requested_version;
                if (out.sdk_path_version_matches) {
                    out.resolved_sdk_dir = out.sdk_path_hint;
                    out.used_sdk_path_hint = true;
                    return out;
                }
                out.warning = "sdk_path points at SDK v" + actual +
                              " but pulp.toml requests v" +
                              out.requested_version + "; ignoring sdk_path";
            } else {
                out.sdk_path_custom_unverifiable = true;
                out.resolved_sdk_dir = out.sdk_path_hint;
                out.used_sdk_path_hint = true;
                return out;
            }
        }
    }

    if (!out.sdk_checkout_hint.empty() && fs::exists(out.sdk_checkout_hint)) {
        if (materialize) {
            out.resolved_sdk_dir = ensure_checkout_sdk(out.sdk_checkout_hint,
                                                       out.requested_version);
            if (!out.resolved_sdk_dir.empty()) return out;
        } else {
            auto local = local_sdk_cache_path(out.requested_version);
            if (sdk_config_exists(local)) {
                out.resolved_sdk_dir = local;
                return out;
            }
        }
    }

    auto downloaded = sdk_cache_path(out.requested_version);
    if (sdk_config_exists(downloaded)) {
        out.resolved_sdk_dir = downloaded;
        return out;
    }

    if (materialize) {
        out.resolved_sdk_dir = ensure_sdk(out.requested_version);
    }
    return out;
}

bool enforce_project_cli_compatibility(const fs::path& project_root,
                                       const std::string& command_name,
                                       bool allow_unsupported_sdk) {
    if (allow_unsupported_sdk || project_root.empty()) return true;

    namespace vd = pulp::cli::version_diag;
    auto cli = vd::parse_semver(PULP_SDK_VERSION);
    auto project_sdk = vd::parse_semver(read_sdk_version(project_root));
    auto project_cli_min = vd::read_project_cli_min_version(project_root);
    auto preflight = vd::analyze_execution_preflight(cli, project_sdk, project_cli_min);
    if (preflight.supported) return true;

    auto format_semver = [](const vd::Semver& value) {
        return value.raw.empty() ? std::string("(unknown)")
                                 : "v" + value.raw;
    };

    std::cerr << "Error: " << command_name
              << " blocked: project requires a newer Pulp CLI.\n";
    std::cerr << "  Installed CLI: " << format_semver(cli) << "\n";
    if (!project_sdk.raw.empty()) {
        std::cerr << "  Project SDK:   " << format_semver(project_sdk) << "\n";
    }
    if (project_cli_min.comparable) {
        std::cerr << "  CLI minimum:   " << format_semver(project_cli_min) << "\n";
    }
    std::cerr << "  Project root:  " << project_root.string() << "\n";
    for (const auto& blocker : preflight.blockers) {
        std::cerr << "  - " << blocker << "\n";
    }
    std::cerr << "\n";
    if (preflight.required_cli.comparable) {
        std::cerr << "Run `pulp upgrade " << preflight.required_cli.raw
                  << "` and retry.\n";
    } else {
        std::cerr << "Run `pulp upgrade` and retry.\n";
    }
    std::cerr << "Use `pulp doctor --versions` to inspect the mismatch.\n";
    std::cerr << "If you need to bypass this guard, rerun with "
              << "`--allow-unsupported-sdk` (unsupported).\n";
    return false;
}

bool cache_preflight_check(const fs::path& project_root,
                           const std::string& command_name) {
    namespace fcc = pulp::cli::fetchcontent_cache;

    // Escape hatch — CI environments that intentionally maintain a
    // sealed cache can opt out. Mirrors the `--allow-unsupported-sdk`
    // shape used by the version preflight above.
    if (const char* skip = std::getenv("PULP_SKIP_CACHE_PREFLIGHT");
        skip && skip[0] != '\0' && std::string(skip) != "0") {
        return true;
    }

    auto cache_root = fcc::default_cache_root();
    if (cache_root.empty()) return true;  // no derivable cache root → nothing to check

    fcc::DeclaredRefs refs;
    if (!project_root.empty()) {
        refs = fcc::parse_declared_refs_from_file(
            project_root / "CMakeLists.txt");
    }
    auto env = fcc::make_real_env(cache_root, refs);
    auto entries = fcc::discover_fetchcontent_cache(env);
    // Only gate on states that genuinely break configure/build —
    // StaleCommit entries are harmless because CMake's override path
    // keys on the *current* sanitized ref. See blocks_preflight() and
    // the Codex P1 review on PR #753.
    if (!fcc::blocks_preflight(entries)) return true;

    std::cerr << "Error: " << command_name
              << " blocked by FetchContent cache health check.\n";
    (void)fcc::render_preflight(entries, cache_root, std::cerr);
    return false;
}

std::string read_user_config_value(const std::string& section, const std::string& key) {
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

std::string normalize_pr_workflow(std::string workflow) {
    workflow = trim(strip_quotes(workflow));
    std::transform(workflow.begin(), workflow.end(), workflow.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return workflow;
}

bool is_valid_pr_workflow(const std::string& workflow) {
    const auto normalized = normalize_pr_workflow(workflow);
    return normalized == "shipyard" ||
           normalized == "github" ||
           normalized == "manual";
}

PrWorkflowSelection resolve_pr_workflow(const std::string& cli_override) {
    auto make = [](std::string raw, std::string source) {
        PrWorkflowSelection out;
        out.workflow = normalize_pr_workflow(raw);
        out.source = std::move(source);
        if (!is_valid_pr_workflow(out.workflow)) {
            out.error = "pr.workflow must be one of: shipyard, github, manual";
        }
        return out;
    };

    if (!trim(cli_override).empty()) {
        return make(cli_override, "cli");
    }
    if (const char* env = std::getenv("PULP_PR_WORKFLOW"); env && *env) {
        return make(env, "env:PULP_PR_WORKFLOW");
    }
    auto configured = read_user_config_value("pr", "workflow");
    if (!configured.empty()) {
        return make(configured, "config:pr.workflow");
    }
    return PrWorkflowSelection{"shipyard", "default", {}};
}

std::string read_pinned_shipyard_version(const fs::path& root) {
    std::ifstream f(root / "tools" / "shipyard.toml");
    if (!f) return {};
    std::string line;
    while (std::getline(f, line)) {
        auto t = trim(line);
        if (t.rfind("version", 0) != 0) continue;
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        auto rhs = trim(t.substr(eq + 1));
        if (rhs.size() >= 2 && rhs.front() == '"' && rhs.back() == '"') {
            return rhs.substr(1, rhs.size() - 2);
        }
        return rhs;
    }
    return {};
}

static std::string parse_shipyard_version_output(std::string out) {
    for (char& c : out) {
        if (c == ',' || c == '(' || c == ')') c = ' ';
    }
    std::istringstream tokens(out);
    std::string token;
    while (tokens >> token) {
        while (!token.empty() &&
               (token.back() == ',' || token.back() == ';' || token.back() == ':')) {
            token.pop_back();
        }
        auto check = token;
        if (!check.empty() && check.front() == 'v') check.erase(check.begin());
        if (check.empty() || !std::isdigit(static_cast<unsigned char>(check.front()))) {
            continue;
        }
        if (check.find('.') == std::string::npos) continue;
        return token.front() == 'v' ? token : ("v" + token);
    }
    return {};
}

std::string capture_shipyard_version(const std::string& shipyard_bin) {
    if (shipyard_bin.empty()) return {};
    auto out = exec_output(shell_quote(shipyard_bin) + " --version 2>&1");
    return parse_shipyard_version_output(trim(out));
}

bool write_user_config_value(const std::string& section,
                             const std::string& key,
                             const std::string& value) {
    auto home = pulp_home();
    if (home.empty()) return false;
    auto path = home / "config.toml";

    std::string contents;
    if (fs::exists(path)) {
        std::ifstream f(path);
        if (f.is_open()) {
            std::ostringstream buf;
            buf << f.rdbuf();
            contents = buf.str();
        }
    }

    auto rewritten = pulp::cli::update_check::write_toml_key_in_section(
        contents, section, key, value);

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f << rewritten;
    return f.good();
}

std::string read_project_cmake_version(const fs::path& project_root) {
    auto cmake_path = project_root / "CMakeLists.txt";
    if (!fs::exists(cmake_path)) return {};

    std::ifstream f(cmake_path);
    std::string line;
    std::regex version_re(R"(project\s*\([^)]*VERSION\s+(\d+\.\d+\.\d+))");
    while (std::getline(f, line)) {
        std::smatch m;
        if (std::regex_search(line, m, version_re)) {
            return m[1].str();
        }
    }
    return {};
}

