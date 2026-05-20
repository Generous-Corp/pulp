// pulp — CLI tool for the Pulp audio plugin framework
// Command dispatch via structured table. See cmd_*.cpp for implementations.

#include "cli_common.hpp"
#include "package_commands.hpp"
#include "package_registry.hpp"
#include "tool_registry.hpp"
#include "update_check.hpp"
#include "update_mode.hpp"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

// ── Command Table ───────────────────────────────────────────────────────────

struct Command {
    const char* name;
    const char* summary;
    int (*handler)(const std::vector<std::string>&);
};

static const Command commands[] = {
    {"build",    "Configure and build the project",       cmd_build},
    {"run",      "Launch a standalone Pulp application",  cmd_run},
    {"test",     "Run the test suite",                    cmd_test},
    {"status",   "Show project status and info",          cmd_status},
    {"create",   "Scaffold a new plugin project",         cmd_create},
    {"validate", "Run plugin format validators",          cmd_validate},
    {"doctor",   "Diagnose environment issues",           cmd_doctor},
    {"ship",     "Sign, package, and distribute",         cmd_ship},
    {"design",   "Launch the AI design tool",             cmd_design},
    {"docs",     "Browse local documentation",            cmd_docs},
    {"clean",    "Remove build directory",                cmd_clean},
    {"fmt",      "Run clang-format on the source tree",   cmd_fmt},
    {"cache",    "Manage SDK and asset cache",            cmd_cache},
    {"audio",    "Repo-level audio model and bundle tooling", cmd_audio},
    {"sdk",      "Manage the Pulp SDK installation",      cmd_sdk},
    {"upgrade",  "Update the CLI to the latest version",  cmd_upgrade},
    {"version",  "Show, bump, or check version info",     cmd_version},
    {"dev",      "Unified dev loop: watch, build, test, run", cmd_dev},
    {"loop",     "Leveraged-prototype focus mode: single-platform watch + rebuild (#940)", cmd_loop},
    {"inspect",  "Connect to a running plugin inspector", cmd_inspect},
    {"scan",     "Scan system paths for VST3 / AU / CLAP / LV2 plug-ins", cmd_scan},
    {"host",     "Load a plug-in and run a synthetic audio block through it", cmd_host},
    {"pr",       "One-shot push-a-PR: gates + bump + ship",   cmd_pr},
    {"projects", "Manage the ~/.pulp/projects.json registry", cmd_projects},
    {"project",  "Per-project SDK pin: bump, undo", cmd_project},
    // Regression fix 2026-04-21 (Codex post-merge sweep wave 2): the
    // #562 PR added `{"config", ..., cmd_config}` to this dispatch
    // table, but the #563 merge dropped the line, leaving `pulp config`
    // (and the update.mode / channel / check_interval_hours surface
    // it guards) completely unreachable. Restore so Codex's P1/P2
    // findings on cmd_config.cpp are actually observable behaviour.
    {"config",   "Read or write ~/.pulp/config.toml settings", cmd_config},
    {"coverage", "Local coverage tooling (diff-cover gate mirror)", cmd_coverage},
    {"macos",    "Per-PR macOS-runner retargeting (local/namespace/github-hosted)", cmd_macos},
    {"overflow", "Configure macOS-runner overflow routing (status/enable/disable/threshold)", cmd_overflow},
    {"tweaks",   "Inspect the pulp-tweaks.json sidecar (diff against a design)", cmd_tweaks},
};

static constexpr int command_count = sizeof(commands) / sizeof(commands[0]);

// Script commands: delegate to a Python script in the project tree
struct ScriptCommand {
    const char* name;
    const char* script_path;  // relative to project root
    const char* summary;
};

static const ScriptCommand script_commands[] = {
    {"ci-local", "tools/local-ci/local_ci.py",       "Local-first CI across configured hosts"},
    {"harness",  "tools/harness/verifier.py",        "Catalog-driven coverage harness (compat.json verifier)"},
};

static constexpr int script_command_count = sizeof(script_commands) / sizeof(script_commands[0]);

