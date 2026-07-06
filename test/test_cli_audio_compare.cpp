// test_cli_audio_compare.cpp — shell-out tests for `pulp audio compare`.
//
// Per CLAUDE.md ("CLI behavior changes — shell out to the built binary, assert
// exit code + stderr content"). `pulp audio compare` is a thin orchestrator: it
// locates the opt-in Audio Quality Lab managed tool ($HOME/.pulp/tools/python-
// envs/audio-quality-lab/run.sh) and forwards the `compare` verb to it. These
// tests prove that orchestration WITHOUT needing numpy/soundfile installed:
//   - --help / arity are handled locally,
//   - a missing managed tool yields the actionable install hint + exit 1,
//   - when the tool IS present the CLI forwards the verb + all flags verbatim
//     and passes through the tool's exit code (proven with a fake run.sh).
// The measurement itself is covered by the tool's own 126 pytest cases.

#include "test_cli_shellout_helpers.hpp"

#include <fstream>

using namespace pulp_test_cli;
namespace fs = std::filesystem;

namespace {

// Point $HOME at a fresh temp dir so `locate_tool` looks for the managed wrapper
// under our control. `pulp_home` is cleared because `pulp_home()` honors PULP_HOME
// *before* HOME — a PULP_HOME set on the box (CI/dev) would otherwise bypass our
// HOME override and read the machine's real ~/.pulp. Both ScopedEnvVars are
// caller-owned; their destructors restore the prior environment.
fs::path make_temp_home(ScopedEnvVar& home, ScopedEnvVar& pulp_home) {
    auto dir = unique_temp_dir("pulp-cli-audio-compare-home");
    fs::create_directories(dir);
    pulp_home.unset();
    home.set(dir.string());
    return dir;
}

// Install a fake managed run.sh that records its forwarded argv (one per line)
// to `record` and exits with `code`. Lets us assert the CLI's forwarding +
// exit-code passthrough with no Python dependency.
void install_fake_tool(const fs::path& home, const fs::path& record, int code) {
    auto wrapper_dir = home / ".pulp" / "tools" / "python-envs" / "audio-quality-lab";
    fs::create_directories(wrapper_dir);
    std::ofstream sh((wrapper_dir / "run.sh").string());
    sh << "#!/bin/sh\n"
       << ": > '" << record.string() << "'\n"
       << "for a in \"$@\"; do printf '%s\\n' \"$a\" >> '" << record.string() << "'; done\n"
       << "exit " << code << "\n";
    sh.close();
    fs::permissions(wrapper_dir / "run.sh",
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
}

std::string read_recorded(const fs::path& p) {
    std::ifstream f(p.string());
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("audio compare --help explains it is advisory, not a gate",
          "[cli][shellout][audio-compare]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"audio", "compare", "--help"});
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("advisory") != std::string::npos);
    REQUIRE(r.stdout_output.find("not a gate") != std::string::npos);
    REQUIRE(r.stdout_output.find("validate compare") != std::string::npos);  // points to the gate
}

TEST_CASE("audio compare needs two positional WAVs (exit 2)",
          "[cli][shellout][audio-compare]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto none = run_pulp({"audio", "compare"});
    REQUIRE(none.exit_code == 2);
    REQUIRE(none.stderr_output.find("reference.wav") != std::string::npos);
    // one positional + an option value is still only one positional → still short
    auto one = run_pulp({"audio", "compare", "only.wav", "--profile", "added-hf"});
    REQUIRE(one.exit_code == 2);
}

TEST_CASE("audio compare hints to install the opt-in tool when it is absent",
          "[cli][shellout][audio-compare]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    ScopedEnvVar home("HOME");
    ScopedEnvVar pulp_home("PULP_HOME");
    make_temp_home(home, pulp_home);  // empty — no managed wrapper present
    auto r = run_pulp({"audio", "compare", "a.wav", "b.wav"});
    REQUIRE(r.exit_code == 1);  // setup error, distinct from the tool's invalid=2
    REQUIRE(r.stderr_output.find("not installed") != std::string::npos);
    REQUIRE(r.stderr_output.find("pulp tool install audio-quality-lab") != std::string::npos);
}

#ifndef _WIN32
TEST_CASE("audio compare forwards the verb + flags and passes the tool's exit code",
          "[cli][shellout][audio-compare]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    ScopedEnvVar home("HOME");
    ScopedEnvVar pulp_home("PULP_HOME");
    auto dir = make_temp_home(home, pulp_home);
    auto record = dir / "forwarded-args.txt";
    install_fake_tool(dir, record, /*code=*/7);  // distinctive advisory-ish code to prove passthrough

    auto r = run_pulp({"audio", "compare", "ref.wav", "cand.wav",
                       "--profile", "added-hf", "--reference-role", "golden"});
    REQUIRE(r.exit_code == 7);  // the CLI passed the tool's exit code straight through

    const std::string fwd = read_recorded(record);
    // Exact forwarding: `compare` is prepended, then every original arg verbatim,
    // in order, with nothing inserted, dropped, or reordered.
    REQUIRE(fwd == "compare\nref.wav\ncand.wav\n"
                   "--profile\nadded-hf\n--reference-role\ngolden\n");
}
#endif
