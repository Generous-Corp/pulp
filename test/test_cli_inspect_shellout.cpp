// Focused shell-out coverage for `pulp inspect`.
//
// These tests launch the built CLI against a real authenticated, discoverable
// inspector session so command, output, and structured-error paths are covered.

#include "test_cli_shellout_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/authentication.hpp>
#include <pulp/inspect/discovery.hpp>
#include <pulp/inspect/inspector_server.hpp>
#include <pulp/inspect/protocol.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace pulp_test_cli;
using pulp::inspect::InspectorMessage;
using pulp::inspect::InspectorCapability;
using pulp::inspect::InspectorDiscoveryPublisher;
using pulp::inspect::InspectorDiscoveryRecord;
using pulp::inspect::InspectorPolicyConfig;
using pulp::inspect::InspectorProfile;
using pulp::inspect::InspectorServer;
using pulp::inspect::InspectorServerConfig;
using pulp::inspect::InspectorSession;
using pulp::inspect::InspectorSessionInfo;
using pulp::inspect::generate_inspector_secret;
using pulp::inspect::make_error;
using pulp::inspect::make_response;

namespace {

InspectorPolicyConfig fixture_policy() {
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Develop;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::SessionControl,
        InspectorCapability::StateRead,
        InspectorCapability::StateWrite,
        InspectorCapability::UiRead,
        InspectorCapability::DiagnosticsRead,
        InspectorCapability::LogsRead,
        InspectorCapability::AuthoringTweaks,
    };
    return policy;
}

struct InspectServerFixture {
    fs::path temp = unique_temp_dir("pulp-cli-inspect-shellout");
    ScopedEnvVar runtime_dir{"PULP_INSPECTOR_RUNTIME_DIR"};
    ScopedEnvVar update_disabled{"PULP_UPDATE_CHECK_DISABLED"};
    InspectorDiscoveryPublisher publisher{temp};
    InspectorPolicyConfig policy = fixture_policy();
    std::function<InspectorMessage(const InspectorMessage&)> handler;
    std::vector<InspectorMessage> seen;
    InspectorSession session{
        InspectorSessionInfo{
            "cli-shellout-session", "cli-shellout-instance",
            "com.pulp.cli-shellout", "1"},
        policy,
        [this](const InspectorMessage& request) {
            seen.push_back(request);
            return handler ? handler(request)
                           : make_response(request.id, "{}");
        }};
    InspectorServer server;
    std::uint16_t port = 0;

    InspectServerFixture() {
        fs::create_directories(temp);
#ifndef _WIN32
        REQUIRE(::chmod(temp.c_str(), 0700) == 0);
#endif
        runtime_dir.set(temp.string());
        update_disabled.set("1");
        const auto token = generate_inspector_secret();
        REQUIRE(token.has_value());
        InspectorDiscoveryRecord record;
        record.session_id = session.info().session_id;
        record.instance_id = session.info().instance_id;
        record.plugin_id = session.info().plugin_id;
        REQUIRE(server.start_authenticated(
            InspectorServerConfig{&session, &publisher, record, *token}));
        port = static_cast<std::uint16_t>(server.port());
    }

    ~InspectServerFixture() {
        server.stop();
        std::error_code ec;
        fs::remove_all(temp, ec);
    }

    std::string port_string() const { return std::to_string(port); }
};

std::string read_text_file(const fs::path& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

std::string compact_json_for_assertion(std::string text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c);
    }), text.end());
    return text;
}

}  // namespace

TEST_CASE("pulp inspect one-shot prints a server response",
          "[cli][shellout][inspect]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    InspectServerFixture fixture;
    fixture.handler = [&](const InspectorMessage& request) {
        return make_response(request.id,
                             R"({"ok":true,"source":"inspect-test"})");
    };

    auto result = run_pulp({"inspect",
                            "--host", "localhost",
                            "--port", fixture.port_string(),
                            "--command", "DOM.getDocument",
                            "--params", R"({"depth":2})"},
                           10000);

    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stderr_output.empty());
    REQUIRE(result.stdout_output.find("Connecting to 127.0.0.1:" +
                                      fixture.port_string()) !=
            std::string::npos);
    REQUIRE(result.stdout_output.find("Connected to inspector") !=
            std::string::npos);
    REQUIRE(compact_json_for_assertion(result.stdout_output)
                .find(R"({"ok":true,"source":"inspect-test"})") !=
            std::string::npos);
    REQUIRE(fixture.seen.size() == 1);
    REQUIRE(fixture.seen[0].id >= 2);
    REQUIRE(fixture.seen[0].method == "DOM.getDocument");
    REQUIRE(compact_json_for_assertion(fixture.seen[0].params_json) ==
            R"({"depth":2})");
}

