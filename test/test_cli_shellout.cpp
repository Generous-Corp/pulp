// Shell-out CLI behaviour tests for `pulp`.
//
// Per CLAUDE.md: "CLI behavior changes — shell out to the built binary,
// assert exit code + stderr content." Before this file, the CLI had
// two targeted unit tests (test_cli_create_targets, test_cli_design_binding)
// but no end-to-end shell-out validation — the lesson from #295 was
// that silent-failure bugs slip through when no test actually launches
// the binary. This closes that gap with deterministic exit-code +
// stdout/stderr invariants for the non-destructive subcommands every
// user hits on day one.

#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pulp::platform;
namespace fs = std::filesystem;

namespace {

// Platform-aware setenv/unsetenv: Windows MSVC doesn't provide POSIX
// `setenv` / `unsetenv` — it uses `_putenv_s` instead (empty string
// value for unset). Wrap both so every test body stays portable.
inline int pulp_setenv(const char* name, const char* value, int /*overwrite*/) {
#if defined(_WIN32)
    return _putenv_s(name, value);
#else
    return ::setenv(name, value, 1);
#endif
}

inline int pulp_unsetenv(const char* name) {
#if defined(_WIN32)
    return _putenv_s(name, "");
#else
    return ::unsetenv(name);
#endif
}


// The pulp CLI binary lands at <build>/tools/cli/pulp once `pulp-cli`
// has been built. The test runner's working directory at invocation
// time is <build>/test, so "../tools/cli/pulp" is the relative path.
// Fall back to a PULP_CLI_PATH env override for adversarial CI setups.
fs::path pulp_binary() {
    if (const char* env = std::getenv("PULP_CLI_PATH"); env && *env) {
        return fs::path(env);
    }
    return fs::current_path() / ".." / "tools" / "cli" / "pulp";
}

ProcessResult run_pulp(const std::vector<std::string>& args,
                       int timeout_ms = 10000) {
    auto bin = pulp_binary();
    if (!fs::exists(bin)) {
        // Binary not built for this target — rather than fail noisily,
        // surface a SKIP at the call site. Tests check for .ok() first.
        ProcessResult r;
        r.exit_code = -1;
        r.stderr_output = "pulp binary not at " + bin.string();
        return r;
    }
    return exec(bin.string(), args, timeout_ms);
}

bool binary_exists() { return fs::exists(pulp_binary()); }

std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("pulp help exits 0 with a usage banner on stdout",
          "[cli][shellout]") {
    if (!binary_exists()) {
        SUCCEED("pulp binary not built for this test run; skipping");
        return;
    }
    auto r = run_pulp({"help"});
    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(r.timed_out);
    // Must advertise the word "pulp" and list the build subcommand so
    // first-time users aren't dropped into an empty help output — this
    // is the category of regression that the #295 silent-failure
    // lesson warned about.
    REQUIRE(r.stdout_output.find("pulp") != std::string::npos);
    REQUIRE(r.stdout_output.find("build") != std::string::npos);
}

TEST_CASE("pulp --help is an alias for pulp help",
          "[cli][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto a = run_pulp({"help"});
    auto b = run_pulp({"--help"});
    auto c = run_pulp({"-h"});
    // Every alias exits 0 and reaches the same top-level usage.
    REQUIRE(a.exit_code == 0);
    REQUIRE(b.exit_code == 0);
    REQUIRE(c.exit_code == 0);
    REQUIRE(a.stdout_output.find("pulp") != std::string::npos);
    REQUIRE(b.stdout_output.find("pulp") != std::string::npos);
    REQUIRE(c.stdout_output.find("pulp") != std::string::npos);
}

TEST_CASE("pulp with no arguments prints help and exits 0",
          "[cli][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({});
    // Bare `pulp` is expected to show usage rather than error out — it
    // mustn't fake-succeed silently or hang.
    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE((r.stdout_output.find("pulp") != std::string::npos ||
             r.stdout_output.find("Usage") != std::string::npos ||
             r.stdout_output.find("Commands") != std::string::npos));
}

TEST_CASE("pulp <unknown-command> exits non-zero with a diagnostic",
          "[cli][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"thisisnotarealcommand"});
    REQUIRE(r.exit_code != 0);
    REQUIRE_FALSE(r.timed_out);
    // At least one of stdout/stderr must reference the unknown command
    // or a help pointer — otherwise the user is left guessing.
    auto combined = r.stdout_output + r.stderr_output;
    bool mentioned =
        combined.find("Unknown") != std::string::npos ||
        combined.find("unknown") != std::string::npos ||
        combined.find("help") != std::string::npos;
    REQUIRE(mentioned);
}

TEST_CASE("pulp config <unknown-subcommand> exits non-zero with a diagnostic",
          "[cli][shellout][codex-562]") {
    // Codex 2026-04-21 wave 2 P2 on #562: `pulp config foo` previously
    // fell through to usage() which returned 0, so scripts/CI could
    // not detect a typo'd subcommand. The new behaviour returns
    // exit code 2 with an "Unknown config subcommand" diagnostic on
    // stderr. Known subcommands (get/set/list/help) keep exit 0.
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"config", "thisisnotarealsubcommand"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    REQUIRE(r.stderr_output.find("Unknown config subcommand") != std::string::npos);
}

TEST_CASE("pulp version subcommand runs and mentions the SDK",
          "[cli][shellout][version]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"version"});
    // `pulp version` prints SDK/plugin version info and exits 0. Guard
    // against a regression where the subcommand silently exits with
    // success but an empty body (the exact shape of bug #295 for the
    // ship command).
    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(r.timed_out);
    // Must have SOMETHING on stdout — stderr alone is not enough.
    REQUIRE_FALSE(r.stdout_output.empty());
}

TEST_CASE("pulp version check exits 0 on a clean tree and mentions SDK/plugin/marketplace",
          "[cli][shellout][version]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"version", "check"});
    REQUIRE_FALSE(r.timed_out);
    // The check may exit non-zero if version files have drifted. Both
    // exit-code branches must include the three surfaces so the user
    // can act on the diagnostic.
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE((combined.find("sdk") != std::string::npos ||
             combined.find("SDK") != std::string::npos ||
             combined.find("CMakeLists.txt") != std::string::npos));
}

// pulp #709 / #468 — `pulp import-design --from claude` ingests a
// manually-exported Claude Design HTML file AND scaffolds a
// bridge_handlers.cpp next to the generated JS. This is the CLI
// surface that pulp#468's manual-export framing depends on.
TEST_CASE("pulp import-design --from claude writes JS + bridge handler scaffold",
          "[cli][shellout][import-design][issue-709][issue-468]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() / "pulp-claude-smoke";
    fs::create_directories(tmp);
    auto html_path    = tmp / "claude-export.html";
    auto js_path      = tmp / "claude-ui.js";
    auto tokens_path  = tmp / "claude-tokens.json";
    auto bridge_path  = tmp / "claude-bridge.cpp";
    {
        std::ofstream f(html_path);
        f << "<!DOCTYPE html><html><body><div class=\"container\">"
             "<h1>Hello Claude</h1><button>Click</button></div></body></html>";
    }
    // Clean stale artifacts from prior runs.
    fs::remove(js_path);
    fs::remove(tokens_path);
    fs::remove(bridge_path);

    auto r = run_pulp({"import-design",
                       "--from", "claude",
                       "--file",   html_path.string(),
                       "--output", js_path.string(),
                       "--tokens", tokens_path.string(),
                       "--bridge-output", bridge_path.string()},
                       30000);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(js_path));
    REQUIRE(fs::exists(bridge_path));

