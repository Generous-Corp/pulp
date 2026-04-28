// Unit tests for the `pulp run` flag parser introduced by pulp #914.
//
// CLAUDE.md: every correctness fix or feature slice ships its tests in
// the same PR. The shell-out coverage (which exercises the launched
// binary against a real fixture) lives in test_cli_run_headless.cpp;
// this file pins the pure parsing behaviour so future regressions on
// flag composition / forwarding fail loudly.

#include <catch2/catch_test_macros.hpp>

#include "tools/cli/cmd_run.hpp"

using pulp_cli::parse_run_options;
using pulp_cli::assemble_launch_args;

TEST_CASE("pulp run parses bare invocation as no-flag launch",
          "[cli][run][issue-914]") {
    auto r = parse_run_options({});
    REQUIRE_FALSE(r.help);
    REQUIRE(r.error.empty());
    REQUIRE(r.target_name.empty());
    REQUIRE_FALSE(r.headless);
    REQUIRE(r.screenshot_path.empty());
    REQUIRE(r.frames == 1);
    REQUIRE_FALSE(r.watch);
    REQUIRE(r.user_pass_through.empty());
}

TEST_CASE("pulp run --help short-circuits parsing",
          "[cli][run][issue-914]") {
    auto r = parse_run_options({"--help"});
    REQUIRE(r.help);
    REQUIRE(r.error.empty());

    auto rh = parse_run_options({"-h"});
    REQUIRE(rh.help);
}

TEST_CASE("pulp run --headless sets headless without a screenshot path",
          "[cli][run][issue-914]") {
    auto r = parse_run_options({"--headless"});
    REQUIRE(r.headless);
    REQUIRE(r.screenshot_path.empty());
    REQUIRE(r.frames == 1);

    auto args = assemble_launch_args(r);
    // --headless forwarded; no --screenshot; no --frames since default.
    REQUIRE(args.size() == 1);
    REQUIRE(args[0] == "--headless");
}

TEST_CASE("pulp run --screenshot implies --headless and forwards both",
          "[cli][run][issue-914]") {
    auto r = parse_run_options({"--screenshot", "/tmp/foo.png"});
    REQUIRE(r.headless);
    REQUIRE(r.screenshot_path == "/tmp/foo.png");
    REQUIRE(r.error.empty());

    auto args = assemble_launch_args(r);
    REQUIRE(args.size() == 3);
    REQUIRE(args[0] == "--headless");
    REQUIRE(args[1] == "--screenshot");
    REQUIRE(args[2] == "/tmp/foo.png");
}

TEST_CASE("pulp run --screenshot=PATH form is accepted",
          "[cli][run][issue-914]") {
    auto r = parse_run_options({"--screenshot=/tmp/bar.png"});
    REQUIRE(r.headless);
    REQUIRE(r.screenshot_path == "/tmp/bar.png");
}

TEST_CASE("pulp run --screenshot without a path is a parse error",
          "[cli][run][issue-914]") {
    auto r = parse_run_options({"--screenshot"});
    REQUIRE_FALSE(r.error.empty());

    // Edge case: the next token starts with '-', so we can't safely
    // claim it as the path.
    auto r2 = parse_run_options({"--screenshot", "--frames"});
    REQUIRE_FALSE(r2.error.empty());

    auto r3 = parse_run_options({"--screenshot="});
    REQUIRE_FALSE(r3.error.empty());
}

TEST_CASE("pulp run --frames sets the frame count and forwards it",
          "[cli][run][issue-914]") {
    auto r = parse_run_options({"--frames", "5"});
    REQUIRE(r.frames == 5);
    REQUIRE(r.error.empty());

    auto args = assemble_launch_args(r);
    REQUIRE(args.size() == 2);
    REQUIRE(args[0] == "--frames");
    REQUIRE(args[1] == "5");

    auto reqform = parse_run_options({"--frames=12"});
    REQUIRE(reqform.frames == 12);
}

