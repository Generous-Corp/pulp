// test_cli_audio_latency.cpp — shell-out tests for `pulp audio render`'s
// latency proof.
//
// Per CLAUDE.md ("CLI behavior changes — shell out to the built binary, assert
// exit code + stderr content"). These cover the surface a caller drives: the
// probe refuses, at parse time and with a usable message, every stimulus that
// cannot reveal a delay. That refusal is the safety property — a probe that ran
// anyway would emit an inconclusive artifact and force the caller to decode it,
// and a caller who only checked "did the file appear" would read it as a pass.
//
// The plugin-loading path (load bundle → render → measure → gate the exit code)
// is not exercised here: no test in this suite depends on a built plugin bundle,
// because examples are OFF in several configure lanes and the dependency would
// be flaky. That path is covered instead by pulp-test-latency-contract, which
// drives the same shared evaluators through a source-owned processor across
// every outcome, including the gating one.

#include "test_cli_shellout_helpers.hpp"

using namespace pulp_test_cli;

namespace {

// Args that render successfully on their own; each case adds the probe flags
// under test. No --plugin resolution happens before the parse, so these fail (or
// pass) purely on the grammar.
std::vector<std::string> base_args() {
    return {"audio", "render", "--plugin", "nonexistent.clap", "--out",
            "/dev/null", "--duration-frames", "9600"};
}

ProcessResult render_with(std::vector<std::string> extra) {
    auto args = base_args();
    args.insert(args.end(), extra.begin(), extra.end());
    return run_pulp(args);
}

} // namespace

TEST_CASE("pulp audio render --help documents the latency proof",
          "[cli][audio-render][latency]") {
    if (!binary_exists()) return;
    const auto r = run_pulp({"audio", "render", "--help"});
    REQUIRE(r.exit_code == 0);
    const auto text = r.stdout_output + r.stderr_output;
    REQUIRE(text.find("--latency-report") != std::string::npos);
    REQUIRE(text.find("--latency-policy") != std::string::npos);
    REQUIRE(text.find("--latency-tolerance") != std::string::npos);
    // The gating contract has to be discoverable, or a caller will assume the
    // artifact's existence means the plugin passed.
    REQUIRE(text.find("NONZERO") != std::string::npos);
}

TEST_CASE("pulp audio render: the latency probe refuses silence",
          "[cli][audio-render][latency]") {
    if (!binary_exists()) return;
    // Silence carries no delay information. Refuse before loading the plugin.
    const auto r = render_with({"--latency-report", "/dev/null"});
    REQUIRE(r.exit_code == 2);
    REQUIRE(r.stderr_output.find("silence") != std::string::npos);
    REQUIRE(r.stderr_output.find("noise") != std::string::npos);
}

TEST_CASE("pulp audio render: the latency probe refuses a sine for delayed-null",
          "[cli][audio-render][latency]") {
    if (!binary_exists()) return;
    // A tone whose period is a whole number of samples nulls just as well one
    // period late, so the delay is not recoverable from it.
    const auto r = render_with({"--input-signal", "sine:440", "--latency-report",
                                "/dev/null", "--latency-policy", "delayed-null"});
    REQUIRE(r.exit_code == 2);
    REQUIRE(r.stderr_output.find("sine") != std::string::npos);
}

TEST_CASE("pulp audio render: the marker policy requires exactly one onset",
          "[cli][audio-render][latency]") {
    if (!binary_exists()) return;
    const auto r = render_with({"--input-signal", "noise", "--latency-report",
                                "/dev/null", "--latency-policy", "marker"});
    REQUIRE(r.exit_code == 2);
    REQUIRE(r.stderr_output.find("impulse") != std::string::npos);
}

TEST_CASE("pulp audio render: latency flags must agree with each other",
          "[cli][audio-render][latency]") {
    if (!binary_exists()) return;

    // A policy with nowhere to write its evidence.
    const auto no_report = render_with({"--input-signal", "noise",
                                        "--latency-policy", "delayed-null"});
    REQUIRE(no_report.exit_code == 2);
    REQUIRE(no_report.stderr_output.find("--latency-report") != std::string::npos);

    // An intrinsic response offset means nothing to the delayed-null policy.
    const auto stray_intrinsic = render_with({"--input-signal", "noise",
                                              "--latency-report", "/dev/null",
                                              "--latency-intrinsic", "100"});
    REQUIRE(stray_intrinsic.exit_code == 2);
    REQUIRE(stray_intrinsic.stderr_output.find("marker") != std::string::npos);

    // An unknown policy name.
    const auto bad_policy = render_with({"--input-signal", "noise", "--latency-report",
                                         "/dev/null", "--latency-policy", "correlate"});
    REQUIRE(bad_policy.exit_code == 2);
}
