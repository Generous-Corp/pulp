// test_cli_shellout_lifecycle.cpp — extracted from test_cli_shellout.cpp
// in the 2026-05 Phase 5 P5-4 follow-up refactor.
//
// CLI lifecycle-command shell-out tests — pulp doctor, pulp dev,
// pulp design, pulp sdk, pulp upgrade. All five commands sit in the
// "lifecycle / diagnostics" surface of the CLI: they help users
// inspect and maintain their dev environment rather than building or
// shipping a plugin. Bundling them keeps the parent TU under the
// < 2,000-line P5-4 target while preserving their shared shell-out
// contract (PULP_HOME tmpdir + binary_exists guard + ProcessResult
// stdout/exit_code assertions).

#include "test_cli_shellout_helpers.hpp"

using namespace pulp::platform;
namespace fs = std::filesystem;
using namespace pulp_test_cli;

TEST_CASE("pulp doctor android|ios are recognized subcommands",
          "[cli][shellout][doctor][issue-355]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    const auto bin = fs::absolute(pulp_binary());

    // Use --versions so the parser still has to accept the mobile
    // subcommand before the diagnostic short-circuit, without running the
    // slow host SDK probes that can hang on saturated CI runners.
    auto android = exec(bin.string(), {"doctor", "android", "--versions"}, 10000);
    auto ios     = exec(bin.string(), {"doctor", "ios", "--versions"},     10000);
    auto bogus   = exec(bin.string(), {"doctor", "potato", "--versions"},  10000);
    auto missing_only = exec(bin.string(), {"doctor", "--only"}, 10000);
    auto flag_only = exec(bin.string(), {"doctor", "--only", "--versions"}, 10000);
    auto extra_positional = exec(bin.string(), {"doctor", "android", "extra"}, 10000);

    REQUIRE_FALSE(android.timed_out);
    REQUIRE_FALSE(ios.timed_out);
    REQUIRE_FALSE(bogus.timed_out);
    REQUIRE_FALSE(missing_only.timed_out);
    REQUIRE_FALSE(flag_only.timed_out);
    REQUIRE_FALSE(extra_positional.timed_out);

    REQUIRE(android.exit_code == 0);
    REQUIRE(ios.exit_code == 0);
    REQUIRE(android.stdout_output.find("Pulp Version Diagnostics") != std::string::npos);
    REQUIRE(ios.stdout_output.find("Pulp Version Diagnostics") != std::string::npos);

    // bogus subcommand: rejected at the parser with a helpful Usage line.
    REQUIRE(bogus.exit_code == 2);
    REQUIRE(bogus.stderr_output.find("unknown subcommand") != std::string::npos);
    REQUIRE(bogus.stderr_output.find("Usage:") != std::string::npos);

    REQUIRE(missing_only.exit_code == 2);
    REQUIRE(missing_only.stderr_output.find("--only requires a value") !=
            std::string::npos);
    REQUIRE(flag_only.exit_code == 2);
    REQUIRE(flag_only.stderr_output.find("--only requires a value") != std::string::npos);
    REQUIRE(extra_positional.exit_code == 2);
    REQUIRE(extra_positional.stderr_output.find("unexpected argument") !=
            std::string::npos);
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

TEST_CASE("pulp design validates owned option values before autobind",
          "[cli][shellout][design][coverage][phase3]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    struct Case {
        std::vector<std::string> args;
        std::string needle;
    };
    const std::vector<Case> cases = {
        {{"design", "--build-dir"}, "--build-dir requires a value"},
        {{"design", "--build-dir", "--script", "tool.js"}, "--build-dir requires a value"},
        {{"design", "--script"}, "--script requires a value"},
        {{"design", "--script", "--watch"}, "--script requires a value"},
    };

    for (const auto& c : cases) {
        auto r = run_pulp(c.args, 10000);
        INFO("stderr: " << r.stderr_output);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 2);
        REQUIRE(r.stderr_output.find(c.needle) != std::string::npos);
        REQUIRE(r.stderr_output.find("Cannot infer design binding") == std::string::npos);
    }
}