    // Scaffold must reference the framework surface by full name so
    // future readers can trace the generated file back to its
    // framework host even after they've edited the handlers.
    std::ifstream b(bridge_path);
    std::string bridge_contents((std::istreambuf_iterator<char>(b)),
                                 std::istreambuf_iterator<char>());
    REQUIRE(bridge_contents.find("pulp::view::EditorBridge") != std::string::npos);
    REQUIRE(bridge_contents.find("add_handler") != std::string::npos);
    REQUIRE(bridge_contents.find("attach_webview") != std::string::npos);

    // Stdout reports both outputs (the JS view AND the scaffold).
    const auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find("bridge handler scaffold") != std::string::npos);
}

// Paired with the above: `--no-bridge-scaffold` suppresses the
// scaffold emission while keeping the HTML → JS path intact.
TEST_CASE("pulp import-design --from claude --no-bridge-scaffold writes only the JS",
          "[cli][shellout][import-design][issue-709]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() / "pulp-claude-smoke-no-scaffold";
    fs::create_directories(tmp);
    auto html_path   = tmp / "claude-export.html";
    auto js_path     = tmp / "claude-ui.js";
    auto bridge_path = tmp / "bridge_handlers.cpp";
    {
        std::ofstream f(html_path);
        f << "<!DOCTYPE html><html><body><p>hi</p></body></html>";
    }
    fs::remove(js_path);
    fs::remove(bridge_path);

    auto r = run_pulp({"import-design",
                       "--from", "claude",
                       "--file", html_path.string(),
                       "--output", js_path.string(),
                       "--no-bridge-scaffold"},
                      30000);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(js_path));
    REQUIRE_FALSE(fs::exists(bridge_path));
}

TEST_CASE("pulp help output lists the top-level subcommands",
          "[cli][shellout][help]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"help"});
    REQUIRE(r.exit_code == 0);

    // Every subcommand that's visible to users in the ci skill / docs
    // needs to survive help output — if someone silently drops one
    // from the dispatch table, this fails loudly.
    for (const char* cmd : {"build", "test", "run", "validate", "ship",
                            "version", "doctor", "create", "clean",
                            "docs", "status"}) {
        INFO("help output missing subcommand: " << cmd);
        REQUIRE(r.stdout_output.find(cmd) != std::string::npos);
    }
}

// #51 / #356: `pulp validate --strict` is supposed to upgrade
// skipped-because-missing-tool into a hard failure.
//
// cmd_validate now parses flags up-front and rejects unknown flags
// with exit code 2 — that gives us a testable distinction between
// "known flag, can't run because no project" and "unknown flag"
// without needing a real build tree. Codex P2 on PR #381 correctly
// flagged that the prior version of this test couldn't tell the two
// apart: unknown flags were silently ignored, so the assertion would
// pass even if --strict handling were removed.
TEST_CASE("pulp validate --strict is a recognized flag",
          "[cli][shellout][validate][issue-356]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    // Run from /tmp so resolve_active_project_root() bails with
    // "not in a Pulp project directory". Flag parsing runs first, so
    // --strict passes through cleanly and we hit exit 1, not 2.
    //
    // NB: resolve the absolute binary path up front. run_pulp's
    // pulp_binary() is cwd-relative ("<cwd>/../tools/cli/pulp"), so
    // if we swap cwd to /tmp first the lookup resolves to a path
    // that doesn't exist and we never actually run the CLI.
    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(fs::temp_directory_path());
    auto strict = exec(bin.string(), {"validate", "--strict"}, 10000);
    auto bogus = exec(bin.string(), {"validate", "--this-flag-does-not-exist"}, 10000);
    fs::current_path(cwd_saver);

    REQUIRE_FALSE(strict.timed_out);
    REQUIRE_FALSE(bogus.timed_out);

    // --strict is known: flag parser accepts, we fall through to the
    // project-root bail-out, exit 1.
    REQUIRE(strict.exit_code == 1);
    REQUIRE(strict.stderr_output.find("unknown flag") == std::string::npos);

    // Unknown flag: rejected with exit 2 before the project-root check
    // even runs. If cmd_validate ever silently swallows unknown flags
    // again, this fails loudly and --strict handling is protected.
    REQUIRE(bogus.exit_code == 2);
    REQUIRE(bogus.stderr_output.find("unknown flag") != std::string::npos);
    REQUIRE(bogus.stderr_output.find("--this-flag-does-not-exist")
            != std::string::npos);
}

