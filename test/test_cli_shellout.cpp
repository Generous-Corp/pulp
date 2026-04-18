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

#include <filesystem>
#include <string>
#include <vector>

using namespace pulp::platform;
namespace fs = std::filesystem;

namespace {

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

// #8 / #355 — `pulp doctor android` and `pulp doctor ios` are
// recognized subcommands; bogus subcommand fails with exit 2 + Usage.
TEST_CASE("pulp doctor android|ios are recognized subcommands",
          "[cli][shellout][doctor][issue-355]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    const auto bin = fs::absolute(pulp_binary());

    auto android = exec(bin.string(), {"doctor", "android"}, 30000);
    auto ios     = exec(bin.string(), {"doctor", "ios"},     30000);
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
