// cmd_upgrade.cpp — `pulp upgrade` subcommand
//
// Moved out of cmd_misc.cpp in release-discovery Slice 2 (#547) so the
// surface grows independently. This slice adds `--check-only` which
// reports the cached latest release without downloading anything, and
// wires the pre-fetch/cache read so the upgrade path shares state with
// the on-every-invocation update banner.
//
// The actual enforcement of auto/prompt/manual/off update modes lands
// in Slice 5 (#499). This slice is the minimal surface: a config
// reader, a cache, a banner, and a stub that tells the user what
// would happen.
//
// URL/asset-name convention lives in upgrade_url.hpp — DO NOT bake
// the version into the filename. See .agents/skills/cli-maintenance
// "`pulp upgrade`" section and test_cli_upgrade_url.cpp.

#include "cli_common.hpp"
#include "migration_index.hpp"
#include "update_check.hpp"
#include "upgrade_install.hpp"
#include "upgrade_url.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace uc = pulp::cli::update_check;
namespace mig = pulp::cli::migration;

// ── Cache location helpers (shared with pulp_cli.cpp dispatch path) ─────────

fs::path update_cache_path() {
    auto home = pulp_home();
    if (home.empty()) return {};
    return home / "update-cache.json";
}

int cmd_upgrade(const std::vector<std::string>& args) {
    bool check_only = false;
    bool notes_only = false;
    bool notes_json = false;
    // Pulp #2087: `pulp upgrade` updates BOTH CLI and SDK by default.
    // `--cli-only` keeps the pre-#2087 behavior for the rare case
    // where a user wants a newer CLI paired with their current SDK
    // (e.g., they want a CLI bug fix without touching project pins).
    bool cli_only = false;
    std::string from_override;
    std::string to_override;
    std::string target_version;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help" || args[i] == "-h") {
            std::cout << "pulp upgrade — update the Pulp CLI + SDK to the latest version\n\n";
            std::cout << "Usage: pulp upgrade [--check-only] [--cli-only] [--notes [--json]] [--from X --to Y] [version]\n\n";
            std::cout << "Options:\n";
            std::cout << "  --check-only   Report the latest available version and exit.\n";
            std::cout << "                 Reads the update cache; does not download.\n";
            std::cout << "  --cli-only     Update CLI only; skip SDK install. Pre-#2087 behavior.\n";
            std::cout << "                 Default since #2087: also fetch the matching SDK.\n";
            std::cout << "  --notes        Print migration notes for the upgrade hop and exit.\n";
            std::cout << "                 No binary swap. Reads the embedded migration index.\n";
            std::cout << "  --notes --json Same, but emit a stable-shape JSON document instead\n";
            std::cout << "                 of human-readable text. Agent-consumable.\n";
            std::cout << "  --from X       Override the `from` version (default: installed CLI).\n";
            std::cout << "  --to   Y       Override the `to`   version (default: cached latest).\n";
            std::cout << "\n";
            std::cout << "If no version is specified, upgrades to the latest release.\n";
            std::cout << "Requires curl (macOS/Linux) or Invoke-WebRequest (Windows).\n";
            std::cout << "\n";
            std::cout << "Project pinning (#2087): if pulp upgrade is run from inside a project\n";
            std::cout << "whose pulp.toml pins an explicit SDK version, the SDK swap leaves that\n";
            std::cout << "pin alone — pinning is sacred. Run `pulp project unpin` first to track\n";
            std::cout << "latest, or `pulp project pin <new>` to migrate the pin.\n";
            return 0;
        }
        if (args[i] == "--check-only") { check_only = true; continue; }
        if (args[i] == "--cli-only")   { cli_only = true; continue; }
        if (args[i] == "--notes")      { notes_only = true; continue; }
        if (args[i] == "--json")       { notes_json = true; continue; }
        if (args[i] == "--from" && i + 1 < args.size()) { from_override = args[++i]; continue; }
        if (args[i] == "--to"   && i + 1 < args.size()) { to_override   = args[++i]; continue; }
        if (args[i][0] != '-') target_version = args[i];
    }

    // ── --notes path (Slice 3 surface) ──────────────────────────────────────
    //
    // Print migration notes that apply to the hop `from` → `to` and
    // exit. No network, no binary swap. Defaults:
    //   from = PULP_SDK_VERSION (the version of the running binary)
    //   to   = cached latest_version (falls back to `from` if no cache)
    //
    // The JSON variant is stable-shape (see migration_index.hpp) — Slice 4
    // depends on it. Printed to stdout so scripts can `pulp upgrade
    // --notes --json | jq` without stripping a banner.
    if (notes_only) {
        std::string from = !from_override.empty() ? from_override : std::string(PULP_SDK_VERSION);
        std::string to = to_override;
        if (to.empty()) {
            auto cache_path = update_cache_path();
            if (!cache_path.empty()) {
                if (auto cache = uc::read_cache_file(cache_path);
                    cache && !cache->latest_version.empty()) {
                    to = cache->latest_version;
                }
            }
            if (to.empty()) to = from;  // no cache; degenerate hop = no notes
        }

        auto entries = mig::applicable_entries(from, to);
        if (notes_json) {
            std::cout << mig::render_notes_json(entries, from, to);
        } else {
            std::cout << mig::render_notes_text(entries, from, to);
        }
        return 0;
    }

    // ── --check-only path (Slice 2 surface) ─────────────────────────────────
    //
    // Reports what the banner would say. Uses uc::resolve_latest_with_persist
    // so a stale or empty cache triggers a synchronous refresh. The
    // on-every-invocation detached refresh in pulp_cli.cpp gets killed when
    // short-lived commands exit before curl completes (#1599), so we
    // cannot rely on it. The user is explicitly asking about updates here,
    // so a 1–2s blocking GitHub call is the right tradeoff.
    if (check_only) {
        auto cache_path = update_cache_path();
        std::string installed = PULP_SDK_VERSION;
        const bool disabled =
            pulp::runtime::get_env("PULP_UPDATE_CHECK_DISABLED").has_value();

        std::string latest;
        std::string url;
        if (disabled) {
            // Honor the off-switch even when there's no cache yet — the
            // contract is "zero network calls".
            auto cache = !cache_path.empty() ? uc::read_cache_file(cache_path)
                                             : std::nullopt;
            if (cache && !cache->latest_version.empty()) {
                latest = cache->latest_version;
                url = cache->release_notes_url;
            } else {
                std::cout << "Installed:  v" << installed << "\n";
                std::cout << "Latest:     update check disabled; not queried\n";
                return 0;
            }
        } else {
            uc::GitHubReleasesFetcher fetcher;
            auto resolved = uc::resolve_latest_with_persist(
                fetcher, cache_path, PULP_GITHUB_REPO, uc::now_epoch_sec(), 24);
            if (resolved.refreshed) {
                std::cout << "Refreshing from GitHub Releases...\n";
            }
            if (!resolved.ok) {
                std::cerr << "Error: could not fetch latest version: "
                          << resolved.error << "\n";
                return 1;
            }
            latest = resolved.latest_version;
            url = resolved.release_notes_url;
        }

        std::cout << "Installed:  v" << installed << "\n";
        std::cout << "Latest:     v" << latest << "\n";
        if (!url.empty()) std::cout << "Notes:      " << url << "\n";
        if (uc::is_newer(installed, latest)) {
            std::cout << "\nA newer release is available. Run `pulp upgrade` to install it.\n";
            std::cout << "Slice 5 (#499) will add auto/prompt/manual/off enforcement.\n";
        } else {
            std::cout << "\nYou're on the latest release.\n";
        }
        return 0;
    }

    std::cout << "Checking for updates...\n";

    // Resolve the latest version. resolve_latest_with_persist handles the
    // empty/stale/fresh cases and writes the cache on a successful fetch
    // so the next invocation isn't stuck behind the unreliable detached
    // background refresh (#1599).
    std::string latest;
    if (target_version.empty()) {
        uc::GitHubReleasesFetcher fetcher;
        auto resolved = uc::resolve_latest_with_persist(
            fetcher, update_cache_path(), PULP_GITHUB_REPO,
            uc::now_epoch_sec(), 24);
        if (resolved.ok) latest = resolved.latest_version;
    }

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
    // Release assets use "x64" (not "x86_64"). Keep this in sync with the
    // release workflow in .github/workflows/release-cli.yml — otherwise
    // `pulp upgrade` 404s on the download step.
    arch = "x64";