TEST_CASE("pulp dev validates value options before build or watch",
          "[cli][shellout][dev][coverage][phase3]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = fs::temp_directory_path() /
               ("pulp-shellout-dev-parser-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()));
    fs::create_directories(tmp / "build");
    {
        std::ofstream f(tmp / "pulp.toml");
        f << "[pulp]\n"
          << "sdk_version = \"latest\"\n";
    }
    {
        std::ofstream cache(tmp / "build" / "CMakeCache.txt");
        cache << "# fake cache; parser errors should return before build\n";
    }

    struct Case {
        std::vector<std::string> args;
        std::string needle;
    };
    const std::vector<Case> cases = {
        {{"dev", "--run"}, "--run requires a value"},
        {{"dev", "--run", "--test"}, "--run requires a value"},
        {{"dev", "--design"}, "--design requires a value"},
        {{"dev", "--design", "--validate"}, "--design requires a value"},
        {{"dev", "--target"}, "--target requires a value"},
        {{"dev", "--target", "--run", "app"}, "--target requires a value"},
    };

    const auto bin = fs::absolute(pulp_binary());
    auto cwd_saver = fs::current_path();
    fs::current_path(tmp);
    for (const auto& c : cases) {
        auto r = exec(bin.string(), c.args, 10000);
        INFO("stderr: " << r.stderr_output);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 2);
        REQUIRE(r.stderr_output.find(c.needle) != std::string::npos);
        REQUIRE(r.stdout_output.find("Building") == std::string::npos);
    }
    fs::current_path(cwd_saver);
    fs::remove_all(tmp);
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

// Tier A Slice 11: `pulp doctor --au-cache` refreshes the macOS AU
// registrar. We test --dry-run for determinism (running real killall
// in CI would race with other tests on the same runner).
TEST_CASE("pulp doctor --au-cache --dry-run reports the command and exits 0",
          "[cli][shellout][doctor][au-cache]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto r = run_pulp({"doctor", "--au-cache", "--dry-run"}, 10000);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
#if defined(__APPLE__)
    REQUIRE(r.stdout_output.find("AudioComponentRegistrar")
            != std::string::npos);
    REQUIRE(r.stdout_output.find("would run") != std::string::npos);
#else
    // Off-macOS, the flag is a documented no-op.
    REQUIRE(r.stdout_output.find("non-macOS") != std::string::npos);
#endif
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

TEST_CASE("pulp upgrade validates parser errors before network access",
          "[cli][shellout][upgrade][coverage][phase3]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    struct Case {
        std::vector<std::string> args;
        std::string needle;
    };
    const std::vector<Case> cases = {
        {{"upgrade", "--from"}, "--from requires a value"},
        {{"upgrade", "--from", "--to", "0.29.0"}, "--from requires a value"},
        {{"upgrade", "--to"}, "--to requires a value"},
        {{"upgrade", "--to", "--notes"}, "--to requires a value"},
        {{"upgrade", "--definitely-not-upgrade"}, "unknown flag"},
    };

    for (const auto& c : cases) {
        auto r = run_pulp(c.args, 10000);
        INFO("stderr: " << r.stderr_output);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 2);
        REQUIRE(r.stderr_output.find(c.needle) != std::string::npos);
        REQUIRE(r.stdout_output.find("Downloading") == std::string::npos);
    }
}

TEST_CASE("pulp sdk install validates parser errors before side effects",
          "[cli][shellout][sdk][coverage][phase3]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto home = unique_temp_dir("pulp-sdk-parser-home");

    struct Case {
        std::vector<std::string> args;
        std::string needle;
    };
    const std::vector<Case> cases = {
        {{"sdk", "install", "--version"}, "--version requires a value"},
        {{"sdk", "install", "--version", "--local"}, "--version requires a value"},
        {{"sdk", "install", "--definitely-not-sdk"}, "unknown flag"},
        {{"sdk", "install", "extra"}, "unknown argument"},
    };

    pulp_setenv("PULP_HOME", home.string().c_str(), 1);
    for (const auto& c : cases) {
        auto r = run_pulp(c.args, 10000);
        INFO("stderr: " << r.stderr_output);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 2);
        REQUIRE(r.stderr_output.find(c.needle) != std::string::npos);
        REQUIRE(r.stdout_output.find("Downloading SDK") == std::string::npos);
        REQUIRE(r.stdout_output.find("Building SDK") == std::string::npos);
    }
    pulp_unsetenv("PULP_HOME");

    REQUIRE_FALSE(fs::exists(home / "sdk"));
    REQUIRE_FALSE(fs::exists(home / "sdk-local"));
    fs::remove_all(home);
}