TEST_CASE("pulp build fails fast when standalone SDK is ahead of the installed CLI",
          "[cli][shellout][build][issue-682]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() /
               ("pulp-shellout-build-skew-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()));
    fs::create_directories(tmp);
    {
        std::ofstream f(tmp / "pulp.toml");
        f << "[pulp]\n"
          << "sdk_version = \"99.0.0\"\n"
          << "cli_min_version = \"99.0.0\"\n";
    }

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(tmp);
    auto r = exec(bin.string(), {"build"}, 10000);
    fs::current_path(cwd_saver);
    fs::remove_all(tmp);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 1);
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find("requires a newer Pulp CLI") != std::string::npos);
    REQUIRE(combined.find("pulp upgrade 99.0.0") != std::string::npos);
    REQUIRE(combined.find("pulp doctor --versions") != std::string::npos);
    REQUIRE(combined.find("--allow-unsupported-sdk") != std::string::npos);
}

TEST_CASE("pulp build allows explicit unsupported SDK bypass",
          "[cli][shellout][build][issue-682]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() /
               ("pulp-shellout-build-allow-skew-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()));
    fs::create_directories(tmp);
    {
        std::ofstream f(tmp / "pulp.toml");
        f << "[pulp]\n"
          << "sdk_version = \"99.0.0\"\n"
          << "cli_min_version = \"99.0.0\"\n";
    }

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(tmp);
    auto r = exec(bin.string(), {"build", "--allow-unsupported-sdk"}, 10000);
    fs::current_path(cwd_saver);
    fs::remove_all(tmp);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find("requires a newer Pulp CLI") == std::string::npos);
    REQUIRE(combined.find("pulp upgrade 99.0.0") == std::string::npos);
}

// #8 / #355 — `pulp doctor android` and `pulp doctor ios` are
// recognized subcommands; bogus subcommand fails with exit 2 + Usage.
TEST_CASE("pulp doctor android|ios are recognized subcommands",
          "[cli][shellout][doctor][issue-355]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    const auto bin = fs::absolute(pulp_binary());

    // Doctor walks xcode-select / xcrun / simctl on macOS and the Android
    // SDK probe on non-macOS; under CI load (github-hosted ARM64 runners
    // especially) these can stretch well past a minute. This test only
    // validates subcommand recognition, so allow extra headroom and let
    // exit-code assertions catch real parser regressions.
    constexpr int doctor_timeout_ms = 180000;
    auto android = exec(bin.string(), {"doctor", "android"}, doctor_timeout_ms);
    auto ios     = exec(bin.string(), {"doctor", "ios"},     doctor_timeout_ms);
    auto bogus   = exec(bin.string(), {"doctor", "potato"},  10000);

    REQUIRE_FALSE(android.timed_out);
    REQUIRE_FALSE(ios.timed_out);
    REQUIRE_FALSE(bogus.timed_out);

    // android + ios run the new check sets — exit 0 when the host
    // has the tooling, exit 1 when something's missing. Either is
    // fine here; we're verifying the SUBCOMMAND is recognized, not
    // that the dev host is fully provisioned. exit 2 is the
    // "unknown subcommand" failure path we're guarding against.
    REQUIRE(android.exit_code != 2);
    REQUIRE(ios.exit_code != 2);
    REQUIRE(android.stdout_output.find("Pulp Doctor") != std::string::npos);
    REQUIRE(ios.stdout_output.find("Pulp Doctor") != std::string::npos);

    // bogus subcommand: rejected at the parser with a helpful Usage line.
    REQUIRE(bogus.exit_code == 2);
    REQUIRE(bogus.stderr_output.find("unknown subcommand") != std::string::npos);
    REQUIRE(bogus.stderr_output.find("Usage:") != std::string::npos);
}

// Issue #743: `pulp doctor --validators` is a recognized flag and
// produces a per-validator OK/FAIL/WARN report. We don't assert the
// per-tool verdicts (they depend on host install state) — just that
// the parser accepts the flag, the section header renders, and the
// exit code is in the documented set (0 = all healthy, 1 = at least
// one validator missing or broken). An exit code of 2 would mean the
// flag parser rejected --validators, which is exactly the regression
// this test guards against.
TEST_CASE("pulp doctor --validators is a recognized flag",
          "[cli][shellout][doctor][issue-743]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    const auto bin = fs::absolute(pulp_binary());
    auto r = exec(bin.string(), {"doctor", "--validators"}, 30000);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 2);  // parser accepted the flag
    REQUIRE(r.stdout_output.find("Pulp Doctor") != std::string::npos);
    REQUIRE(r.stdout_output.find("Validators") != std::string::npos);

    // --fix --dry-run is the safest flag combo to exercise: no fs
    // mutation, but the heal pipeline still runs end-to-end. Also
    // verifies that --fix combined with --validators is accepted.
    auto dry = exec(bin.string(),
                    {"doctor", "--validators", "--fix", "--dry-run"}, 30000);
    REQUIRE_FALSE(dry.timed_out);
    REQUIRE(dry.exit_code != 2);
    REQUIRE(dry.stdout_output.find("Pulp Doctor") != std::string::npos);
}

