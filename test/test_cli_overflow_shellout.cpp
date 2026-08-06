#include "test_cli_shellout_helpers.hpp"

using namespace pulp_test_cli;
namespace fs = std::filesystem;

TEST_CASE("pulp overflow validates non-mutating operator arguments", "[cli][shellout][overflow]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }

    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");

    auto help = run_pulp({"overflow", "--help"}, 10000);
    REQUIRE_FALSE(help.timed_out);
    REQUIRE(help.exit_code == 0);
    REQUIRE(help.stdout_output.find("pulp overflow") != std::string::npos);
    REQUIRE(help.stdout_output.find("threshold [N]") != std::string::npos);
    REQUIRE(help.stdout_output.find("PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON") != std::string::npos);
    REQUIRE(help.stdout_output.find("local-only") != std::string::npos);
    REQUIRE(help.stdout_output.find("Unsetting the variable") != std::string::npos);
    REQUIRE(help.stdout_output.find("namespace-profile") == std::string::npos);

    auto unknown = run_pulp({"overflow", "wat"}, 10000);
    REQUIRE_FALSE(unknown.timed_out);
    REQUIRE(unknown.exit_code == 1);
    REQUIRE(unknown.stderr_output.find("unknown subcommand") != std::string::npos);

    auto threshold_extra = run_pulp({"overflow", "threshold", "1", "2"}, 10000);
    REQUIRE_FALSE(threshold_extra.timed_out);
    REQUIRE(threshold_extra.exit_code == 1);
    REQUIRE(threshold_extra.stderr_output.find("too many args") != std::string::npos);

    auto threshold_negative = run_pulp({"overflow", "threshold", "-1"}, 10000);
    REQUIRE_FALSE(threshold_negative.timed_out);
    REQUIRE(threshold_negative.exit_code == 1);
    REQUIRE(threshold_negative.stderr_output.find("must be >= 0") != std::string::npos);

    auto threshold_bad = run_pulp({"overflow", "threshold", "nope"}, 10000);
    REQUIRE_FALSE(threshold_bad.timed_out);
    REQUIRE(threshold_bad.exit_code == 1);
    REQUIRE(threshold_bad.stderr_output.find("is not a number") != std::string::npos);

    auto enable_missing_to = run_pulp({"overflow", "enable", "--to"}, 10000);
    REQUIRE_FALSE(enable_missing_to.timed_out);
    REQUIRE(enable_missing_to.exit_code == 2);
    REQUIRE(enable_missing_to.stderr_output.find("--to requires a value") != std::string::npos);

    auto enable_flag_value = run_pulp({"overflow", "enable", "--to", "--flag"}, 10000);
    REQUIRE_FALSE(enable_flag_value.timed_out);
    REQUIRE(enable_flag_value.exit_code == 2);
    REQUIRE(enable_flag_value.stderr_output.find("--to requires a value") != std::string::npos);

    for (const auto& sentinel : {"local-only", "\"local-only\"", "  \"local-only\"  ",
                                 "[\"local-only\"]", "\"local\\u002donly\""}) {
        auto result = run_pulp({"overflow", "enable", "--to", sentinel}, 10000);
        REQUIRE_FALSE(result.timed_out);
        REQUIRE(result.exit_code == 2);
        REQUIRE(result.stderr_output.find("pulp overflow disable") != std::string::npos);
    }

    for (const auto& invalid :
         {"null", "{}", "[]", "[\"macos-15\", 2]", "[\" \"]", "\"macos-15\" trailing",
          "\"macos-15\"}garbage", "\"macos\\q15\"", "\"macos-15\"]garbage", "\"macos-15",
          "[\"macos-15\"", "[\"macos-15\",]", "\"macos\\u007f15\"", "\"macos\\u008015\"",
          "\"macos-15 \"", "\"macos 15\"", "\"macos\\u00a015\"", "\"macos-\\u202e15\""}) {
        INFO(invalid);
        auto result = run_pulp({"overflow", "enable", "--to", invalid}, 10000);
        REQUIRE_FALSE(result.timed_out);
        REQUIRE(result.exit_code == 2);
    }
}

TEST_CASE("pulp overflow disable writes the bare local-only sentinel",
          "[cli][shellout][overflow]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }

    auto fixture = unique_temp_dir("pulp-overflow-gh");
    auto log = fixture / "gh-args.txt";
#if defined(_WIN32)
    write_text(fixture / "gh.bat", "@echo off\r\n"
                                   "echo %*>>\"%PULP_OVERFLOW_TEST_LOG%\"\r\n"
                                   "if \"%1\"==\"api\" exit /b 1\r\n");
#else
    auto shim = fixture / "gh";
    write_text(shim, "#!/bin/sh\n"
                     "printf '%s\\n' \"$*\" >> \"$PULP_OVERFLOW_TEST_LOG\"\n"
                     "if [ \"$1\" = api ]; then exit 1; fi\n");
    fs::permissions(shim, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace);
#endif

    ScopedEnvVar path("PATH");
    const auto old_path = std::getenv("PATH") ? std::getenv("PATH") : "";
#if defined(_WIN32)
    path.set(fixture.string() + ";" + old_path);
#else
    path.set(fixture.string() + ":" + old_path);
#endif
    ScopedEnvVar log_path("PULP_OVERFLOW_TEST_LOG");
    log_path.set(log.string());
    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");
    auto probe = run_pulp({"overflow", "status"}, 10000);
    auto probe_args = read_file(log);
    REQUIRE_FALSE(probe.timed_out);
    REQUIRE(probe_args.find("repos/Generous-Corp/pulp/actions/variables/") != std::string::npos);

    auto result = run_pulp({"overflow", "disable"}, 10000);
    auto enable = run_pulp({"overflow", "enable", "--to", "\"macos-15\""}, 10000);
    auto args = read_file(log);
    fs::remove_all(fixture);

    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(enable.timed_out);
    REQUIRE(enable.exit_code == 0);
    REQUIRE(args.find("variable set PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON") != std::string::npos);
    REQUIRE(args.find("--repo Generous-Corp/pulp") != std::string::npos);
    REQUIRE(args.find("--body") != std::string::npos);
    REQUIRE(args.find("local-only") != std::string::npos);
    REQUIRE(args.find("macos-15") != std::string::npos);
}
