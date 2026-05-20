// test_cli_shellout_loop.cpp — extracted from test_cli_shellout.cpp
// in the 2026-05 Phase 5 P5-4 refactor.
//
// Issue #940 — `pulp loop` shell-out tests. Covers the surface that
// doesn't depend on a live watch loop (the watch-loop interactions are
// tested by `pulp dev`'s harness):
//
//   * `pulp loop --help` / --status / --off
//   * `pulp loop --platform=<known|unknown>`
//   * `pulp loop --no-watch` and related flags
//   * `pulp loop --watch-issues` / `--ar-swap-from` deferred-slice hints
//
// All tests run with PULP_HOME pointed at a per-test tmpdir so the
// developer's real config.toml is never touched.

#include "test_cli_shellout_helpers.hpp"

using namespace pulp::platform;
namespace fs = std::filesystem;
using namespace pulp_test_cli;

// Issue #940 Slice 1 — `pulp loop` is the leveraged-prototype focus
// mode entry point. These tests cover the surface that doesn't depend
// on a live watch loop (the watch-loop interactions are tested by
// `pulp dev`'s harness):
//
//   * `pulp loop --help`     exits 0 + advertises focus mode
//   * `pulp loop --status`   exits 0, reports detected host + focus state
//   * `pulp loop --off`      clears focus state (and tolerates "already off")
//   * `pulp loop --platform=<known>`   accepts macos/linux/windows
//   * `pulp loop --platform=<unknown>` exits non-zero with diagnostic
//   * `pulp loop --no-watch` flips focus + exits 0 without watching
//
// All of these run with PULP_HOME pointed at a per-test tmpdir so the
// developer's real config.toml is never touched.
TEST_CASE("pulp loop --help exits 0 with focus-mode banner",
          "[cli][shellout][loop][issue-940]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"loop", "--help"});
    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.stdout_output.find("leveraged-prototype focus mode") != std::string::npos);
    REQUIRE(r.stdout_output.find("--platform") != std::string::npos);
    REQUIRE(r.stdout_output.find("--off") != std::string::npos);
}

TEST_CASE("pulp loop --status reports detected host and focus state",
          "[cli][shellout][loop][issue-940]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-940-status");
    fs::create_directories(tmp_home);
    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    auto r = run_pulp({"loop", "--status"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("detected host") != std::string::npos);
    REQUIRE(r.stdout_output.find("focus platform") != std::string::npos);
    // Fresh PULP_HOME — no focus persisted yet.
    REQUIRE(r.stdout_output.find("(none") != std::string::npos);

    pulp_unsetenv("PULP_HOME");
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    fs::remove_all(tmp_home);
}

TEST_CASE("pulp loop --off clears focus state idempotently",
          "[cli][shellout][loop][issue-940]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-940-off");
    fs::create_directories(tmp_home);
    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    // First call: nothing to clear, must still exit 0.
    auto r1 = run_pulp({"loop", "--off"});
    REQUIRE_FALSE(r1.timed_out);
    REQUIRE(r1.exit_code == 0);
    REQUIRE(r1.stdout_output.find("already in cross-platform mode") != std::string::npos);

    // Set focus via --no-watch, then clear.
    auto r_set = run_pulp({"loop", "--platform=macos", "--no-watch"});
    REQUIRE_FALSE(r_set.timed_out);
    REQUIRE(r_set.exit_code == 0);
    REQUIRE(r_set.stdout_output.find("focus mode active on macos") != std::string::npos);

    auto r2 = run_pulp({"loop", "--off"});
    REQUIRE_FALSE(r2.timed_out);
    REQUIRE(r2.exit_code == 0);
    REQUIRE(r2.stdout_output.find("focus mode cleared") != std::string::npos);
    REQUIRE(r2.stdout_output.find("Cross-platform mode restored") != std::string::npos);

    pulp_unsetenv("PULP_HOME");
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    fs::remove_all(tmp_home);
}

TEST_CASE("pulp loop --platform=<known> accepts macos|linux|windows",
          "[cli][shellout][loop][issue-940]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-940-platform");
    fs::create_directories(tmp_home);
    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    for (const char* p : {"macos", "linux", "windows"}) {
        auto r = run_pulp({"loop", "--platform", p, "--no-watch"});
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 0);
        std::string expect = std::string("focus mode active on ") + p;
        REQUIRE(r.stdout_output.find(expect) != std::string::npos);
    }

    pulp_unsetenv("PULP_HOME");
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    fs::remove_all(tmp_home);
}

TEST_CASE("pulp loop --platform=<unknown> exits non-zero with diagnostic",
          "[cli][shellout][loop][issue-940]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-940-unknown");
    fs::create_directories(tmp_home);
    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    auto r = run_pulp({"loop", "--platform=plan9", "--no-watch"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code != 0);
    auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find("unknown --platform") != std::string::npos);
    REQUIRE(combined.find("macos") != std::string::npos);

    pulp_unsetenv("PULP_HOME");
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    fs::remove_all(tmp_home);
}