TEST_CASE("pulp run --frames rejects non-positive and non-integer values",
          "[cli][run][issue-914]") {
    REQUIRE_FALSE(parse_run_options({"--frames", "0"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--frames", "-2"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--frames", "abc"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--frames"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--frames=zero"}).error.empty());
}

TEST_CASE("pulp run --watch is recorded and composes with --headless",
          "[cli][run][issue-914]") {
    auto plain = parse_run_options({"--watch"});
    REQUIRE(plain.watch);
    REQUIRE_FALSE(plain.headless);

    auto compound = parse_run_options(
        {"--watch", "--headless", "--screenshot", "/tmp/w.png", "--frames", "3"});
    REQUIRE(compound.watch);
    REQUIRE(compound.headless);
    REQUIRE(compound.screenshot_path == "/tmp/w.png");
    REQUIRE(compound.frames == 3);

    auto args = assemble_launch_args(compound);
    // --watch is consumed by the CLI, NOT forwarded to the launched
    // binary. The forwarded args reflect the headless+screenshot+frames
    // shape only.
    REQUIRE(args.size() == 5);
    REQUIRE(args[0] == "--headless");
    REQUIRE(args[1] == "--screenshot");
    REQUIRE(args[2] == "/tmp/w.png");
    REQUIRE(args[3] == "--frames");
    REQUIRE(args[4] == "3");
}

TEST_CASE("pulp run accepts a positional target name",
          "[cli][run][issue-914]") {
    auto r = parse_run_options({"pulp-gain"});
    REQUIRE(r.target_name == "pulp-gain");

    auto with_flags = parse_run_options(
        {"pulp-gain", "--headless", "--screenshot", "/tmp/x.png"});
    REQUIRE(with_flags.target_name == "pulp-gain");
    REQUIRE(with_flags.headless);
    REQUIRE(with_flags.screenshot_path == "/tmp/x.png");
}

TEST_CASE("pulp run forwards arguments after `--` verbatim",
          "[cli][run][issue-914]") {
    auto r = parse_run_options(
        {"--headless", "--", "--script", "ui.js", "--debug-port=9222"});
    REQUIRE(r.headless);
    REQUIRE(r.user_pass_through.size() == 3);
    REQUIRE(r.user_pass_through[0] == "--script");
    REQUIRE(r.user_pass_through[1] == "ui.js");
    REQUIRE(r.user_pass_through[2] == "--debug-port=9222");

    auto args = assemble_launch_args(r);
    REQUIRE(args.size() == 4);
    REQUIRE(args[0] == "--headless");
    REQUIRE(args[1] == "--script");
    REQUIRE(args[2] == "ui.js");
    REQUIRE(args[3] == "--debug-port=9222");
}

TEST_CASE("pulp run preserves unknown flags as pass-through",
          "[cli][run][issue-914]") {
    // Legacy cmd_run silently dropped unknown flags; preserving that
    // permissive shape avoids breaking callers that already forward
    // launcher-specific flags inline.
    auto r = parse_run_options({"--headless", "--debug-port=9222"});
    REQUIRE(r.headless);
    // --debug-port=9222 is unknown to the parser, so it ends up
    // as a pass-through token. (For unknown flags that take a separate
    // value argument, prefer `pulp run --headless -- --debug-port 9222`
    // since the parser otherwise treats the second token as a positional
    // target — matching the legacy behaviour.)
    REQUIRE(r.user_pass_through.size() == 1);
    REQUIRE(r.user_pass_through[0] == "--debug-port=9222");
}

TEST_CASE("pulp run --frames default does not appear in launch args",
          "[cli][run][issue-914]") {
    // When --frames is the default 1, we DON'T forward it: keeps the
    // launch line minimal and avoids spurious diffs in CI logs.
    auto r = parse_run_options({"--headless"});
    auto args = assemble_launch_args(r);
    for (const auto& a : args) {
        REQUIRE(a != "--frames");
    }
}
