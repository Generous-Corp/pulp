// Focused shell-out coverage for the reduced `pulp inspect` surface.

#include "test_cli_shellout_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <choc/text/choc_JSON.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace pulp_test_cli;

TEST_CASE("pulp control profiles is canonical and inspect profiles is a dated alias",
          "[cli][shellout][inspect][control][profiles]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }

    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");

    const auto canonical = run_pulp({"control", "profiles", "--json"}, 10000);
    INFO(canonical.stderr_output);
    REQUIRE_FALSE(canonical.timed_out);
    REQUIRE(canonical.exit_code == 0);
    const auto profiles_json = choc::json::parse(canonical.stdout_output);
    CHECK(profiles_json["schemaVersion"].getInt64() == 1);
    REQUIRE(profiles_json["profiles"].size() == 3);
    CHECK(profiles_json["profiles"][0]["id"].getString() == "off");
    CHECK(profiles_json["profiles"][1]["id"].getString() == "observe");
    CHECK(profiles_json["profiles"][2]["id"].getString() == "develop");

    const auto alias = run_pulp({"inspect", "profiles", "--json"}, 10000);
    INFO(alias.stderr_output);
    REQUIRE_FALSE(alias.timed_out);
    REQUIRE(alias.exit_code == 0);
    CHECK(alias.stdout_output == canonical.stdout_output);
    CHECK(alias.stderr_output.find("deprecated; use `pulp control profiles`") !=
          std::string::npos);
    CHECK(alias.stderr_output.find("Pulp 0.800.0 on 2026-10-01") != std::string::npos);

    const auto human = run_pulp({"control", "profiles"}, 10000);
    REQUIRE_FALSE(human.timed_out);
    REQUIRE(human.exit_code == 0);
    CHECK(human.stdout_output.find("off\n") != std::string::npos);
    CHECK(human.stdout_output.find("observe\n") != std::string::npos);
    CHECK(human.stdout_output.find("develop\n") != std::string::npos);

    const auto rejected = run_pulp({"control", "profiles", "--instance", "wrong"}, 10000);
    REQUIRE_FALSE(rejected.timed_out);
    CHECK(rejected.exit_code == 2);
    CHECK(rejected.stderr_output.find("profiles accepts only --json") != std::string::npos);

    const auto rejected_default_params =
        run_pulp({"control", "profiles", "--params", "{}"}, 10000);
    REQUIRE_FALSE(rejected_default_params.timed_out);
    CHECK(rejected_default_params.exit_code == 2);
}

TEST_CASE("pulp inspect rejects retired authority and arbitrary commands",
          "[cli][shellout][inspect][authority]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }

    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");

    for (const char* retired : {"list", "capabilities", "doctor"}) {
        const auto result = run_pulp({"inspect", retired}, 10000);
        INFO(retired << ": " << result.stderr_output);
        REQUIRE_FALSE(result.timed_out);
        CHECK(result.exit_code == 2);
        CHECK(result.stderr_output.find("unknown inspect command") != std::string::npos);
    }

    for (const char* selector :
         {"--host", "--port", "--session", "--instance", "--publication", "--output"}) {
        const auto result = run_pulp({"inspect", selector}, 10000);
        INFO(selector << ": " << result.stderr_output);
        REQUIRE_FALSE(result.timed_out);
        CHECK(result.exit_code == 2);
        CHECK(result.stderr_output.find("unknown inspect argument") != std::string::npos);
    }

    const auto arbitrary = run_pulp(
        {"inspect", "--command", "Runtime.evaluate", "--params", "{}"}, 10000);
    REQUIRE_FALSE(arbitrary.timed_out);
    CHECK(arbitrary.exit_code == 2);
    CHECK(arbitrary.stderr_output.find("accepts only Trace.startSession or Trace.stopSession") !=
          std::string::npos);

    const auto missing_command_value = run_pulp({"inspect", "--command"}, 10000);
    REQUIRE_FALSE(missing_command_value.timed_out);
    CHECK(missing_command_value.exit_code == 2);
    CHECK(missing_command_value.stderr_output.find("--command requires a value") !=
          std::string::npos);

    const auto unpaired_params = run_pulp({"inspect", "--params", "{}"}, 10000);
    REQUIRE_FALSE(unpaired_params.timed_out);
    CHECK(unpaired_params.exit_code == 2);
    CHECK(unpaired_params.stderr_output.find("--params requires --command") !=
          std::string::npos);
}

TEST_CASE("pulp inspect canonical Trace bridge is narrow and default denied",
          "[cli][shellout][inspect][trace][control]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }

    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");

    for (const char* method : {"Trace.startSession", "Trace.stopSession"}) {
        const auto result =
            run_pulp({"inspect", "--command", method, "--params", "{}"}, 10000);
        INFO(method << ": " << result.stderr_output);
        REQUIRE_FALSE(result.timed_out);
        CHECK(result.exit_code == 1);
        CHECK(result.stderr_output.find("control_session_unavailable") != std::string::npos);
    }
}

TEST_CASE("pulp inspect audit is read only and fails closed before activation",
          "[cli][shellout][inspect][audit]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }

    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");

    const auto help = run_pulp({"inspect", "audit", "--help"}, 10000);
    REQUIRE_FALSE(help.timed_out);
    REQUIRE(help.exit_code == 0);
    CHECK(help.stdout_output.find("audit ARTIFACT [--json]") != std::string::npos);

    const auto missing_argument = run_pulp({"inspect", "audit"}, 10000);
    REQUIRE_FALSE(missing_argument.timed_out);
    REQUIRE(missing_argument.exit_code == 2);
    CHECK(missing_argument.stderr_output.find("requires ARTIFACT") != std::string::npos);

    const auto temp = unique_temp_dir("pulp-cli-inspect-audit");
    const auto absent = temp / "not-an-artifact";
    const auto blocked = run_pulp({"inspect", "audit", absent.string(), "--json"}, 10000);
    REQUIRE_FALSE(blocked.timed_out);
    REQUIRE(blocked.exit_code == 1);
    const auto result = choc::json::parse(blocked.stdout_output);
    CHECK(result["schema"].getString() == "pulp.control.audit.v1");
    CHECK_FALSE(result["ok"].getBool());
    CHECK(result["verdict"].getString() == "block");
    CHECK_FALSE(fs::exists(absent));
    fs::remove_all(temp);
}
