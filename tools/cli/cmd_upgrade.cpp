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
    std::string from_override;
    std::string to_override;
    std::string target_version;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help" || args[i] == "-h") {
            std::cout << "pulp upgrade — update the Pulp CLI to the latest version\n\n";
            std::cout << "Usage: pulp upgrade [--check-only] [--notes [--json]] [--from X --to Y] [version]\n\n";
            std::cout << "Options:\n";
            std::cout << "  --check-only   Report the latest available version and exit.\n";
            std::cout << "                 Reads the update cache; does not download.\n";
            std::cout << "                 (Full enforcement lands in #499 Slice 5.)\n";
            std::cout << "  --notes        Print migration notes for the upgrade hop and exit.\n";
            std::cout << "                 No binary swap. Reads the embedded migration index.\n";
            std::cout << "  --notes --json Same, but emit a stable-shape JSON document instead\n";
            std::cout << "                 of human-readable text. Agent-consumable.\n";
            std::cout << "  --from X       Override the `from` version (default: installed CLI).\n";
            std::cout << "  --to   Y       Override the `to`   version (default: cached latest).\n";
            std::cout << "\n";
            std::cout << "If no version is specified, upgrades to the latest release.\n";
            std::cout << "Requires curl (macOS/Linux) or Invoke-WebRequest (Windows).\n";
            return 0;
        }
        if (args[i] == "--check-only") { check_only = true; continue; }
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
    // Reports what the banner would say without downloading. Reads the
    // cache written by the on-every-invocation background refresh in
    // pulp_cli.cpp. If the cache is empty (first run), we fall through
    // to the legacy check to keep the UX useful.
    if (check_only) {
        auto cache_path = update_cache_path();
        std::string installed = PULP_SDK_VERSION;
        std::optional<uc::CacheEntry> cache;
        if (!cache_path.empty()) cache = uc::read_cache_file(cache_path);

        std::string latest;
        std::string url;
        if (cache && !cache->latest_version.empty()) {
            latest = cache->latest_version;
            url = cache->release_notes_url;
        } else if (pulp::runtime::get_env("PULP_UPDATE_CHECK_DISABLED")) {
            std::cout << "Installed:  v" << installed << "\n";
            std::cout << "Latest:     update check disabled; not queried\n";
            return 0;
        } else {
            std::cout << "Cache empty; querying GitHub Releases...\n";
            uc::GitHubReleasesFetcher fetcher;
            auto r = fetcher.fetch_latest_release(PULP_GITHUB_REPO);
            if (!r.ok) {
                std::cerr << "Error: could not fetch latest version: " << r.error << "\n";
                return 1;
            }
            latest = r.latest_version;
            url = r.release_notes_url;
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

    // Try the cache first so repeated `pulp upgrade` calls within the
    // 24h window don't re-query GitHub.
    std::string latest;
    if (target_version.empty()) {
        auto cache_path = update_cache_path();
        if (!cache_path.empty()) {
            if (auto cache = uc::read_cache_file(cache_path)) {
                if (!cache->latest_version.empty() &&
                    !uc::is_cache_stale(*cache, uc::now_epoch_sec(), 24)) {
                    latest = cache->latest_version;
                }
            }
        }
    }

    if (latest.empty() && target_version.empty()) {
        uc::GitHubReleasesFetcher fetcher;
        auto r = fetcher.fetch_latest_release(PULP_GITHUB_REPO);
        if (r.ok) latest = r.latest_version;
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

    // Hint about the embedded migration notes — the `--notes` surface
    // lives in this same binary so the just-installed CLI can replay
    // the guidance for the hop the user just performed.
    std::string installed = PULP_SDK_VERSION;
    if (installed != version) {
        std::cout << "    Run `pulp upgrade --notes --from " << installed
                  << " --to " << version
                  << "` to see what changed.\n";
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