TEST_CASE("pulp dev fails fast when standalone SDK is ahead of the installed CLI",
          "[cli][shellout][dev][issue-682]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() /
               ("pulp-shellout-dev-skew-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()));
    fs::create_directories(tmp);
    {
        std::ofstream f(tmp / "pulp.toml");
        f << "[pulp]\n"
          << "sdk_version = \"99.0.0\"\n"
          << "cli_min_version = \"99.0.0\"\n";
    }

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(tmp);
    auto r = exec(bin.string(), {"dev"}, 10000);
    fs::current_path(cwd_saver);
    fs::remove_all(tmp);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 1);
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find("requires a newer Pulp CLI") != std::string::npos);
    REQUIRE(combined.find("pulp upgrade 99.0.0") != std::string::npos);
    REQUIRE(combined.find("--allow-unsupported-sdk") != std::string::npos);
}

// Issue #499 Slice 1: `pulp doctor --versions` is the foundation of
// the release-discovery UX. Verify the subcommand is wired, always
// exits 0 (skew findings are advisory), and surfaces the diagnostic
// header + CLI version line on stdout. This catches the class of
// silent-failure bug described in #295: an --versions flag that is
// parsed but routes to a no-op would pass CI green without this test.
TEST_CASE("pulp doctor --versions prints diagnostics and exits 0",
          "[cli][shellout][doctor][issue-499]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto r = run_pulp({"doctor", "--versions"}, 30000);
    REQUIRE_FALSE(r.timed_out);
    // Exit code is always 0 — skew is advisory, not a hard failure.
    REQUIRE(r.exit_code == 0);
    // The report header must be on stdout so scripts can distinguish
    // this output from the default `pulp doctor` pipeline.
    REQUIRE(r.stdout_output.find("Pulp Version Diagnostics") != std::string::npos);
    // We always print the CLI line, even when no project is active.
    REQUIRE(r.stdout_output.find("CLI:") != std::string::npos);
}

// Issue #548 Slice 3: `pulp upgrade --notes` prints migration notes
// for the hop without downloading anything. `--from`/`--to` overrides
// let us exercise the filter deterministically regardless of the
// installed CLI version on the test host.
TEST_CASE("pulp upgrade --notes --from A --to B prints migration header",
          "[cli][shellout][upgrade][issue-548]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    // Use an env override to neutralise the network-facing update
    // banner so its stderr noise doesn't interfere.
    auto r = run_pulp({"upgrade", "--notes", "--from", "0.23.0", "--to", "0.29.0"}, 15000);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("Pulp migration notes: 0.23.0 -> 0.29.0")
            != std::string::npos);
    // At least one of the seeded migration docs must surface between
    // 0.23.0 and 0.29.0 — otherwise the embedded index was not
    // regenerated from docs/migrations/.
    REQUIRE(r.stdout_output.find("v0.") != std::string::npos);
}

TEST_CASE("pulp upgrade --notes --json emits stable-shape JSON keys",
          "[cli][shellout][upgrade][issue-548]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto r = run_pulp({"upgrade", "--notes", "--json",
                       "--from", "0.23.0", "--to", "0.29.0"}, 15000);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    // Slice 4 depends on these keys — they are a stable public surface.
    REQUIRE(r.stdout_output.find("\"from\":")       != std::string::npos);
    REQUIRE(r.stdout_output.find("\"to\":")         != std::string::npos);
    REQUIRE(r.stdout_output.find("\"entries\":")    != std::string::npos);
    REQUIRE(r.stdout_output.find("\"version\":")    != std::string::npos);
    REQUIRE(r.stdout_output.find("\"breaking\":")   != std::string::npos);
    REQUIRE(r.stdout_output.find("\"summary\":")    != std::string::npos);
    REQUIRE(r.stdout_output.find("\"applies_if\":") != std::string::npos);
    REQUIRE(r.stdout_output.find("\"body\":")       != std::string::npos);
}

TEST_CASE("pulp upgrade --check-only honors disabled update checks with an empty cache",
          "[cli][shellout][upgrade][codecov]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() /
               ("pulp-shellout-upgrade-disabled-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()));
    fs::create_directories(tmp);

    pulp_setenv("PULP_HOME", tmp.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);
    auto r = run_pulp({"upgrade", "--check-only"}, 10000);
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    pulp_unsetenv("PULP_HOME");
    fs::remove_all(tmp);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("Installed:  v") != std::string::npos);
    REQUIRE(r.stdout_output.find("Latest:     update check disabled; not queried")
            != std::string::npos);
    REQUIRE(r.stdout_output.find("Cache empty; querying GitHub Releases")
            == std::string::npos);
}