#else
    arch = "unknown";
#endif

    std::string self_path = current_executable_path().string();
    if (self_path.empty()) {
        std::cerr << "Error: could not determine current binary path.\n";
        std::cerr << "  Download manually from: https://github.com/danielraffel/pulp/releases\n";
        return 1;
    }

    auto [tarball, url] = pulp::cli::pulp_upgrade_url_for(version, platform, arch);
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

    fs::path tmp_path = tmp_dir;
    fs::path archive_path = tmp_path / tarball;
    fs::path new_binary = tmp_path / pulp::cli::upgrade_install::primary_binary_name();
    if (!fs::exists(new_binary)) {
        std::cerr << "Error: extracted archive does not contain '"
                  << pulp::cli::upgrade_install::primary_binary_name()
                  << "' binary.\n";
        run("rm -rf " + tmp_dir);
        return 1;
    }

    fs::path self = self_path;
    fs::path install_dir = self.parent_path();
    if (install_dir.empty()) install_dir = fs::current_path();
    std::string backup = self_path + ".bak";
    std::vector<fs::path> installed_payloads;
    try {
        // Copy sibling payloads first. If this fails, leave the current
        // `pulp` binary untouched; after self-replace, Rust needs the
        // sibling `pulp-cpp` delegate to service unported commands.
        installed_payloads = pulp::cli::upgrade_install::install_sibling_payloads(
            tmp_path, install_dir, new_binary, archive_path);

        if (fs::exists(backup)) fs::remove(backup);
        fs::rename(self, backup);
        fs::copy_file(new_binary, self);
        fs::permissions(self, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write
                                 | fs::perms::group_exec | fs::perms::group_read
                                 | fs::perms::others_exec | fs::perms::others_read);
        fs::remove(backup);
    } catch (const std::exception& e) {
        std::cerr << "Error installing upgrade payload: " << e.what() << "\n";
        if (fs::exists(backup) && !fs::exists(self)) {
            fs::rename(backup, self);
        }
        run("rm -rf " + tmp_dir);
        return 1;
    }

    run("rm -rf " + tmp_dir);

    std::cout << "\n  \xe2\x9c\x93 Pulp CLI upgraded to v" << version << "\n";
    if (pulp::cli::upgrade_install::installed_cpp_delegate(installed_payloads)) {
        std::cout << "    Installed pulp-cpp delegate alongside pulp.\n";
    }

    // Hint about the embedded migration notes — the `--notes` surface
    // lives in this same binary so the just-installed CLI can replay
    // the guidance for the hop the user just performed.
    std::string installed = PULP_SDK_VERSION;
    if (installed != version) {
        std::cout << "    Run `pulp upgrade --notes --from " << installed
                  << " --to " << version
                  << "` to see what changed.\n";
    }

    // Pulp #2087: by default, also install the matching SDK so the
    // CLI and SDK stay coherent. The CLI's own `sdk install` flow
    // handles caching, idempotence, and the GitHub-releases fetch.
    //
    // We spawn the NEWLY-installed CLI (not this process's binary, in
    // case the executable's been replaced) so the SDK install uses
    // the new version's release-asset URL conventions if those changed.
    //
    // If running inside a project that pins an explicit SDK in pulp.toml,
    // we install the latest SDK side-by-side but leave the project's
    // pin alone — pinning is sacred. The user is told how to opt out
    // of pinning if they want to track latest.
    if (!cli_only) {
        std::cout << "\n  Installing matching SDK v" << version << "...\n";
        // Use shell quoting because self_path might contain spaces.
        int sdk_rc = run(shell_quote(self_path) + " sdk install --version "
                         + version);
        if (sdk_rc == 0) {
            std::cout << "    \xe2\x9c\x93 SDK v" << version << " installed.\n";
            if (installed != version) {
                std::cout << "    Versions: CLI " << installed << " -> "
                          << version << "; SDK " << installed << " -> "
                          << version << "\n";
            }
        } else {
            std::cout << "    (warning) SDK install failed; CLI was updated.\n";
            std::cout << "    Retry with: pulp sdk install --version "
                      << version << "\n";
        }

        // Detect a pinned project in cwd and surface a clear non-action
        // notice so the user knows the pin wasn't touched. This is the
        // user-facing payload of "pinning is sacred" — without the
        // notice, users who type `pulp upgrade` from inside a pinned
        // project wonder why their build still uses the old SDK.
        auto cwd = fs::current_path();
        if (fs::exists(cwd / "pulp.toml") && !is_floating_sdk(cwd)) {
            auto pinned = read_raw_sdk_version(cwd);
            auto project_name = cwd.filename().string();
            std::cout << "\n  \xe2\x84\xb9 Project " << project_name
                      << " stays on pinned SDK " << pinned
                      << "; latest available is " << version << ".\n"
                      << "    Run `pulp project unpin` to start tracking latest,\n"
                      << "    or `pulp project pin " << version
                      << "` to migrate the pin.\n";
        }
    }

    // Slice 7 (#564) wiring: honor update.bump_projects after a
    // successful CLI upgrade. The just-installed binary owns the
    // actual `pulp project bump --all` behaviour — we emit a hint
    // or exec into the binary depending on the configured mode.
    //
    //   prompt (default) → print the hint; let the user run it
    //   auto             → print the hint; a future slice execs it
    //                      (Windows-safe: runs OUTSIDE pulp.exe)
    //   off              → stay quiet
    //
    // The full "end-to-end silent" flow from the design doc needs the
    // replaced binary to be the one running `project bump`, which is
    // the NEXT invocation. This slice prints the correct nudge now;
    // the actual spawn lives in the follow-up (tracked inline).
    {
        auto bp = trim(read_user_config_value("update", "bump_projects"));
        if (bp.empty()) bp = "prompt";
        if (bp != "off") {
            std::cout << "\n  Pin projects to the new CLI version:\n"
                      << "    pulp project bump --all" << "\n";
            if (bp == "auto") {
                std::cout << "    (update.bump_projects = auto; will run on next invocation)\n";
            }
        }
    }

    // Bump the banner-shown marker so we don't nag the user again
    // about the version they just installed.
    auto cache_path = update_cache_path();
    if (!cache_path.empty()) {
        auto cache = uc::read_cache_file(cache_path).value_or(uc::CacheEntry{});
        cache.banner_shown_for_version = version;
        cache.latest_version = version;
        cache.last_check_epoch_sec = uc::now_epoch_sec();
        uc::write_cache_file(cache_path, cache);
    }
    return 0;
}