TEST_CASE("pulp loop validates value options before focus state changes",
          "[cli][shellout][loop][coverage][phase3]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-loop-parser-errors");
    fs::create_directories(tmp_home);
    ScopedEnvVar pulp_home("PULP_HOME");
    pulp_home.set(tmp_home.string());

    struct Case {
        std::vector<std::string> args;
        std::string stderr_substring;
    };
    const std::vector<Case> cases = {
        {{"loop", "--platform"}, "--platform requires a value"},
        {{"loop", "--platform", "--no-watch"}, "--platform requires a value"},
        {{"loop", "--watch-issues"}, "--watch-issues requires a value"},
        {{"loop", "--watch-issues", "--no-watch"}, "--watch-issues requires a value"},
        {{"loop", "--ar-swap-from"}, "--ar-swap-from requires a value"},
        {{"loop", "--ar-swap-from", "--no-watch"}, "--ar-swap-from requires a value"},
        {{"loop", "--run"}, "--run requires a value"},
        {{"loop", "--run", "--target", "pulp-format"}, "--run requires a value"},
        {{"loop", "--target"}, "--target requires a value"},
        {{"loop", "--target", "--no-watch"}, "--target requires a value"},
    };

    for (const auto& c : cases) {
        INFO("loop args size: " << c.args.size());
        auto r = run_pulp(c.args, 10000);
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 2);
        REQUIRE(r.stderr_output.find(c.stderr_substring) != std::string::npos);
    }

    auto status = run_pulp({"loop", "--status"}, 10000);
    REQUIRE_FALSE(status.timed_out);
    REQUIRE(status.exit_code == 0);
    REQUIRE(status.stdout_output.find("focus platform: (none") != std::string::npos);

    fs::remove_all(tmp_home);
}

TEST_CASE("pulp loop --watch-issues prints deferred-slice hint",
          "[cli][shellout][loop][issue-940]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-940-watch-issues");
    fs::create_directories(tmp_home);
    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    auto r = run_pulp({"loop", "--watch-issues=924,927", "--no-watch"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    // Slice 3 deferred — surface the issue number so the user knows
    // when the helper lands.
    REQUIRE(r.stdout_output.find("--watch-issues=924,927") != std::string::npos);
    REQUIRE(r.stdout_output.find("deferred") != std::string::npos);

    pulp_unsetenv("PULP_HOME");
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    fs::remove_all(tmp_home);
}

TEST_CASE("pulp loop --ar-swap-from prints deferred-slice hint",
          "[cli][shellout][loop][issue-940]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-940-ar-swap");
    fs::create_directories(tmp_home);
    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    // Long form (separate value)
    auto r1 = run_pulp({"loop", "--ar-swap-from", "feat/fix-x", "--no-watch"});
    REQUIRE_FALSE(r1.timed_out);
    REQUIRE(r1.exit_code == 0);
    REQUIRE(r1.stdout_output.find("--ar-swap-from=feat/fix-x") != std::string::npos);
    REQUIRE(r1.stdout_output.find("deferred") != std::string::npos);

    // Equals form
    auto r2 = run_pulp({"loop", "--ar-swap-from=feat/fix-y", "--no-watch"});
    REQUIRE_FALSE(r2.timed_out);
    REQUIRE(r2.exit_code == 0);
    REQUIRE(r2.stdout_output.find("--ar-swap-from=feat/fix-y") != std::string::npos);

    pulp_unsetenv("PULP_HOME");
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    fs::remove_all(tmp_home);
}

TEST_CASE("pulp loop --status reports persisted focus platform",
          "[cli][shellout][loop][issue-940]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-940-status-set");
    fs::create_directories(tmp_home);
    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    // Set focus, then assert --status reflects the persisted value.
    auto rs = run_pulp({"loop", "--platform=linux", "--no-watch"});
    REQUIRE_FALSE(rs.timed_out);
    REQUIRE(rs.exit_code == 0);

    auto r = run_pulp({"loop", "--status"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("focus platform: linux") != std::string::npos);

    pulp_unsetenv("PULP_HOME");
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    fs::remove_all(tmp_home);
}

TEST_CASE("pulp loop accepts --test, --validate, --target, --run flags via --no-watch",
          "[cli][shellout][loop][issue-940]") {
    // The flags exercise the parser branches that wouldn't otherwise run
    // when only --status / --off / --no-watch are used. With --no-watch
    // we exit before the watch loop, so the binary still terminates.
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp_home = unique_temp_dir("pulp-940-flags");
    fs::create_directories(tmp_home);
    pulp_setenv("PULP_HOME", tmp_home.string().c_str(), 1);
    pulp_setenv("PULP_UPDATE_CHECK_DISABLED", "1", 1);

    // --test, -t, --test-filter=, --validate, --allow-unsupported-sdk
    // all exit cleanly with --no-watch.
    auto r1 = run_pulp({"loop", "--platform=macos", "--test", "--validate", "--no-watch"});
    REQUIRE_FALSE(r1.timed_out);
    REQUIRE(r1.exit_code == 0);

    auto r2 = run_pulp({"loop", "--platform=macos", "-t", "--test-filter=Foo", "--no-watch"});
    REQUIRE_FALSE(r2.timed_out);
    REQUIRE(r2.exit_code == 0);

    // --target T pushes onto build_args; --run T sets launch_target. Both
    // branches must accept the next-arg form without breaking the parser.
    auto r3 = run_pulp({"loop", "--platform=macos", "--target", "pulp-format",
                        "--run", "pulp-gain-standalone", "--no-watch"});
    REQUIRE_FALSE(r3.timed_out);
    REQUIRE(r3.exit_code == 0);

    // Trailing -- forwards remaining args to the launched binary; with
    // --no-watch we just exit so the args are silently consumed.
    auto r4 = run_pulp({"loop", "--platform=macos", "--no-watch", "--", "--arg1", "value1"});
    REQUIRE_FALSE(r4.timed_out);
    REQUIRE(r4.exit_code == 0);

    pulp_unsetenv("PULP_HOME");
    pulp_unsetenv("PULP_UPDATE_CHECK_DISABLED");
    fs::remove_all(tmp_home);
}