// Issue #550 Slice 5: `update.mode = off` must produce zero network
// traffic and zero banner output. We can't directly observe the
// network inside ctest, but we CAN verify that (a) the command
// finishes well under the anonymous-GitHub round-trip latency (caching
// the cache is fine; actually dialing the API is not), and (b) no
// update banner appears on stderr. The banner hook's early-return on
// `Mode::Off` is what this test guards.
//
// We use PULP_HOME to point the CLI at a scratch config dir seeded
// with `update.mode = "off"` so we don't mutate the developer's real
// ~/.pulp. The test creates the config file directly rather than
// shelling out to `pulp config set` so we can reason about the
// sequence (`pulp config set` itself clears the snooze file — we want
// to isolate the dispatch-path behaviour).
TEST_CASE("pulp with update.mode=off never prints a banner",
          "[cli][shellout][update-mode][issue-550]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() /
               ("pulp-shellout-mode-off-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()));
    fs::create_directories(tmp);
    {
        std::ofstream cfg(tmp / "config.toml");
        cfg << "[update]\nmode = \"off\"\n";
    }
    // Seed the cache with a spurious "newer version available" entry.
    // If the off-mode short-circuit regressed, this is what would
    // leak into the banner.
    {
        std::ofstream cache(tmp / "update-cache.json");
        cache << "{\n"
              << "  \"schema\": 1,\n"
              << "  \"last_check_epoch_sec\": 1713638400,\n"
              << "  \"latest_version\": \"99.99.99\",\n"
              << "  \"release_notes_url\": \"https://example.invalid/\",\n"
              << "  \"banner_shown_for_version\": \"\"\n"
              << "}\n";
    }

    const auto bin = fs::absolute(pulp_binary());
#ifdef _WIN32
    _putenv_s("PULP_HOME", tmp.string().c_str());
#else
    pulp_setenv("PULP_HOME", tmp.string().c_str(), 1);
#endif
    auto r = exec(bin.string(), {"help"}, 10000);
#ifdef _WIN32
    _putenv_s("PULP_HOME", "");
#else
    pulp_unsetenv("PULP_HOME");
#endif
    fs::remove_all(tmp);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    // The seeded cache has latest=99.99.99 which would trigger the
    // prompt/manual/auto banners. off-mode must suppress ALL of them.
    REQUIRE(r.stderr_output.find("99.99.99") == std::string::npos);
    REQUIRE(r.stderr_output.find("available") == std::string::npos);
    REQUIRE(r.stderr_output.find("downloaded") == std::string::npos);
}

// Issue #550 Slice 5: `update.mode = manual` prints the one-liner
// once per version. The banner shape is locked — it must differ from
// the prompt-mode banner so users (and shell scripts) can tell them
// apart. Same PULP_HOME scratch-dir trick as the off-mode test.
TEST_CASE("pulp with update.mode=manual prints the manual notice",
          "[cli][shellout][update-mode][issue-550]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() /
               ("pulp-shellout-mode-manual-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()));
    fs::create_directories(tmp);
    {
        std::ofstream cfg(tmp / "config.toml");
        cfg << "[update]\nmode = \"manual\"\n";
    }
    {
        std::ofstream cache(tmp / "update-cache.json");
        cache << "{\n"
              << "  \"schema\": 1,\n"
              << "  \"last_check_epoch_sec\": 1713638400,\n"
              << "  \"latest_version\": \"99.99.99\",\n"
              << "  \"release_notes_url\": \"https://example.invalid/\",\n"
              << "  \"banner_shown_for_version\": \"\"\n"
              << "}\n";
    }

    const auto bin = fs::absolute(pulp_binary());
#ifdef _WIN32
    _putenv_s("PULP_HOME", tmp.string().c_str());
#else
    pulp_setenv("PULP_HOME", tmp.string().c_str(), 1);
#endif
    // Use `doctor --versions` — it's not on the banner_blocked list,
    // so the dispatch hook runs for it. `help` IS blocked.
    auto r = exec(bin.string(), {"doctor", "--versions"}, 30000);
#ifdef _WIN32
    _putenv_s("PULP_HOME", "");
#else
    pulp_unsetenv("PULP_HOME");
#endif
    fs::remove_all(tmp);

    REQUIRE_FALSE(r.timed_out);
    // Manual mode banner shape: "Run `pulp upgrade` when you're ready."
    REQUIRE(r.stderr_output.find("when you're ready") != std::string::npos);
    REQUIRE(r.stderr_output.find("99.99.99") != std::string::npos);
}

// Issue #564 Slice 7: `pulp project bump --help` must be wired at the
// dispatch level. Any future regression where the `project` command
// falls out of the dispatch table fails loudly here — same class of
// silent-failure bug that motivated the rest of this file.
TEST_CASE("pulp project is a recognized command with bump + undo subcommands",
          "[cli][shellout][issue-564]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto help = run_pulp({"project", "--help"}, 10000);
    REQUIRE_FALSE(help.timed_out);
    REQUIRE(help.exit_code == 0);
    REQUIRE(help.stdout_output.find("project bump") != std::string::npos);
    REQUIRE(help.stdout_output.find("project undo") != std::string::npos);

    auto bump_help = run_pulp({"project", "bump", "--help"}, 10000);
    REQUIRE_FALSE(bump_help.timed_out);
    REQUIRE(bump_help.exit_code == 0);
    REQUIRE(bump_help.stdout_output.find("--all") != std::string::npos);
    REQUIRE(bump_help.stdout_output.find("--dry-run") != std::string::npos);
    REQUIRE(bump_help.stdout_output.find("--force-dirty") != std::string::npos);
    REQUIRE(bump_help.stdout_output.find("--allow-downgrade") != std::string::npos);
    REQUIRE(bump_help.stdout_output.find("--allow-cli-skew") != std::string::npos);
    REQUIRE(bump_help.stdout_output.find("--allow-redundant") != std::string::npos);
    REQUIRE(bump_help.stdout_output.find("--verify-builds") != std::string::npos);
    REQUIRE(bump_help.stdout_output.find("pulp.toml sdk_version") != std::string::npos);

    auto undo_help = run_pulp({"project", "undo", "--help"}, 10000);
    REQUIRE_FALSE(undo_help.timed_out);
    REQUIRE(undo_help.exit_code == 0);
    REQUIRE(undo_help.stdout_output.find("Revert") != std::string::npos);

    auto bogus = run_pulp({"project", "potato"}, 10000);
    REQUIRE_FALSE(bogus.timed_out);
    REQUIRE(bogus.exit_code != 0);
    REQUIRE(bogus.stderr_output.find("unknown subcommand") != std::string::npos);
}

// Issue #564 Slice 7: `pulp project bump --dry-run` rejects an
// invalid --to value with a diagnostic. No writes happen.
TEST_CASE("pulp project bump rejects non-semver --to",
          "[cli][shellout][issue-564]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    const auto bin = fs::absolute(pulp_binary());
    // Run from /tmp so there's no CMakeLists.txt — but we expect the
    // semver validation to fire BEFORE any filesystem touch anyway.
    auto cwd_saver = fs::current_path();
    fs::current_path(fs::temp_directory_path());
    auto r = exec(bin.string(), {"project", "bump", "--to=not-a-version", "--dry-run"}, 10000);
    fs::current_path(cwd_saver);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    REQUIRE(r.stderr_output.find("invalid target version") != std::string::npos);
}

TEST_CASE("pulp project bump updates standalone SDK pins and undo reverts them",
          "[cli][shellout][project-bump][issue-244]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto base = fs::temp_directory_path() /
                ("pulp-shellout-project-bump-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
    auto project = base / "Clock";
    auto home = base / "home";
    fs::create_directories(project);
    fs::create_directories(home);
    {
        std::ofstream cfg(home / "config.toml");
        cfg << "[update]\nmode = \"off\"\n";
    }
    {
        std::ofstream cmake(project / "CMakeLists.txt");
        cmake << "cmake_minimum_required(VERSION 3.20)\n"
              << "project(Clock VERSION 1.0.0 LANGUAGES CXX)\n"
              << "find_package(Pulp 0.1.0 REQUIRED)\n";
    }
    {
        std::ofstream toml(project / "pulp.toml");
        toml << "[pulp]\n"
             << "sdk_version = \"0.1.0\"\n"
             << "sdk_path = \"/custom/sdk\"\n";
    }

    const auto bin = fs::absolute(pulp_binary());
    const auto saved_cwd = fs::current_path();
    pulp_setenv("PULP_HOME", home.string().c_str(), 1);
    fs::current_path(project);

    auto bump = exec(bin.string(), {"project", "bump", "--to=0.2.0"}, 10000);
    std::string cmake_after;
    std::string toml_after;
    ProcessResult undo;
    if (!bump.timed_out && bump.exit_code == 0) {
        cmake_after = read_file(project / "CMakeLists.txt");
        toml_after = read_file(project / "pulp.toml");
        undo = exec(bin.string(), {"project", "undo"}, 10000);
    }
    fs::current_path(saved_cwd);
    pulp_unsetenv("PULP_HOME");

    REQUIRE_FALSE(bump.timed_out);
    REQUIRE(bump.exit_code == 0);
    REQUIRE(bump.stdout_output.find("bumped") != std::string::npos);
    REQUIRE(bump.stdout_output.find("custom sdk_path left unchanged") != std::string::npos);

    REQUIRE(cmake_after.find("project(Clock VERSION 1.0.0") != std::string::npos);
    REQUIRE(cmake_after.find("find_package(Pulp 0.2.0 REQUIRED)") != std::string::npos);
    REQUIRE(toml_after.find("sdk_version = \"0.2.0\"") != std::string::npos);
    REQUIRE(toml_after.find("sdk_path = \"/custom/sdk\"") != std::string::npos);

    REQUIRE_FALSE(undo.timed_out);
    REQUIRE(undo.exit_code == 0);
    REQUIRE(undo.stdout_output.find("reverted") != std::string::npos);
    REQUIRE(read_file(project / "CMakeLists.txt").find("find_package(Pulp 0.1.0 REQUIRED)")
            != std::string::npos);
    REQUIRE(read_file(project / "pulp.toml").find("sdk_version = \"0.1.0\"")
            != std::string::npos);

    fs::remove_all(base);
}

TEST_CASE("pulp upgrade --notes with no hop prints the empty-notes line",
          "[cli][shellout][upgrade][issue-548]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    // from == to → degenerate hop with zero applicable notes.
    auto r = run_pulp({"upgrade", "--notes", "--from", "0.29.0", "--to", "0.29.0"}, 15000);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("No migration notes apply") != std::string::npos);
}

// Issue #549 Slice 4: the `/upgrade` Claude Code slash command shells out
// to `pulp upgrade --notes --json` and parses the resulting document.
// The slash command hardcodes the JSON key names as part of its parsing
// step (.claude/commands/upgrade.md) — if the CLI renames any of them
// without updating the skill, the slash command silently falls back to
// empty context. Guard the exact keys the slash command reads.
//
// Separate from the existing issue-548 JSON test because Slice 4 also
// cares about (a) the document being parseable even when entries is
// empty and (b) every key the .claude/commands/upgrade.md parsing step
// relies on — `summary`, `body`, `applies_if` included.
TEST_CASE("pulp upgrade --notes --json is slash-command-parseable",
          "[cli][shellout][upgrade][issue-549]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    // Real hop over the seeded migration docs — we expect >=1 entry so
    // the JSON document isn't a degenerate "entries": [] case.
    auto hop = run_pulp({"upgrade", "--notes", "--json",
                        "--from", "0.23.0", "--to", "0.29.0"}, 15000);
    REQUIRE_FALSE(hop.timed_out);
    REQUIRE(hop.exit_code == 0);

    // Every key the `/upgrade` slash command reads must be present. The
    // slash command's parsing step in .claude/commands/upgrade.md
    // treats these as a stable public surface.
    for (const char* key : {"\"from\":", "\"to\":", "\"entries\":",
                            "\"version\":", "\"breaking\":",
                            "\"summary\":", "\"applies_if\":",
                            "\"body\":"}) {
        INFO("missing slash-command-required key: " << key);
        REQUIRE(hop.stdout_output.find(key) != std::string::npos);
    }

    // Degenerate hop: the slash command still needs to see a
    // well-formed document so its JSON parser doesn't choke on empty
    // output — `entries: []` is the contract, not an empty string.
    auto noop = run_pulp({"upgrade", "--notes", "--json",
                         "--from", "0.29.0", "--to", "0.29.0"}, 15000);
    REQUIRE_FALSE(noop.timed_out);
    REQUIRE(noop.exit_code == 0);
    REQUIRE(noop.stdout_output.find("\"from\":")    != std::string::npos);
    REQUIRE(noop.stdout_output.find("\"to\":")      != std::string::npos);
    REQUIRE(noop.stdout_output.find("\"entries\":") != std::string::npos);
}

// #591 Codex P2 / wave-4 sweep: `pulp docs build-site` must resolve
// `mkdocs.yml` from the project root, not the caller's CWD. This test
// invokes the CLI from inside a subdirectory (`tools/`) and asserts
// that the composed mkdocs command references the repo-root config —
// the previous bare `mkdocs build` would have failed with a
// "Config file 'mkdocs.yml' does not exist" error here.
//
// We don't require mkdocs to be installed: if it's missing, run exits
// non-zero with a "pip install -r requirements-docs.txt" hint on
// stderr, which is still a valid pass for this regression because the
// failure mode is "mkdocs not found", not "config file not found".
// What we must NOT see is a "Config file 'mkdocs.yml' does not exist"
// error, because that would mean `-f <root>/mkdocs.yml` wasn't passed.
TEST_CASE("pulp docs build-site resolves mkdocs.yml from project root",
          "[cli][shellout][docs][issue-591]") {
    if (!binary_exists()) {
        SUCCEED("pulp binary not built for this test run; skipping");
        return;
    }

    // Walk up from the test CWD (<build>/test) to the repo root, then
    // drop into tools/ — a real subdirectory that exists in every
    // Pulp checkout. This mirrors the "user runs `pulp docs
    // build-site` from inside tools/" scenario Codex flagged.
    fs::path repo_root = fs::current_path() / ".." / "..";
    repo_root = fs::weakly_canonical(repo_root);
    if (!fs::exists(repo_root / "mkdocs.yml")) {
        SUCCEED("mkdocs.yml not at expected repo root — likely non-standard "
                "build layout; skipping");
        return;
    }
    fs::path subdir = repo_root / "tools";
    REQUIRE(fs::exists(subdir));

    // Pulp #597: pulp_binary() resolves from current_path() / "../tools/cli/pulp"
    // — that assumes the test is run from <build>/test. When we change
    // cwd below to <repo>/tools, the relative resolution points at
    // <repo>/tools/cli/pulp (source, not built), which doesn't exist,
    // and run_pulp silently returns exit_code=-1 without launching the
    // CLI — making the regression test vacuous. Resolve the absolute
    // binary path BEFORE the cwd change and pipe it through PULP_CLI_PATH,
    // which pulp_binary() honors as an explicit override.
    fs::path absolute_pulp_bin = fs::absolute(pulp_binary());
    REQUIRE(fs::exists(absolute_pulp_bin));
    // fs::path::c_str() returns const wchar_t* on Windows, so stringify
    // explicitly so the pulp_setenv(const char*, const char*, int) overload
    // resolves on every platform.
    const std::string absolute_pulp_bin_str = absolute_pulp_bin.string();
    pulp_setenv("PULP_CLI_PATH", absolute_pulp_bin_str.c_str(), 1);

    fs::path saved_cwd = fs::current_path();
    std::error_code ec;
    fs::current_path(subdir, ec);
    REQUIRE_FALSE(ec);

    // --strict is safe: if mkdocs runs at all, the build either
    // succeeds or surfaces a warnings-as-errors exit; neither produces
    // the "Config file 'mkdocs.yml' does not exist" string.
    auto r = run_pulp({"docs", "build-site", "--site-dir",
                      (repo_root / "build" / "site-shellout-test").string()},
                     60000);

    fs::current_path(saved_cwd, ec);  // always restore
    pulp_unsetenv("PULP_CLI_PATH");

    REQUIRE_FALSE(r.timed_out);
    // Permit mkdocs-missing ("command not found" path, rc != 0 with
    // install hint on stderr) but NOT "Config file 'mkdocs.yml' does
    // not exist" — that would mean the `-f <root>/mkdocs.yml` fix
    // regressed.
    const std::string combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find("Config file 'mkdocs.yml' does not exist")
            == std::string::npos);
    REQUIRE(combined.find("does not exist; use --config-file")
            == std::string::npos);
}

TEST_CASE("pulp docs covers local reader index, search, open, and show paths",
          "[cli][shellout][docs][issue-643]") {
    if (!binary_exists()) {
        SUCCEED("pulp binary not built for this test run; skipping");
        return;
    }

    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    SECTION("usage lists the local docs reader subcommands") {
        auto r = run_pulp({"docs"});
        REQUIRE(r.exit_code == 0);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.stdout_output.find("pulp docs") != std::string::npos);
        REQUIRE(r.stdout_output.find("show command") != std::string::npos);
        REQUIRE(r.stdout_output.find("build-site") != std::string::npos);
    }

    SECTION("index and open resolve docs-index slugs") {
        auto index = run_pulp({"docs", "index"});
        REQUIRE(index.exit_code == 0);
        REQUIRE_FALSE(index.timed_out);
        REQUIRE(index.stdout_output.find("Available Documentation") != std::string::npos);
        REQUIRE(index.stdout_output.find("getting-started") != std::string::npos);
        REQUIRE(index.stdout_output.find("docs/guides/getting-started.md") != std::string::npos);

        auto opened = run_pulp({"docs", "open", "getting-started"});
        REQUIRE(opened.exit_code == 0);
        REQUIRE_FALSE(opened.timed_out);
        REQUIRE(opened.stdout_output.find("Getting Started") != std::string::npos);
    }

    SECTION("search reports exact doc matches") {
        auto r = run_pulp({"docs", "search", "WebView"});
        REQUIRE(r.exit_code == 0);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE((r.stdout_output.find("match(es) found") != std::string::npos ||
                 r.stdout_output.find("docs/") != std::string::npos));
    }

    SECTION("show reads support, command, cmake, and style manifests") {
        auto support = run_pulp({"docs", "show", "support", "vst3"});
        REQUIRE(support.exit_code == 0);
        REQUIRE_FALSE(support.timed_out);
        REQUIRE(support.stdout_output.find("Support info") != std::string::npos);
        REQUIRE(support.stdout_output.find("vst3") != std::string::npos);

        auto command = run_pulp({"docs", "show", "command", "ship"});
        REQUIRE(command.exit_code == 0);
        REQUIRE_FALSE(command.timed_out);
        REQUIRE(command.stdout_output.find("Command: ship") != std::string::npos);
        REQUIRE(command.stdout_output.find("Subcommands") != std::string::npos);
        REQUIRE(command.stdout_output.find("package") != std::string::npos);

        auto cmake = run_pulp({"docs", "show", "cmake", "pulp_add_plugin"});
        REQUIRE(cmake.exit_code == 0);
        REQUIRE_FALSE(cmake.timed_out);
        REQUIRE(cmake.stdout_output.find("CMake function: pulp_add_plugin") != std::string::npos);

        auto style = run_pulp({"docs", "show", "style"});
        REQUIRE(style.exit_code == 0);
        REQUIRE_FALSE(style.timed_out);
        REQUIRE(style.stdout_output.find("Style Rules") != std::string::npos);
        REQUIRE(style.stdout_output.find("public_headers_only") != std::string::npos);
    }

    SECTION("reader errors include actionable diagnostics") {
        auto search_usage = run_pulp({"docs", "search"});
        REQUIRE(search_usage.exit_code != 0);
        REQUIRE(search_usage.stderr_output.find("Usage: pulp docs search") != std::string::npos);

        auto missing_slug = run_pulp({"docs", "open", "not-a-real-doc"});
        REQUIRE(missing_slug.exit_code != 0);
        REQUIRE(missing_slug.stderr_output.find("no doc found") != std::string::npos);

        auto unknown_show = run_pulp({"docs", "show", "widget"});
        REQUIRE(unknown_show.exit_code != 0);
        REQUIRE(unknown_show.stderr_output.find("Unknown show topic") != std::string::npos);
    }

    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
}