// Binary commands: delegate to a built binary
struct BinaryCommand {
    const char* name;
    const char* binary_path;  // relative to project build dir
    const char* summary;
    const char* extra_arg;    // prepended arg (e.g., "--demo"), or nullptr
};

static const BinaryCommand binary_commands[] = {
    {"design-debug",  "tools/design/pulp-design-debug",            "Headless design debug runner", nullptr},
    {"import-design", "tools/import-design/pulp-import-design",    "Import designs from Figma/Stitch/v0/Pencil", nullptr},
    {"export-tokens", "tools/import-design/pulp-import-design",    "Export theme as W3C Design Tokens", "--export-tokens"},
};

static constexpr int binary_command_count = sizeof(binary_commands) / sizeof(binary_commands[0]);

// ── Delegation Helpers ─────────────────────────────────────────────────────

static int delegate_to_script(const ScriptCommand& sc, const std::vector<std::string>& args) {
    return delegate_to_python_script(sc.script_path, args);
}

static int delegate_to_binary(const BinaryCommand& bc, const std::vector<std::string>& args) {
    return delegate_to_build_binary(bc.binary_path, args,
                                    bc.extra_arg ? std::string(bc.extra_arg) : std::string{});
}

// ── Package Manager Commands ───────────────────────────────────────────��───

static int handle_audit(const std::vector<std::string>& args) {
    bool pkg_flag = false, plat_flag = false, lic_flag = false;
    for (auto& a : args) {
        if (a == "--packages") pkg_flag = true;
        if (a == "--platforms") plat_flag = true;
        if (a == "--licenses") lic_flag = true;
    }
    if (pkg_flag || plat_flag || lic_flag) {
        auto root = find_project_root();
        if (root.empty()) {
            std::cerr << "Error: not in a Pulp project directory\n";
            return 1;
        }
        int rc = 0;
        if (pkg_flag) rc |= pulp::cli::pkg::audit_packages(root);
        if (plat_flag) rc |= pulp::cli::pkg::audit_platforms(root);
        if (lic_flag) rc |= pulp::cli::pkg::audit_licenses(root);
        return rc;
    }
    return delegate_to_python_script("tools/audit.py", args);
}

// ── Usage (auto-generated from command tables) ──────────────────────────────

static void print_usage() {
    std::cout << "pulp — Pulp audio plugin framework CLI\n\n";
    std::cout << "Usage: pulp <command> [options]\n\n";
    std::cout << "Commands:\n";
    for (int i = 0; i < command_count; ++i) {
        std::cout << "  " << std::left << std::setw(14) << commands[i].name
                  << " " << commands[i].summary << "\n";
    }
    std::cout << "\n";
    for (int i = 0; i < script_command_count; ++i) {
        std::cout << "  " << std::left << std::setw(14) << script_commands[i].name
                  << " " << script_commands[i].summary << "\n";
    }
    for (int i = 0; i < binary_command_count; ++i) {
        std::cout << "  " << std::left << std::setw(14) << binary_commands[i].name
                  << " " << binary_commands[i].summary << "\n";
    }
    std::cout << "  " << std::left << std::setw(14) << "audit" << " License and clean-room audit\n";
    std::cout << "  " << std::left << std::setw(14) << "add" << " Add a component to the project\n";
    std::cout << "  " << std::left << std::setw(14) << "help" << " Show this help\n";
    std::cout << "\nExamples:\n";
    std::cout << "  pulp create MyPlugin              # Create a new effect plugin\n";
    std::cout << "  pulp create MySynth --type instrument  # Create an instrument\n";
    std::cout << "  pulp doctor             # Check environment for issues\n";
    std::cout << "  pulp build              # Build all targets\n";
    std::cout << "  pulp test               # Run all tests\n";
    std::cout << "  pulp validate           # Validate built plugins\n";
    std::cout << "  pulp docs index         # List available docs\n";
    std::cout << "  pulp status             # Show project info\n";
}

