// pulp — CLI tool for the Pulp audio plugin framework
// Command dispatch via structured table. See cmd_*.cpp for implementations.

#include "cli_common.hpp"
#include "package_commands.hpp"
#include "package_registry.hpp"
#include "tool_registry.hpp"
#include "update_check.hpp"

#include <atomic>
#include <cstring>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
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
    {"cache",    "Manage SDK and asset cache",            cmd_cache},
    {"audio",    "Repo-level audio model and bundle tooling", cmd_audio},
    {"sdk",      "Manage the Pulp SDK installation",      cmd_sdk},
    {"upgrade",  "Update the CLI to the latest version",  cmd_upgrade},
    {"version",  "Show, bump, or check version info",     cmd_version},
    {"dev",      "Unified dev loop: watch, build, test, run", cmd_dev},
    {"scan",     "Scan system paths for VST3 / AU / CLAP / LV2 plug-ins", cmd_scan},
    {"host",     "Load a plug-in and run a synthetic audio block through it", cmd_host},
    {"pr",       "One-shot push-a-PR: gates + bump + ship",   cmd_pr},
    {"config",   "Read or write ~/.pulp/config.toml settings", cmd_config},
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
    {"inspect",       "tools/screenshot/pulp-screenshot",          "Launch the component inspector", "--demo"},
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

// ── Update-check dispatch (Slice 2 of #499, issue #547) ────────────────────
//
// Before we dispatch the user's command, we optionally:
//   1. Read the cached latest-release JSON (~/.pulp/update-cache.json).
//   2. Emit a single-line banner on stderr if `update.mode != off` and
//      the cached latest_version is strictly newer than the installed
//      PULP_SDK_VERSION, and we haven't already shown the banner for
//      this version (tracked by banner_shown_for_version).
//   3. Kick off a non-blocking background refresh if the cache is
//      older than `update.check_interval_hours` (default 24).
//
// Slice 2 scope ends at "print the banner and refresh the cache". The
// full auto/prompt/manual/off enforcement (interactive y/N, snooze,
// pending-upgrade markers) lives in Slice 5. That's why `prompt` mode
// today prints a one-shot informational banner — the interactive
// prompt comes later so we can ship this without re-tooling the
// whole dispatch path.
//
// Commands that should NEVER emit the banner (so they stay
// machine-parseable for scripts) are listed in `banner_blocked_commands`
// below. `config` is on that list so `pulp config get update.mode` has
// clean stdout.

namespace {

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
// design doc Section A. "off" short-circuits the whole update path.
std::string read_update_mode() {
    auto v = read_user_config_value("update", "mode");
    if (v.empty()) return "prompt";
    return v;
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

fs::path banner_cache_path() {
    auto home = pulp_home();
    if (home.empty()) return {};
    return home / "update-cache.json";
}

// Fire a best-effort background refresh. We deliberately don't block
// on the result — the banner will pick it up on the *next* invocation.
// `std::async` with `std::launch::async` runs detached; we leak the
// future intentionally so the thread survives `main` returning. (In
// practice the network call completes in < 2s; the OS cleans up on
// exit either way.)
//
// Race-window note: the main thread may write the cache after the
// banner check (to stamp banner_shown_for_version). To avoid clobbering
// that write, the background thread re-reads the cache from disk right
// before writing. This keeps `banner_shown_for_version` set even if
// the refresh finishes later than the banner-write.
void kick_background_refresh(const fs::path& cache_path) {
    if (cache_path.empty()) return;
    static std::future<void> s_refresh_future;
    s_refresh_future = std::async(std::launch::async, [cache_path]() {
        pulp::cli::update_check::GitHubReleasesFetcher fetcher;
        auto r = fetcher.fetch_latest_release(PULP_GITHUB_REPO);
        // Re-read from disk — this is the race-avoidance point.
        auto latest = pulp::cli::update_check::read_cache_file(cache_path)
                          .value_or(pulp::cli::update_check::CacheEntry{});
        latest.schema = pulp::cli::update_check::kCacheSchemaVersion;
        latest.last_check_epoch_sec = pulp::cli::update_check::now_epoch_sec();
        if (r.ok) {
            latest.latest_version = r.latest_version;
            latest.release_notes_url = r.release_notes_url;
        }
        pulp::cli::update_check::write_cache_file(cache_path, latest);
    });
}

// Top-level hook invoked before dispatching the user's command. Best
// effort — any exception is swallowed so a cache-read bug can never
// fail the real command.
void maybe_emit_update_banner_and_refresh(const std::string& command) {
    try {
        if (banner_blocked(command)) return;

        // Explicit off-switch that bypasses config entirely. Used by
        // CI / air-gapped envs and by the unit-test shell-out lane.
        if (pulp::runtime::get_env("PULP_UPDATE_CHECK_DISABLED")) return;

        auto mode = read_update_mode();
        if (mode == "off") return;

        auto cache_path = banner_cache_path();
        if (cache_path.empty()) return;

        auto cache_opt = pulp::cli::update_check::read_cache_file(cache_path);
        pulp::cli::update_check::CacheEntry cache =
            cache_opt.value_or(pulp::cli::update_check::CacheEntry{});

        if (mode == "prompt" &&
            !cache.latest_version.empty() &&
            pulp::cli::update_check::is_newer(PULP_SDK_VERSION, cache.latest_version) &&
            cache.banner_shown_for_version != cache.latest_version) {
            std::cerr << pulp::cli::update_check::compose_banner(
                             PULP_SDK_VERSION, cache.latest_version)
                      << "\n";
            // Mark so we don't nag the user every invocation. The
            // banner reprints only when a newer release arrives and
            // resets banner_shown_for_version via the refresh below.
            cache.banner_shown_for_version = cache.latest_version;
            pulp::cli::update_check::write_cache_file(cache_path, cache);
        }
        // `manual` mode: no banner today. Slice 5 will surface it
        // differently (print on --help, not on every invocation).

        auto interval = read_check_interval_hours();
        if (pulp::cli::update_check::is_cache_stale(
                cache, pulp::cli::update_check::now_epoch_sec(), interval)) {
            kick_background_refresh(cache_path);
        }
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

    maybe_emit_update_banner_and_refresh(command);

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