// #682 — PULP_DEBUG=1 must emit timestamped phase markers to stderr so
// future "pulp hung at 0% CPU" reports pin themselves. Unset by default
// it must stay silent so scripts that parse stderr aren't affected.
TEST_CASE("PULP_DEBUG=1 surfaces phase markers to stderr (#682)",
          "[cli][shellout][issue-682]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    // Disable the update-check network path so the test stays offline
    // and deterministic — we're testing the instrumentation hook, not
    // the network dependency.
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    SECTION("unset: stderr stays free of phase markers") {
        pulp_unsetenv("PULP_DEBUG");
        auto r = run_pulp({"version"});
        REQUIRE(r.exit_code == 0);
        REQUIRE(r.stderr_output.find("[pulp-debug") == std::string::npos);
    }

    SECTION("PULP_DEBUG=1: emits markers on stderr, not stdout") {
        pulp_setenv("PULP_DEBUG", "1", 1);
        auto r = run_pulp({"version"});
        REQUIRE(r.exit_code == 0);
        REQUIRE(r.stderr_output.find("[pulp-debug") != std::string::npos);
        REQUIRE(r.stderr_output.find("update-banner") != std::string::npos);
        REQUIRE(r.stdout_output.find("[pulp-debug") == std::string::npos);
        pulp_unsetenv("PULP_DEBUG");
    }

    SECTION("PULP_DEBUG=0: treated as off, no markers") {
        pulp_setenv("PULP_DEBUG", "0", 1);
        auto r = run_pulp({"version"});
        REQUIRE(r.exit_code == 0);
        REQUIRE(r.stderr_output.find("[pulp-debug") == std::string::npos);
        pulp_unsetenv("PULP_DEBUG");
    }

    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
}