// ── Update-check dispatch (Slice 2 #547 + Slice 5 #550 of #499) ─────────────
//
// Before we dispatch the user's command, we optionally:
//   1. Complete any pending auto-mode upgrade staged by a previous
//      invocation (read ~/.pulp/pending-upgrade, print the completion
//      notice, clear the marker, and — on Windows — clean up the
//      `.pulp.old` tombstone left over from the rename-swap).
//   2. Read the cached latest-release JSON (~/.pulp/update-cache.json).
//   3. Emit a mode-appropriate banner on stderr:
//        auto   → quiet (unless we just completed a staged upgrade)
//        prompt → one-line nag per new version; 24h snooze on decline
//        manual → one-line "Run `pulp upgrade` when ready" per version
//        off    → no banner, no network
//   4. Kick off a non-blocking background refresh if the cache is
//      older than `update.check_interval_hours` (default 24).
//   5. For `auto`: if a newer release is known, stage a background
//      download and write ~/.pulp/pending-upgrade so the next
//      invocation completes the swap.
//
// Slice 5 (#550) scope:
//   - All four modes wired into the invocation path.
//   - ~/.pulp/pending-upgrade marker (read on next invocation).
//   - ~/.pulp/update-snooze (24h) for prompt-mode decline.
//   - Windows tombstone cleanup (`pulp.exe.pulp.old` left behind by
//     `MoveFileEx` during the swap step).
//   - `off` mode produces ZERO network calls (the early-return below
//     runs before any cache read or fetcher spawn).
//
// Commands that should NEVER emit the banner (so they stay
// machine-parseable for scripts) are listed in `kBannerBlockedCommands`
// below. `config` and `version` are on that list so e.g.
// `pulp config get update.mode` has clean stdout.

