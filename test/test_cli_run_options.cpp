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
          "[cli][run][issue-914][issue-643]") {
    REQUIRE_FALSE(parse_run_options({"--frames", "0"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--frames", "-2"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--frames", "abc"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--frames", "12x"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--frames", "12.5"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--frames", "+5"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--frames"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--frames=zero"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--frames=12x"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--frames=12.5"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--frames=+5"}).error.empty());

    auto partial = parse_run_options({"--frames=12x"});
    REQUIRE(partial.frames == 1);
}

TEST_CASE("pulp run --frames rejects overflow and plus-prefixed values",
          "[cli][run]") {
    REQUIRE_FALSE(parse_run_options({"--frames", "999999999999999999999"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--frames=999999999999999999999"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--frames", "+5"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--frames=+5"}).error.empty());
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

TEST_CASE("pulp run forwards all tokens after separator including known flags",
          "[cli][run]") {
    auto r = parse_run_options({"target", "--", "--headless", "--frames", "9"});
    REQUIRE(r.target_name == "target");
    REQUIRE_FALSE(r.headless);
    REQUIRE(r.frames == 1);
    REQUIRE(r.user_pass_through == std::vector<std::string>{
        "--headless", "--frames", "9",
    });

    auto args = assemble_launch_args(r);
    REQUIRE(args == r.user_pass_through);
}

TEST_CASE("pulp run ignores repeated separator tokens before pass-through",
          "[cli][run]") {
    auto r = parse_run_options({"target", "--", "--", "--frames=4"});
    REQUIRE(r.target_name == "target");
    REQUIRE(r.user_pass_through == std::vector<std::string>{"--frames=4"});
}

TEST_CASE("pulp run treats additional positionals as pass-through",
          "[cli][run]") {
    auto r = parse_run_options({"primary-target", "secondary", "third"});
    REQUIRE(r.target_name == "primary-target");
    REQUIRE(r.user_pass_through == std::vector<std::string>{"secondary", "third"});
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

TEST_CASE("pulp run keeps last repeated scalar flag values",
          "[cli][run]") {
    auto r = parse_run_options({
        "--frames", "2",
        "--screenshot", "/tmp/first.png",
        "--frames=8",
        "--screenshot=/tmp/final.png",
    });

    REQUIRE(r.error.empty());
    REQUIRE(r.headless);
    REQUIRE(r.frames == 8);
    REQUIRE(r.screenshot_path == "/tmp/final.png");
    REQUIRE(r.user_pass_through.empty());

    auto args = assemble_launch_args(r);
    REQUIRE(args == std::vector<std::string>{
        "--headless", "--screenshot", "/tmp/final.png", "--frames", "8",
    });
}

TEST_CASE("pulp run preserves pass-through ordering after launcher flags",
          "[cli][run]") {
    auto r = parse_run_options({
        "demo",
        "--headless",
        "--frames", "4",
        "--unknown",
        "--",
        "--runtime", "trace",
    });

    REQUIRE(r.error.empty());
    REQUIRE(r.target_name == "demo");
    REQUIRE(r.headless);
    REQUIRE(r.frames == 4);
    REQUIRE(r.user_pass_through == std::vector<std::string>{
        "--unknown", "--runtime", "trace",
    });

    auto args = assemble_launch_args(r);
    REQUIRE(args == std::vector<std::string>{
        "--headless", "--frames", "4", "--unknown", "--runtime", "trace",
    });
}

TEST_CASE("pulp run records launcher flags before a separator only",
          "[cli][run]") {
    auto r = parse_run_options({
        "--screenshot=/tmp/before.png",
        "--",
        "--screenshot=/tmp/after.png",
        "--watch",
        "--frames=9",
    });

    REQUIRE(r.error.empty());
    REQUIRE(r.headless);
    REQUIRE(r.screenshot_path == "/tmp/before.png");
    REQUIRE_FALSE(r.watch);
    REQUIRE(r.frames == 1);
    REQUIRE(r.user_pass_through == std::vector<std::string>{
        "--screenshot=/tmp/after.png", "--watch", "--frames=9",
    });

    auto args = assemble_launch_args(r);
    REQUIRE(args == std::vector<std::string>{
        "--headless",
        "--screenshot",
        "/tmp/before.png",
        "--screenshot=/tmp/after.png",
        "--watch",
        "--frames=9",
    });
}

TEST_CASE("pulp run help stops before later invalid flags",
          "[cli][run]") {
    auto long_help = parse_run_options({"--help", "--frames", "bad"});
    REQUIRE(long_help.help);
    REQUIRE(long_help.error.empty());
    REQUIRE(long_help.user_pass_through.empty());

    auto short_help = parse_run_options({"-h", "--screenshot"});
    REQUIRE(short_help.help);
    REQUIRE(short_help.error.empty());
    REQUIRE_FALSE(short_help.headless);
}

TEST_CASE("pulp run reports screenshot path shape errors without consuming later tokens",
          "[cli][run]") {
    auto flag_like = parse_run_options({"--screenshot", "-out.png"});
    REQUIRE(flag_like.error == "--screenshot requires a path argument");
    REQUIRE(flag_like.headless);
    REQUIRE(flag_like.screenshot_path.empty());

    auto empty_equals = parse_run_options({"--screenshot=", "--frames", "2"});
    REQUIRE(empty_equals.error == "--screenshot= requires a non-empty path");
    REQUIRE(empty_equals.headless);
    REQUIRE(empty_equals.frames == 1);
}

TEST_CASE("pulp run reports frame count shape errors precisely",
          "[cli][run]") {
    auto missing = parse_run_options({"--frames"});
    REQUIRE(missing.error == "--frames requires an integer argument");
    REQUIRE(missing.frames == 1);

    auto flag_like = parse_run_options({"--frames", "--watch"});
    REQUIRE(flag_like.error == "--frames requires an integer argument");
    REQUIRE_FALSE(flag_like.watch);

    auto zero = parse_run_options({"--frames=0"});
    REQUIRE(zero.error == "--frames must be > 0");
    REQUIRE(zero.frames == 1);

    auto malformed = parse_run_options({"--frames=2x"});
    REQUIRE(malformed.error == "--frames= requires an integer");
    REQUIRE(malformed.frames == 1);
}

TEST_CASE("pulp run accepts boundary frame count values",
          "[cli][run]") {
    auto single = parse_run_options({"--frames", "1"});
    REQUIRE(single.error.empty());
    REQUIRE(single.frames == 1);

    auto large = parse_run_options({"--frames=2147483647"});
    REQUIRE(large.error.empty());
    REQUIRE(large.frames == 2147483647);

    auto args = assemble_launch_args(large);
    REQUIRE(args == std::vector<std::string>{"--frames", "2147483647"});
}

TEST_CASE("pulp run --audio-inspector parses and forwards without implying headless",
          "[cli][run][audio-inspector]") {
    auto r = parse_run_options({"--audio-inspector"});
    REQUIRE(r.error.empty());
    REQUIRE(r.audio_inspector);
    REQUIRE_FALSE(r.headless);  // a dev may want the visible inspector window

    auto args = assemble_launch_args(r);
    REQUIRE(args == std::vector<std::string>{"--audio-inspector"});

    // Composes with --screenshot (which implies headless) for CI capture.
    auto ci = parse_run_options({"--audio-inspector", "--screenshot", "/tmp/i.png"});
    REQUIRE(ci.audio_inspector);
    REQUIRE(ci.headless);
    auto ci_args = assemble_launch_args(ci);
    REQUIRE(ci_args == std::vector<std::string>{
        "--headless", "--screenshot", "/tmp/i.png", "--audio-inspector",
    });
}

TEST_CASE("pulp run --audio-probe-json implies headless and forwards the path",
          "[cli][run][audio-inspector]") {
    auto r = parse_run_options({"--audio-probe-json", "/tmp/probe.json"});
    REQUIRE(r.error.empty());
    REQUIRE(r.audio_probe_json_path == "/tmp/probe.json");
    REQUIRE(r.headless);  // one-shot dump + exit, like --screenshot

    auto args = assemble_launch_args(r);
    REQUIRE(args == std::vector<std::string>{
        "--headless", "--audio-probe-json", "/tmp/probe.json",
    });

    auto eq = parse_run_options({"--audio-probe-json=/tmp/p2.json"});
    REQUIRE(eq.audio_probe_json_path == "/tmp/p2.json");
    REQUIRE(eq.headless);
}

TEST_CASE("pulp run --audio-probe-json without a path is a parse error",
          "[cli][run][audio-inspector]") {
    REQUIRE_FALSE(parse_run_options({"--audio-probe-json"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-probe-json", "--frames"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-probe-json="}).error.empty());
}

TEST_CASE("pulp run --audio-scope-json implies headless and forwards acquisition flags",
          "[cli][run][audio-scope]") {
    auto r = parse_run_options({
        "--audio-scope-json", "/tmp/scope.json",
        "--audio-scope-window", "4096",
        "--audio-scope-trigger", "rising-zero",
        "--audio-scope-channel", "0",
    });
    REQUIRE(r.error.empty());
    REQUIRE(r.audio_scope_json_path == "/tmp/scope.json");
    REQUIRE(r.audio_scope_window == 4096);
    REQUIRE(r.audio_scope_trigger == "rising-zero");
    REQUIRE(r.audio_scope_channel == 0);
    REQUIRE(r.headless);

    auto args = assemble_launch_args(r);
    REQUIRE(args == std::vector<std::string>{
        "--headless",
        "--audio-scope-json", "/tmp/scope.json",
        "--audio-scope-window", "4096",
        "--audio-scope-trigger", "rising-zero",
        "--audio-scope-channel", "0",
    });

    auto eq = parse_run_options({
        "--audio-scope-json=/tmp/scope2.json",
        "--audio-scope-window=128",
        "--audio-scope-trigger=raw",
        "--audio-scope-channel=1",
    });
    REQUIRE(eq.error.empty());
    REQUIRE(eq.audio_scope_json_path == "/tmp/scope2.json");
    REQUIRE(eq.audio_scope_window == 128);
    REQUIRE(eq.audio_scope_trigger == "raw");
    REQUIRE(eq.audio_scope_channel == 1);
}

TEST_CASE("pulp run rejects invalid audio scope options and inspector contention",
          "[cli][run][audio-scope]") {
    REQUIRE_FALSE(parse_run_options({"--audio-scope-json"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-scope-json="}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-scope-window", "0"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-scope-window=-1"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-scope-trigger", "smooth"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-scope-channel", "-1"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-scope-window", "128"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-scope-trigger=raw"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-scope-channel=1"}).error.empty());

    auto contention = parse_run_options({
        "--audio-inspector", "--audio-scope-json", "/tmp/scope.json",
    });
    REQUIRE_FALSE(contention.error.empty());
}

TEST_CASE("pulp run --audio-capture-wav implies headless and forwards the path",
          "[cli][run][audio-capture-wav]") {
    auto r = parse_run_options({"--audio-capture-wav", "/tmp/cap.wav",
                                "--audio-capture-frames", "1024"});
    REQUIRE(r.error.empty());
    REQUIRE(r.audio_capture_wav_path == "/tmp/cap.wav");
    REQUIRE(r.audio_capture_frames == 1024);
    REQUIRE(r.headless);
    auto args = assemble_launch_args(r);
    REQUIRE(std::find(args.begin(), args.end(), "--audio-capture-wav") != args.end());
    REQUIRE(std::find(args.begin(), args.end(), "/tmp/cap.wav") != args.end());
    REQUIRE(std::find(args.begin(), args.end(), "--audio-capture-frames") != args.end());
    REQUIRE(std::find(args.begin(), args.end(), "1024") != args.end());

    auto eq = parse_run_options({"--audio-capture-wav=/tmp/cap2.wav"});
    REQUIRE(eq.error.empty());
    REQUIRE(eq.audio_capture_wav_path == "/tmp/cap2.wav");
    REQUIRE(eq.headless);
    // Frames default to 0 (ring default) → not forwarded.
    auto eq_args = assemble_launch_args(eq);
    REQUIRE(std::find(eq_args.begin(), eq_args.end(), "--audio-capture-frames")
            == eq_args.end());
}

TEST_CASE("pulp run rejects invalid audio-capture-wav options and FIFO contention",
          "[cli][run][audio-capture-wav]") {
    REQUIRE_FALSE(parse_run_options({"--audio-capture-wav"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-capture-wav="}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-capture-frames", "0",
                                     "--audio-capture-wav", "/tmp/c.wav"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-capture-frames=-1",
                                     "--audio-capture-wav", "/tmp/c.wav"}).error.empty());
    // --audio-capture-frames requires --audio-capture-wav.
    REQUIRE_FALSE(parse_run_options({"--audio-capture-frames", "512"}).error.empty());
    // Shares the one capture FIFO with inspector + scope-json.
    REQUIRE_FALSE(parse_run_options({"--audio-inspector",
                                     "--audio-capture-wav", "/tmp/c.wav"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-scope-json", "/tmp/s.json",
                                     "--audio-capture-wav", "/tmp/c.wav"}).error.empty());
}

TEST_CASE("pulp run --audio-capture-rolling implies headless and forwards the path",
          "[cli][run][audio-capture-rolling]") {
    auto r = parse_run_options({"--audio-capture-rolling", "/tmp/roll.wav",
                                "--audio-capture-rolling-frames", "8192"});
    REQUIRE(r.error.empty());
    REQUIRE(r.audio_capture_rolling_path == "/tmp/roll.wav");
    REQUIRE(r.audio_capture_rolling_frames == 8192);
    REQUIRE(r.headless);
    auto args = assemble_launch_args(r);
    REQUIRE(std::find(args.begin(), args.end(), "--audio-capture-rolling") != args.end());
    REQUIRE(std::find(args.begin(), args.end(), "/tmp/roll.wav") != args.end());
    REQUIRE(std::find(args.begin(), args.end(), "--audio-capture-rolling-frames")
            != args.end());
    REQUIRE(std::find(args.begin(), args.end(), "8192") != args.end());

    auto eq = parse_run_options({"--audio-capture-rolling=/tmp/roll2.wav"});
    REQUIRE(eq.error.empty());
    REQUIRE(eq.audio_capture_rolling_path == "/tmp/roll2.wav");
    REQUIRE(eq.headless);
    REQUIRE(eq.audio_capture_rolling_int24 == false);  // float is the default
    auto eq_args = assemble_launch_args(eq);
    REQUIRE(std::find(eq_args.begin(), eq_args.end(), "--audio-capture-rolling-frames")
            == eq_args.end());
    REQUIRE(std::find(eq_args.begin(), eq_args.end(), "--audio-capture-rolling-format")
            == eq_args.end());  // default float not forwarded

    auto eq_frames = parse_run_options({"--audio-capture-rolling=/tmp/roll3.wav",
                                        "--audio-capture-rolling-frames=2048"});
    REQUIRE(eq_frames.error.empty());
    REQUIRE(eq_frames.audio_capture_rolling_path == "/tmp/roll3.wav");
    REQUIRE(eq_frames.audio_capture_rolling_frames == 2048);
    auto eq_frame_args = assemble_launch_args(eq_frames);
    REQUIRE(std::find(eq_frame_args.begin(), eq_frame_args.end(),
                      "--audio-capture-rolling-frames") != eq_frame_args.end());
    REQUIRE(std::find(eq_frame_args.begin(), eq_frame_args.end(), "2048")
            != eq_frame_args.end());
}

TEST_CASE("pulp run --audio-capture-rolling-format int24 parses and forwards",
          "[cli][run][audio-capture-rolling]") {
    auto r = parse_run_options({"--audio-capture-rolling", "/tmp/r.wav",
                                "--audio-capture-rolling-format", "int24"});
    REQUIRE(r.error.empty());
    REQUIRE(r.audio_capture_rolling_int24);
    auto args = assemble_launch_args(r);
    REQUIRE(std::find(args.begin(), args.end(), "--audio-capture-rolling-format") != args.end());
    REQUIRE(std::find(args.begin(), args.end(), "int24") != args.end());

    // Explicit float, = form.
    auto f = parse_run_options({"--audio-capture-rolling=/tmp/r.wav",
                                "--audio-capture-rolling-format=float"});
    REQUIRE(f.error.empty());
    REQUIRE(f.audio_capture_rolling_int24 == false);

    // Invalid value and requires-rolling are rejected.
    REQUIRE_FALSE(parse_run_options({"--audio-capture-rolling", "/tmp/r.wav",
                                     "--audio-capture-rolling-format", "int32"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-capture-rolling-format", "int24"}).error.empty());
}

TEST_CASE("pulp run rejects invalid audio-capture-rolling options and capture-mode contention",
          "[cli][run][audio-capture-rolling]") {
    REQUIRE_FALSE(parse_run_options({"--audio-capture-rolling"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-capture-rolling="}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-capture-rolling-frames", "0",
                                     "--audio-capture-rolling", "/tmp/r.wav"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-capture-rolling-frames=0",
                                     "--audio-capture-rolling", "/tmp/r.wav"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-capture-rolling-frames=abc",
                                     "--audio-capture-rolling", "/tmp/r.wav"}).error.empty());
    // --audio-capture-rolling-frames requires --audio-capture-rolling.
    REQUIRE_FALSE(parse_run_options({"--audio-capture-rolling-frames", "512"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-capture-rolling-frames=512"}).error.empty());
    // Only one capture mode per invocation.
    REQUIRE_FALSE(parse_run_options({"--audio-inspector",
                                     "--audio-capture-rolling", "/tmp/r.wav"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-probe-json", "/tmp/probe.json",
                                     "--audio-capture-rolling", "/tmp/r.wav"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-scope-json", "/tmp/s.json",
                                     "--audio-capture-rolling", "/tmp/r.wav"}).error.empty());
    REQUIRE_FALSE(parse_run_options({"--audio-capture-wav", "/tmp/c.wav",
                                     "--audio-capture-rolling", "/tmp/r.wav"}).error.empty());
}

TEST_CASE("pulp run treats negative-looking targets as pass-through flags",
          "[cli][run]") {
    auto r = parse_run_options({"--standalone", "demo"});
    REQUIRE(r.target_name == "demo");
    REQUIRE(r.user_pass_through == std::vector<std::string>{"--standalone"});
    REQUIRE_FALSE(r.headless);

    auto args = assemble_launch_args(r);
    REQUIRE(args == std::vector<std::string>{"--standalone"});
}