TEST_CASE("pulp inspect one-shot can discover the advertised server port",
          "[cli][shellout][inspect]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    InspectServerFixture fixture;
    fixture.handler = [&](const InspectorMessage& request) {
        return make_response(request.id, R"({"discovered":true})");
    };

    auto result = run_pulp({"inspect", "--command", "DOM.getDocument"}, 10000);

    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stderr_output.empty());
    REQUIRE(result.stdout_output.find("Found inspector session " +
                                      fixture.session.info().session_id) !=
            std::string::npos);
    REQUIRE(result.stdout_output.find("Connecting to 127.0.0.1:" +
                                      fixture.port_string()) !=
            std::string::npos);
    REQUIRE(compact_json_for_assertion(result.stdout_output)
                .find(R"({"discovered":true})") !=
            std::string::npos);
    REQUIRE(fixture.seen.size() == 1);
    REQUIRE(fixture.seen[0].method == "DOM.getDocument");
    REQUIRE((fixture.seen[0].params_json.empty() ||
             fixture.seen[0].params_json == "{}"));
}

TEST_CASE("pulp inspect one-shot writes output files and propagates errors",
          "[cli][shellout][inspect]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    InspectServerFixture fixture;
    const auto out = fixture.temp / "inspect-response.json";

    fixture.handler = [&](const InspectorMessage& request) {
        if (request.method == "Inspector.getInfo")
            return make_error(request.id, "server rejected request");
        return make_response(request.id, R"({"file":true,"value":42})");
    };

    auto written = run_pulp({"inspect",
                             "--port", fixture.port_string(),
                             "--command", "State.getParameters",
                             "--output", out.string()},
                            10000);

    REQUIRE_FALSE(written.timed_out);
    REQUIRE(written.exit_code == 0);
    REQUIRE(written.stderr_output.empty());
    REQUIRE(written.stdout_output.find("Written to " + out.string()) !=
            std::string::npos);
    REQUIRE(fs::exists(out));
    REQUIRE(compact_json_for_assertion(read_text_file(out)) ==
            R"({"file":true,"value":42})");
    REQUIRE(fixture.seen.size() == 1);
    REQUIRE(fixture.seen[0].method == "State.getParameters");

    auto failed = run_pulp({"inspect",
                            "--port", fixture.port_string(),
                            "--command", "Inspector.getInfo",
                            "--output", (fixture.temp / "failed.json").string()},
                           10000);

    REQUIRE_FALSE(failed.timed_out);
    REQUIRE(failed.exit_code == 1);
    REQUIRE(failed.stderr_output.find("server rejected request") !=
            std::string::npos);
    REQUIRE_FALSE(fs::exists(fixture.temp / "failed.json"));
    REQUIRE(fixture.seen.size() == 2);
    REQUIRE(fixture.seen[1].method == "Inspector.getInfo");
}

TEST_CASE("pulp inspect validates missing values before connecting",
          "[cli][shellout][inspect]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");

    struct Case {
        std::vector<std::string> args;
        std::string diagnostic;
    };

    const std::vector<Case> cases = {
        {{"inspect", "--host"}, "--host requires a value"},
        {{"inspect", "--session"}, "--session requires a value"},
        {{"inspect", "--command"}, "--command requires a value"},
        {{"inspect", "--params"}, "--params requires a value"},
        {{"inspect", "--output"}, "--output requires a value"},
    };

    for (const auto& c : cases) {
        INFO(c.diagnostic);
        auto result = run_pulp(c.args, 10000);
        REQUIRE_FALSE(result.timed_out);
        REQUIRE(result.exit_code == 2);
        REQUIRE(result.stderr_output.find(c.diagnostic) != std::string::npos);
        REQUIRE(result.stdout_output.find("Connecting to") == std::string::npos);
    }

    auto invalid_negative = run_pulp({"inspect", "--port", "-1"}, 10000);
    REQUIRE_FALSE(invalid_negative.timed_out);
    REQUIRE(invalid_negative.exit_code == 2);
    REQUIRE(invalid_negative.stderr_output.find("invalid --port value: -1") !=
            std::string::npos);
    REQUIRE(invalid_negative.stdout_output.find("Connecting to") ==
            std::string::npos);
}