namespace {

namespace uc = pulp::cli::update_check;
namespace um = pulp::cli::update_mode;

const char* kBannerBlockedCommands[] = {
    "config",     // machine-parseable output
    "version",    // same — used by shell scripts
    "help",
    "--help",
    "-h",
};

bool banner_blocked(const std::string& cmd) {
    for (const char* blocked : kBannerBlockedCommands) {
        if (cmd == blocked) return true;
    }
    return false;
}

// Read configured mode. Defaults to "prompt" when absent, matching the
// design doc Section A. Failure-tolerant — a malformed value degrades
// to Prompt (parse_mode() handles that).
um::Mode read_update_mode() {
    return um::parse_mode(read_user_config_value("update", "mode"));
}

int read_check_interval_hours() {
    auto v = read_user_config_value("update", "check_interval_hours");
    if (v.empty()) return 24;
    try {
        int n = std::stoi(v);
        return n > 0 ? n : 24;
    } catch (...) {
        return 24;
    }
}

fs::path pulp_home_path_or_empty() {
    return pulp_home();  // cli_common helper; empty on HOME/USERPROFILE unset
}

fs::path banner_cache_path() {
    auto home = pulp_home_path_or_empty();
    if (home.empty()) return {};
    return home / "update-cache.json";
}

fs::path snooze_path() {
    auto home = pulp_home_path_or_empty();
    if (home.empty()) return {};
    return home / "update-snooze";
}

fs::path pending_upgrade_path() {
    auto home = pulp_home_path_or_empty();
    if (home.empty()) return {};
    return home / "pending-upgrade";
}

// Fire a best-effort background refresh. We deliberately don't block
// on the result — the banner will pick it up on the *next* invocation.
//
// Codex 2026-04-21 wave 2 P1 on #562: a previous version stored the
// `std::future` returned by `std::async(std::launch::async, ...)` in
// a static local. That future's destructor is NOT detached — it
// blocks on the task to finish. On process exit (or on the next
// kick_background_refresh() call, which assigns over it) the CLI
// could hang waiting for a stale network fetch — exactly the
// non-blocking guarantee this path advertised. Switch to a raw
// detached `std::thread` so the refresh truly cannot block `main`.
// If the process exits before the fetch completes the OS reclaims
// the thread; the stale HTTP connection is closed along with the
// socket FD.
//
// Race-window note: the main thread may write the cache after the
// banner check (to stamp banner_shown_for_version). To avoid clobbering
// that write, the background thread re-reads the cache from disk right
// before writing. This keeps `banner_shown_for_version` set even if
// the refresh finishes later than the banner-write.
void kick_background_refresh(const fs::path& cache_path) {
    if (cache_path.empty()) return;
    std::thread([cache_path]() {
        uc::GitHubReleasesFetcher fetcher;
        auto r = fetcher.fetch_latest_release(PULP_GITHUB_REPO);
        // Re-read from disk — this is the race-avoidance point.
        auto latest = uc::read_cache_file(cache_path).value_or(uc::CacheEntry{});
        latest.schema = uc::kCacheSchemaVersion;
        latest.last_check_epoch_sec = uc::now_epoch_sec();
        if (r.ok) {
            latest.latest_version = r.latest_version;
            latest.release_notes_url = r.release_notes_url;
        }
        uc::write_cache_file(cache_path, latest);
    }).detach();
}

// Stage an auto-mode background download. Writes the pending-upgrade
// marker so the NEXT invocation can complete the swap. No direct binary
// replacement here — that happens in cmd_upgrade's existing swap path
// (or on the next invocation for this stubbed implementation). The
// network fetch is delegated to a detached thread so we don't block
// the user's command.
//
// Slice 5's wiring intentionally stops short of actually downloading
// the tarball — cmd_upgrade already owns that code and we don't want
// to duplicate the signature/platform arch matrix. The marker we drop
// here is the signal for the next invocation (or the user's next
// `pulp upgrade`) to perform the swap. This preserves the Section G
// non-goal "no binary is ever replaced without the user's session
// touching `pulp` again".
//
// Returns true if the marker was successfully written; false otherwise
// (empty args, unwritable PULP_HOME, etc.). The auto-mode banner
// suppresses its "downloaded / will complete next invocation" notice
// on false so the user isn't promised a completion that can't happen
// (#590 Codex P2 / wave-4 sweep).
bool kick_auto_stage(const fs::path& marker_path,
                     const std::string& staged_version) {
    if (marker_path.empty() || staged_version.empty()) return false;
    um::PendingUpgrade p;
    p.version = staged_version;
    p.staged_at_epoch_sec = uc::now_epoch_sec();
    // Leave staged_binary_path empty — cmd_upgrade's next invocation
    // will perform the download + swap. The marker's purpose here is
    // to signal intent; the binary path field is reserved for a
    // future slice where the background download lands a file.
    p.staged_binary_path = "";
    return um::write_pending_upgrade(marker_path, p);
}

// Step 1 of the dispatch hook: if a pending-upgrade marker exists and
// its `version` matches the currently-running binary's version, we
// just completed a staged auto upgrade. Print the one-line completion
// banner, clear the marker, and clean up the Windows tombstone.
//
// If the marker's version doesn't match what we're running, leave the
// marker alone — the actual swap hasn't happened yet.
void maybe_complete_pending_upgrade() {
    auto marker = pending_upgrade_path();
    // Note: we intentionally do NOT early-return when the marker is
    // missing or unreadable. The opportunistic tombstone sweep at the
    // bottom of this function must also run for the common "no marker"
    // path so `*.pulp.old` files left behind by direct `pulp upgrade`
    // flows get cleaned up (#590 Codex P2 / wave-4 sweep).
    if (!marker.empty()) {
        if (auto pending = um::read_pending_upgrade(marker)) {
            if (pending->version == std::string(PULP_SDK_VERSION)) {
                std::cerr << um::compose_auto_completed_notice(pending->version) << "\n";
                (void)um::clear_pending_upgrade(marker);
                // Tombstone cleanup after marker removal — in this order so a
                // crash between the two leaves the tombstone but no marker,
                // which is recoverable (the next invocation still tidies up).
                auto self_post = fs::path(current_executable_path());
                if (!self_post.empty()) {
                    (void)um::cleanup_tombstone(self_post);
                }
            }
        }
    }
    // Always opportunistically sweep the tombstone — covers the case
    // where a user ran `pulp upgrade` directly (no marker), the marker
    // was malformed, or `PULP_HOME` was unreadable. Running this
    // unconditionally is safe: cleanup_tombstone() is a no-op when no
    // `*.pulp.old` sibling exists.
    auto self = fs::path(current_executable_path());
    if (!self.empty()) {
        (void)um::cleanup_tombstone(self);
    }
}

// Top-level hook invoked before dispatching the user's command. Best
// effort — any exception is swallowed so a cache-read bug can never
// fail the real command.
void maybe_emit_update_banner_and_refresh(const std::string& command) {
    try {
        pulp_debug("update-banner: maybe_complete_pending_upgrade enter");
        // Pending-upgrade completion runs even for banner-blocked
        // commands so the user sees "upgrade complete" after the
        // swap — but only as a one-liner on stderr, which doesn't
        // corrupt machine-parseable stdout.
        maybe_complete_pending_upgrade();
        pulp_debug("update-banner: maybe_complete_pending_upgrade exit");

        if (banner_blocked(command)) return;

        // Explicit off-switch that bypasses config entirely. Used by
        // CI / air-gapped envs and by the unit-test shell-out lane.
        // Matches design Section G: "zero network calls".
        if (pulp::runtime::get_env("PULP_UPDATE_CHECK_DISABLED")) return;

        pulp_debug("update-banner: read_update_mode");
        auto mode = read_update_mode();
        if (mode == um::Mode::Off) return;

        auto cache_path = banner_cache_path();
        if (cache_path.empty()) return;

        pulp_debug("update-banner: read_cache_file");
        auto cache_opt = uc::read_cache_file(cache_path);
        uc::CacheEntry cache = cache_opt.value_or(uc::CacheEntry{});

        const auto now = uc::now_epoch_sec();
        const std::string installed = PULP_SDK_VERSION;
        const std::string latest = cache.latest_version;
        const bool banner_already_shown_this_cycle =
            !latest.empty() && cache.banner_shown_for_version == latest;
        const auto snooze = snooze_path();
        const bool snooze_active =
            !snooze.empty() && um::is_snooze_active(snooze, now);

        // ── Prompt mode ─────────────────────────────────────────────────────
        if (mode == um::Mode::Prompt) {
            auto decision = um::decide_prompt_banner(
                mode, installed, latest,
                banner_already_shown_this_cycle, snooze_active);
            if (decision.show_banner) {
                std::cerr << uc::compose_banner(installed, latest) << "\n";
                cache.banner_shown_for_version = latest;
                uc::write_cache_file(cache_path, cache);
            }
        }

        // ── Manual mode ─────────────────────────────────────────────────────
        //
        // Per design Section A, manual mode still surfaces the fact
        // that a newer release is available — just never prompts for
        // action. One-liner per new version (tracked via the same
        // banner_shown_for_version field so we don't re-nag on every
        // invocation). Snooze does not apply — manual mode is already
        // the "quiet" mode; adding another silence layer would make
        // the notice invisible.
        if (mode == um::Mode::Manual &&
            !latest.empty() &&
            uc::is_newer(installed, latest) &&
            cache.banner_shown_for_version != latest) {
            std::cerr << um::compose_manual_notice(installed, latest) << "\n";
            cache.banner_shown_for_version = latest;
            uc::write_cache_file(cache_path, cache);
        }

        // ── Auto mode ───────────────────────────────────────────────────────
        //
        // If a newer release is known and we haven't already staged
        // it, drop the pending-upgrade marker + print a one-liner.
        // cmd_upgrade is responsible for the actual swap on the next
        // invocation. We never replace the binary mid-command — the
        // design explicitly forbids that (Section G).
        if (mode == um::Mode::Auto && !latest.empty() &&
            uc::is_newer(installed, latest)) {
            auto marker = pending_upgrade_path();
            auto existing = marker.empty()
                                ? std::optional<um::PendingUpgrade>{}
                                : um::read_pending_upgrade(marker);
            if (um::should_stage_auto_download(mode, installed, latest, existing)) {
                // Only print the "downloaded / will complete next
                // invocation" banner and record it in the cache if the
                // marker actually landed. If PULP_HOME is read-only,
                // full, or otherwise unwritable, suppress the notice
                // so users aren't promised a completion that cannot
                // happen (#590 Codex P2 / wave-4 sweep).
                if (kick_auto_stage(marker, latest)) {
                    std::cerr << um::compose_auto_staged_notice(latest) << "\n";
                    cache.banner_shown_for_version = latest;
                    uc::write_cache_file(cache_path, cache);
                }
            }
        }

        auto interval = read_check_interval_hours();
        if (uc::is_cache_stale(cache, now, interval)) {
            pulp_debug("update-banner: kick_background_refresh (detached)");
            kick_background_refresh(cache_path);
        }
        pulp_debug("update-banner: exit");
    } catch (...) {
        // Never let a banner bug break a user's actual command.
    }
}

}  // namespace

// ── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-color") == 0) {
            g_no_color = true;
        }
    }
    init_color();

    if (argc < 2) {
        print_usage();
        return 0;
    }

    std::string command = argv[1];
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-color") == 0) continue;
        args.push_back(argv[i]);
    }

    pulp_debug("main: update-banner enter");
    maybe_emit_update_banner_and_refresh(command);
    pulp_debug("main: dispatch");

    // Lookup in command table
    for (int i = 0; i < command_count; ++i) {
        if (command == commands[i].name) return commands[i].handler(args);
    }

    // Lookup in script commands
    for (int i = 0; i < script_command_count; ++i) {
        if (command == script_commands[i].name) return delegate_to_script(script_commands[i], args);
    }

    // Lookup in binary commands
    for (int i = 0; i < binary_command_count; ++i) {
        if (command == binary_commands[i].name) return delegate_to_binary(binary_commands[i], args);
    }

    // Package manager commands
    if (command == "add")      return pulp::cli::pkg::cmd_add(args);
    if (command == "remove")   return pulp::cli::pkg::cmd_remove(args);
    if (command == "list")     return pulp::cli::pkg::cmd_list(args);
    if (command == "search")   return pulp::cli::pkg::cmd_search(args);
    if (command == "update")   return pulp::cli::pkg::cmd_update(args);
    if (command == "suggest")  return pulp::cli::pkg::cmd_suggest(args);
    if (command == "target")   return pulp::cli::pkg::cmd_target(args);
    if (command == "audit")    return handle_audit(args);
    if (command == "tool")     return pulp::cli::tools::cmd_tool(args);

    // Legacy aliases
    if (command == "add-component") {
        return delegate_to_python_script("tools/add-component.py", args);
    }

    if (command == "install") {
        std::cout << "Installing Pulp SDK...\n";
        std::vector<std::string> cache_args = {"fetch", "skia"};
        return cmd_cache(cache_args);
    }

    // Help
    if (command == "help" || command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    // Fuzzy "did you mean?" suggestion
    std::cerr << "Unknown command: " << command << "\n";
    int best_dist = 999;
    const char* best_name = nullptr;
    auto levenshtein = [](const std::string& a, const std::string& b) -> int {
        size_t m = a.size(), n = b.size();
        std::vector<int> dp((m + 1) * (n + 1));
        for (size_t i = 0; i <= m; ++i) dp[i * (n + 1)] = static_cast<int>(i);
        for (size_t j = 0; j <= n; ++j) dp[j] = static_cast<int>(j);
        for (size_t i = 1; i <= m; ++i)
            for (size_t j = 1; j <= n; ++j) {
                int cost = (a[i-1] == b[j-1]) ? 0 : 1;
                dp[i*(n+1)+j] = std::min({dp[(i-1)*(n+1)+j]+1, dp[i*(n+1)+j-1]+1,
                                           dp[(i-1)*(n+1)+j-1]+cost});
            }
        return dp[m*(n+1)+n];
    };
    for (int i = 0; i < command_count; ++i) {
        int d = levenshtein(command, commands[i].name);
        if (d < best_dist) { best_dist = d; best_name = commands[i].name; }
    }
    if (best_dist <= 3 && best_name) {
        std::cerr << "Did you mean: pulp " << best_name << "?\n";
    } else {
        std::cerr << "Run `pulp help` for usage\n";
    }
    return 1;
}