TEST_CASE("pulp sdk status and clean report cache state deterministically",
          "[cli][shellout][sdk][coverage]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto home = unique_temp_dir("pulp-sdk-status-home");
    ScopedEnvVar scoped_pulp_home("PULP_HOME");
    scoped_pulp_home.set(home.string());

    auto empty_status = run_pulp({"sdk", "status"}, 10000);
    REQUIRE_FALSE(empty_status.timed_out);
    REQUIRE(empty_status.exit_code == 0);
    REQUIRE(empty_status.stderr_output.empty());
    REQUIRE(empty_status.stdout_output.find("Pulp SDK Status") != std::string::npos);
    REQUIRE(empty_status.stdout_output.find("No SDK versions installed") != std::string::npos);
    REQUIRE(empty_status.stdout_output.find("Run: pulp sdk install") != std::string::npos);

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    write_text(home / "cache" / "latest_release.txt",
               "0.0.1\n" + std::to_string(now) + "\n");
    write_text(home / "sdk" / "1.2.3" / "version.txt", "1.2.3\n");
    write_text(home / "sdk-local" / "macos-arm64" / "2.0.0" / "lib" / "cmake" /
                   "Pulp" / "PulpConfig.cmake",
               "# fixture\n");
    write_text(home / "sdk-build" / "transient" / "marker.txt", "build cache\n");

    auto populated_status = run_pulp({"sdk", "status"}, 10000);
    REQUIRE_FALSE(populated_status.timed_out);
    REQUIRE(populated_status.exit_code == 0);
    REQUIRE(populated_status.stderr_output.empty());
    REQUIRE(populated_status.stdout_output.find("v1.2.3 (downloaded)") != std::string::npos);
    REQUIRE(populated_status.stdout_output.find("v2.0.0 (local build, macos-arm64)") !=
            std::string::npos);
    REQUIRE(populated_status.stdout_output.find("No SDK versions installed") ==
            std::string::npos);

    auto clean = run_pulp({"sdk", "clean"}, 10000);
    REQUIRE_FALSE(clean.timed_out);
    REQUIRE(clean.exit_code == 0);
    REQUIRE(clean.stderr_output.empty());
    REQUIRE(clean.stdout_output.find("Removed 3 SDK cache directories.") != std::string::npos);
    REQUIRE_FALSE(fs::exists(home / "sdk"));
    REQUIRE_FALSE(fs::exists(home / "sdk-local"));
    REQUIRE_FALSE(fs::exists(home / "sdk-build"));

    auto clean_again = run_pulp({"sdk", "clean"}, 10000);
    REQUIRE_FALSE(clean_again.timed_out);
    REQUIRE(clean_again.exit_code == 0);
    REQUIRE(clean_again.stderr_output.empty());
    REQUIRE(clean_again.stdout_output.find("Removed 0 SDK cache directories.") !=
            std::string::npos);

    fs::remove_all(home);
}

TEST_CASE("pulp status quotes source checkout paths before reading Git metadata",
          "[cli][shellout][status][coverage]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto root = unique_temp_dir("pulp status path with spaces and quote ' fixture");
    fs::create_directories(root / "core");
    fs::create_directories(root / "test");
    fs::create_directories(root / "examples" / "ExampleOne");
    write_text(root / "CMakeLists.txt",
               "cmake_minimum_required(VERSION 3.24)\n"
               "project(StatusFixture VERSION 1.0.0)\n");
    write_text(root / "test" / "status_fixture.cpp", "int status_fixture = 0;\n");

    auto init = exec("git", {"init", "-b", "status-fixture", root.string()}, 10000);
    REQUIRE_FALSE(init.timed_out);
    REQUIRE(init.exit_code == 0);

    const auto bin = fs::absolute(pulp_binary());
    REQUIRE(fs::exists(bin));
    auto previous = fs::current_path();
    fs::current_path(root);
    auto status = exec(bin.string(), {"status"}, 10000);
    fs::current_path(previous);

    REQUIRE_FALSE(status.timed_out);
    INFO("stdout: " << status.stdout_output);
    INFO("stderr: " << status.stderr_output);
    REQUIRE(status.exit_code == 0);
    REQUIRE(status.stderr_output.empty());
    REQUIRE(status.stdout_output.find("Pulp Project Status") != std::string::npos);
    const auto reported_root = fs::weakly_canonical(root);
    REQUIRE(status.stdout_output.find("Root: " + reported_root.string()) !=
            std::string::npos);
    REQUIRE(status.stdout_output.find("Mode: source-tree mode") != std::string::npos);
    REQUIRE(status.stdout_output.find("Branch: status-fixture") != std::string::npos);
    REQUIRE(status.stdout_output.find("Build: not configured") != std::string::npos);
    REQUIRE(status.stdout_output.find("Source files: 0 impl, 0 headers") != std::string::npos);
    REQUIRE(status.stdout_output.find("Test files: 1") != std::string::npos);
    REQUIRE(status.stdout_output.find("Examples: 1") != std::string::npos);

    fs::remove_all(root);
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
