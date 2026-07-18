// osc-render-wav CLI smoke: shells out to the built tool and asserts exit
// code 0 plus a non-empty WAV, once per `--engine` value and once for
// `--seed`. The bridge library (osc_wav_scenario.*) already has API-level
// coverage in test_wav_bridge.cpp and the Python quality-lab bridge test;
// this exercises the argv surface those two do not reach — per CLAUDE.md's
// "CLI behavior changes" testing rule (shell out, assert exit code).

#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include "test_cli_shellout_util.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path tool_binary() {
#if defined(PULP_OSC_RENDER_WAV_BINARY)
    return fs::path(PULP_OSC_RENDER_WAV_BINARY);
#else
    return {};
#endif
}

struct TempFile {
    fs::path path;
    explicit TempFile(const std::string& name) {
        path = fs::temp_directory_path() /
               (name + "-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()) +
                ".wav");
    }
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

pulp::platform::ProcessResult run_tool(const std::vector<std::string>& args) {
    pulp::platform::ProcessOptions options;
    // Same shared hang-guard other CLI shell-out suites use; see
    // test_cli_shellout_util.hpp — a real render here takes milliseconds.
    options.timeout_ms = pulp_test_cli::shellout_timeout_ms();
    return pulp::platform::ChildProcess::run(tool_binary().string(), args, options);
}

}  // namespace

TEST_CASE("osc-render-wav writes a non-empty WAV for every --engine",
          "[cli][osc-render-wav]") {
    if (tool_binary().empty()) {
        SUCCEED("pulp-osc-render-wav not built");
        return;
    }

    for (const std::string& engine : {"vco", "dco", "wt"}) {
        TempFile out("osc-engine-" + engine);
        const auto result =
            run_tool({"--out", out.path.string(), "--engine", engine,
                      "--dur-ms", "10"});
        INFO("engine=" << engine << " stdout=" << result.stdout_output
                        << " stderr=" << result.stderr_output);
        REQUIRE(result.exit_code == 0);

        std::error_code ec;
        REQUIRE(fs::exists(out.path, ec));
        REQUIRE(fs::file_size(out.path, ec) > 0);
    }
}

TEST_CASE("osc-render-wav writes a non-empty WAV with --seed",
          "[cli][osc-render-wav]") {
    if (tool_binary().empty()) {
        SUCCEED("pulp-osc-render-wav not built");
        return;
    }

    TempFile out("osc-seed");
    const auto result =
        run_tool({"--out", out.path.string(), "--engine", "vco",
                  "--jitter-cents", "10", "--seed", "42", "--dur-ms", "10"});
    INFO("stdout=" << result.stdout_output
                    << " stderr=" << result.stderr_output);
    REQUIRE(result.exit_code == 0);

    std::error_code ec;
    REQUIRE(fs::exists(out.path, ec));
    REQUIRE(fs::file_size(out.path, ec) > 0);
}

TEST_CASE("osc-render-wav rejects --seed on a non-vco engine",
          "[cli][osc-render-wav]") {
    if (tool_binary().empty()) {
        SUCCEED("pulp-osc-render-wav not built");
        return;
    }

    TempFile out("osc-seed-rejected");
    const auto result =
        run_tool({"--out", out.path.string(), "--engine", "dco", "--seed",
                  "42"});
    REQUIRE(result.exit_code == 2);
    REQUIRE_FALSE(result.stderr_output.empty());

    std::error_code ec;
    REQUIRE_FALSE(fs::exists(out.path, ec));
}
