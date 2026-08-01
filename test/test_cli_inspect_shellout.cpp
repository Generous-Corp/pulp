// Focused shell-out coverage for `pulp inspect`.
//
// These tests launch the built CLI against a real authenticated, discoverable
// inspector session so command, output, and structured-error paths are covered.

#include "test_cli_shellout_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>

#include <pulp/inspect/authentication.hpp>
#include <pulp/inspect/discovery.hpp>
#include <pulp/inspect/discovery_publisher.hpp>
#if PULP_TEST_INSPECT_DOMAIN_HANDLER
#include <pulp/inspect/domain_handler.hpp>
#endif
#include <pulp/inspect/inspector_server.hpp>
#include <pulp/inspect/protocol.hpp>
#if PULP_TEST_INSPECT_DOMAIN_HANDLER
#include <pulp/inspect/state_inspector.hpp>
#include <pulp/state/store.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
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

class FixtureMainThreadDispatcher {
public:
    FixtureMainThreadDispatcher() {
        worker_ = std::thread([this] { run(); });
        std::unique_lock lock(mutex_);
        ready_cv_.wait(lock, [this] { return ready_; });
    }

    ~FixtureMainThreadDispatcher() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        work_cv_.notify_all();
        worker_.join();
    }

    std::shared_ptr<pulp::inspect::InspectorMainThreadRpc> make_rpc() {
        return std::make_shared<pulp::inspect::InspectorMainThreadRpc>(
            pulp::inspect::InspectorMainThreadRpc::Config{},
            [this](std::function<void()> task) {
                if (!task)
                    return false;
                {
                    std::lock_guard lock(mutex_);
                    if (stopping_)
                        return false;
                    tasks_.push_back(std::move(task));
                }
                work_cv_.notify_one();
                return true;
            },
            [this] {
                std::lock_guard lock(mutex_);
                return ready_ && std::this_thread::get_id() == worker_id_;
            });
    }

private:
    void run() {
        {
            std::lock_guard lock(mutex_);
            worker_id_ = std::this_thread::get_id();
            ready_ = true;
        }
        ready_cv_.notify_all();
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                work_cv_.wait(lock, [this] {
                    return stopping_ || !tasks_.empty();
                });
                if (stopping_ && tasks_.empty())
                    return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_cv_;
    std::condition_variable work_cv_;
    std::deque<std::function<void()>> tasks_;
    std::thread worker_;
    std::thread::id worker_id_;
    bool ready_ = false;
    bool stopping_ = false;
};

InspectorPolicyConfig fixture_policy() {
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Develop;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::SessionControl,
        InspectorCapability::StateRead,
        InspectorCapability::StateWrite,
        InspectorCapability::TestInput,
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
    FixtureMainThreadDispatcher main_thread;
    std::shared_ptr<pulp::inspect::InspectorMainThreadRpc> rpc =
        main_thread.make_rpc();
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
        runtime_dir.set(temp.string());
        update_disabled.set("1");
        const auto token = generate_inspector_secret();
        REQUIRE(token.has_value());
        InspectorDiscoveryRecord record;
        record.session_id = session.info().session_id;
        record.instance_id = session.info().instance_id;
        record.plugin_id = session.info().plugin_id;
        InspectorServerConfig config;
        config.session = &session;
        config.discovery = &publisher;
        config.record = record;
        config.token = *token;
        config.main_thread_rpc = rpc;
        REQUIRE(server.start_authenticated(std::move(config)));
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

TEST_CASE("pulp inspect profiles and list provide stable JSON",
          "[cli][shellout][inspect][workflow]") {
    REQUIRE(binary_exists());

    const auto help = run_pulp({"inspect", "--help"}, 10000);
    REQUIRE_FALSE(help.timed_out);
    REQUIRE(help.exit_code == 0);
    CHECK(help.stdout_output.find(
              "--output FILE     Write a --command response to FILE") !=
          std::string::npos);
    CHECK(help.stdout_output.find("inject-midi --kind note_on|note_off") !=
          std::string::npos);
    CHECK(help.stdout_output.find("set-transport [--playing true|false]") !=
          std::string::npos);

    const auto profiles = run_pulp({"inspect", "profiles", "--json"}, 10000);
    REQUIRE_FALSE(profiles.timed_out);
    REQUIRE(profiles.exit_code == 0);
    CHECK(profiles.stdout_output.find("pulp.inspect.profiles.v1") !=
          std::string::npos);
    CHECK(profiles.stdout_output.find("\"develop\"") != std::string::npos);
    CHECK(profiles.stdout_output.find("state.write") != std::string::npos);

    InspectServerFixture fixture;
    const auto listed = run_pulp(
        {"inspect", "list", "--json", "--session",
         fixture.session.info().session_id, "--instance",
         fixture.session.info().instance_id},
        10000);
    REQUIRE_FALSE(listed.timed_out);
    REQUIRE(listed.exit_code == 0);
    CHECK(listed.stdout_output.find("pulp.inspect.sessions.v1") !=
          std::string::npos);
    CHECK(listed.stdout_output.find(fixture.session.info().session_id) !=
          std::string::npos);
    REQUIRE(fixture.publisher.record().has_value());
    CHECK(listed.stdout_output.find(
              fixture.publisher.record()->publication_id) !=
          std::string::npos);

    const auto human = run_pulp({"inspect", "list"}, 10000);
    REQUIRE_FALSE(human.timed_out);
    REQUIRE(human.exit_code == 0);
    CHECK(human.stdout_output.find("PUBLICATION") != std::string::npos);
    CHECK(human.stdout_output.find(
              fixture.publisher.record()->publication_id) !=
          std::string::npos);
}

TEST_CASE("pulp inspect exposes bounded typed MIDI and transport commands",
          "[cli][shellout][inspect][test-input]") {
    REQUIRE(binary_exists());

    InspectServerFixture fixture;
    fixture.handler = [] (const InspectorMessage& request) {
        if (request.method == "Test.injectMidi")
            return make_response(request.id, R"({"accepted":true})");
        if (request.method == "Test.setTransport")
            return make_response(request.id, R"({"applied":true})");
        return make_response(request.id, "{}");
    };
    REQUIRE(fixture.publisher.record().has_value());
    const auto publication = fixture.publisher.record()->publication_id;
    const std::vector<std::string> exact{
        "--session", fixture.session.info().session_id,
        "--instance", fixture.session.info().instance_id,
        "--publication", publication};

    auto run_exact = [&] (std::vector<std::string> args) {
        args.insert(args.end(), exact.begin(), exact.end());
        return run_pulp(args, 10000);
    };
    const auto note_on = run_exact({"inspect", "inject-midi", "--kind",
                                    "note_on", "--channel", "1", "--note",
                                    "60", "--velocity", "100", "--json"});
    REQUIRE_FALSE(note_on.timed_out);
    REQUIRE(note_on.exit_code == 0);
    CHECK(note_on.stdout_output.find("pulp.inspect.inject-midi.v1") !=
          std::string::npos);
    CHECK(note_on.stdout_output.find(publication) != std::string::npos);
    CHECK(note_on.stdout_output.find("\"accepted\": true") !=
          std::string::npos);

    const auto note_off = run_exact({"inspect", "inject-midi", "--kind",
                                     "note_off", "--channel", "1", "--note",
                                     "60", "--json"});
    REQUIRE_FALSE(note_off.timed_out);
    REQUIRE(note_off.exit_code == 0);

    const auto transport = run_exact({"inspect", "set-transport", "--playing",
                                      "true", "--position-samples", "0",
                                      "--tempo-bpm", "120", "--json"});
    REQUIRE_FALSE(transport.timed_out);
    REQUIRE(transport.exit_code == 0);
    CHECK(transport.stdout_output.find("pulp.inspect.set-transport.v1") !=
          std::string::npos);
    CHECK(transport.stdout_output.find("\"applied\": true") !=
          std::string::npos);

    const auto missing_velocity = run_pulp(
        {"inspect", "inject-midi", "--kind", "note_on", "--channel", "1",
         "--note", "60", "--json"},
        10000);
    REQUIRE(missing_velocity.exit_code == 2);
    CHECK(missing_velocity.stderr_output.find("note_on requires --velocity") !=
          std::string::npos);

    const auto empty_transport = run_pulp(
        {"inspect", "set-transport", "--json"}, 10000);
    REQUIRE(empty_transport.exit_code == 2);
    CHECK(empty_transport.stderr_output.find(
              "set-transport requires --playing, --position-samples, or --tempo-bpm") !=
          std::string::npos);
}

TEST_CASE("pulp inspect capabilities and doctor authenticate exact sessions",
          "[cli][shellout][inspect][workflow]") {
    REQUIRE(binary_exists());

    InspectServerFixture fixture;
    fixture.handler = [&](const InspectorMessage& request) {
        if (request.method == "Inspector.getAgentContext") {
            return make_response(
                request.id,
                "{\"binary\":{\"path\":\"/tmp/fixture\","
                "\"buildId\":\"fixture-build\",\"mtimeUnixMs\":1},"
                "\"identity\":{\"pluginId\":\"" +
                    fixture.session.info().plugin_id +
                    "\",\"sessionId\":\"" +
                    fixture.session.info().session_id +
                    "\",\"instanceId\":\"" +
                    fixture.session.info().instance_id +
                    "\"},\"editor\":{\"open\":true,\"windowVisible\":true},"
                    "\"processing\":{\"active\":true,\"xrunCount\":0},"
                    "\"hotReload\":{\"available\":true,\"enabled\":false,"
                    "\"pending\":false},\"unsavedTweakCount\":0,"
                    "\"actionableIssues\":[]}");
        }
        return make_response(request.id, "{}");
    };
    REQUIRE(fixture.publisher.record().has_value());
    const auto publication = fixture.publisher.record()->publication_id;
    const std::vector<std::string> exact{
        "--session", fixture.session.info().session_id,
        "--instance", fixture.session.info().instance_id,
        "--publication", publication};

    auto capability_args = std::vector<std::string>{
        "inspect", "capabilities", "--json"};
    capability_args.insert(capability_args.end(), exact.begin(), exact.end());
    const auto capabilities = run_pulp(capability_args, 10000);
    REQUIRE_FALSE(capabilities.timed_out);
    REQUIRE(capabilities.exit_code == 0);
    CHECK(capabilities.stdout_output.find(
              "pulp.inspect.capabilities.v1") != std::string::npos);
    CHECK(capabilities.stdout_output.find(publication) != std::string::npos);
    CHECK(capabilities.stdout_output.find("state.read") != std::string::npos);

    auto human_capability_args =
        std::vector<std::string>{"inspect", "capabilities"};
    human_capability_args.insert(
        human_capability_args.end(), exact.begin(), exact.end());
    const auto human_capabilities = run_pulp(human_capability_args, 10000);
    REQUIRE_FALSE(human_capabilities.timed_out);
    REQUIRE(human_capabilities.exit_code == 0);
    CHECK(human_capabilities.stdout_output.find("profile: develop") !=
          std::string::npos);
    CHECK(human_capabilities.stdout_output.find(publication) !=
          std::string::npos);
    CHECK(human_capabilities.stdout_output.find("state.write") !=
          std::string::npos);

    auto doctor_args = std::vector<std::string>{"inspect", "doctor", "--json"};
    doctor_args.insert(doctor_args.end(), exact.begin(), exact.end());
    const auto doctor = run_pulp(doctor_args, 10000);
    REQUIRE_FALSE(doctor.timed_out);
    REQUIRE(doctor.exit_code == 0);
    CHECK(doctor.stdout_output.find("pulp.inspect.doctor.v1") !=
          std::string::npos);
    CHECK(doctor.stdout_output.find("\"ready\": true") !=
          std::string::npos);
    CHECK(doctor.stdout_output.find("fixture") != std::string::npos);

    auto human_doctor_args = std::vector<std::string>{"inspect", "doctor"};
    human_doctor_args.insert(
        human_doctor_args.end(), exact.begin(), exact.end());
    const auto human_doctor = run_pulp(human_doctor_args, 10000);
    REQUIRE_FALSE(human_doctor.timed_out);
    REQUIRE(human_doctor.exit_code == 0);
    CHECK(human_doctor.stdout_output.find(
              "authenticated, capability policy available") !=
          std::string::npos);
}

TEST_CASE("pulp inspect JSON failures use the stable error envelope",
          "[cli][shellout][inspect][workflow][json-error]") {
    REQUIRE(binary_exists());

    const auto invalid_selector = run_pulp(
        {"inspect", "list", "--instance", "orphan", "--json"}, 10000);
    REQUIRE_FALSE(invalid_selector.timed_out);
    REQUIRE(invalid_selector.exit_code == 2);
    CHECK(invalid_selector.stdout_output.empty());
    CHECK(invalid_selector.stderr_output.find("pulp.inspect.error.v1") !=
          std::string::npos);
    CHECK(invalid_selector.stderr_output.find("invalid_selector") !=
          std::string::npos);

    {
        InspectServerFixture fixture;
        REQUIRE(fixture.publisher.record().has_value());
        const auto record = *fixture.publisher.record();
        {
            std::ofstream credential(record.credential_path,
                                     std::ios::binary | std::ios::trunc);
            REQUIRE(credential.good());
            credential << std::string(64, '0');
        }
        const auto auth = run_pulp(
            {"inspect", "capabilities", "--json",
             "--session", record.session_id,
             "--instance", record.instance_id,
             "--publication", record.publication_id},
            10000);
        REQUIRE_FALSE(auth.timed_out);
        REQUIRE(auth.exit_code == 1);
        INFO(auth.stderr_output);
        CHECK(auth.stderr_output.find("pulp.inspect.error.v1") !=
              std::string::npos);
        CHECK(auth.stderr_output.find("authentication_failed") !=
              std::string::npos);
    }

    {
        InspectServerFixture fixture;
        REQUIRE(fixture.publisher.record().has_value());
        const auto& record = *fixture.publisher.record();
        fixture.handler = [](const InspectorMessage& request) {
            return make_error(request.id, "context probe failed",
                              "context_failed", R"({"probe":"context"})");
        };
        const auto protocol = run_pulp(
            {"inspect", "doctor", "--json",
             "--session", record.session_id,
             "--instance", record.instance_id,
             "--publication", record.publication_id},
            10000);
        REQUIRE_FALSE(protocol.timed_out);
        REQUIRE(protocol.exit_code == 1);
        CHECK(protocol.stderr_output.find("pulp.inspect.error.v1") !=
              std::string::npos);
        CHECK(protocol.stderr_output.find("context_failed") !=
              std::string::npos);
        CHECK(protocol.stderr_output.find("\"probe\": \"context\"") !=
              std::string::npos);
    }

    {
        InspectServerFixture fixture;
        REQUIRE(fixture.publisher.record().has_value());
        const auto& record = *fixture.publisher.record();
        fixture.handler = [](const InspectorMessage& request) {
            return make_response(request.id, "not-json");
        };
        const auto invalid_response = run_pulp(
            {"inspect", "doctor", "--json",
             "--session", record.session_id,
             "--instance", record.instance_id,
             "--publication", record.publication_id},
            10000);
        REQUIRE_FALSE(invalid_response.timed_out);
        REQUIRE(invalid_response.exit_code == 1);
        CHECK(invalid_response.stderr_output.find("pulp.inspect.error.v1") !=
              std::string::npos);
        CHECK(invalid_response.stderr_output.find("invalid_response") !=
              std::string::npos);
    }

    {
        InspectServerFixture fixture;
        REQUIRE(fixture.publisher.record().has_value());
        const auto& record = *fixture.publisher.record();
        fixture.handler = [](const InspectorMessage& request) {
            return make_response(request.id, "not-json");
        };
        const auto invalid_response = run_pulp(
            {"inspect", "--command", "DOM.getDocument", "--json",
             "--session", record.session_id,
             "--instance", record.instance_id,
             "--publication", record.publication_id},
            10000);
        REQUIRE_FALSE(invalid_response.timed_out);
        REQUIRE(invalid_response.exit_code == 1);
        CHECK(invalid_response.stdout_output.empty());
        CHECK(invalid_response.stderr_output.find("pulp.inspect.error.v1") !=
              std::string::npos);
        CHECK(invalid_response.stderr_output.find("invalid_response") !=
              std::string::npos);
    }
}

TEST_CASE("pulp inspect live operations require exact selectors",
          "[cli][shellout][inspect][identity]") {
    REQUIRE(binary_exists());

    InspectServerFixture fixture;
    fixture.handler = [&](const InspectorMessage& request) {
        return make_response(request.id, R"({"discovered":true})");
    };

    REQUIRE(fixture.publisher.record().has_value());
    const auto& record = *fixture.publisher.record();

    const std::vector<std::vector<std::string>> incomplete_selectors = {
        {},
        {"--session", record.session_id},
        {"--session", record.session_id,
         "--instance", record.instance_id},
        {"--session", record.session_id,
         "--publication", record.publication_id},
        {"--instance", record.instance_id,
         "--publication", record.publication_id},
    };
    for (const auto& selectors : incomplete_selectors) {
        auto args = std::vector<std::string>{
            "inspect", "set-parameter", "--id", "7", "--value", "0.25",
            "--json"};
        args.insert(args.end(), selectors.begin(), selectors.end());
        const auto rejected = run_pulp(args, 10000);

        REQUIRE_FALSE(rejected.timed_out);
        REQUIRE(rejected.exit_code == 2);
        CHECK(rejected.stdout_output.empty());
        CHECK(rejected.stderr_output.find("pulp.inspect.error.v1") !=
              std::string::npos);
        CHECK(rejected.stderr_output.find("invalid_arguments") !=
              std::string::npos);
        CHECK(rejected.stderr_output.find(
                  "live inspect operations require --session, --instance, and --publication") !=
              std::string::npos);
        CHECK(fixture.seen.empty());
    }

    const auto wrong_identity = run_pulp(
        {"inspect", "set-parameter", "--id", "7", "--value", "0.25",
         "--json", "--session", record.session_id,
         "--instance", record.instance_id,
         "--publication", "wrong-publication"},
        10000);
    REQUIRE_FALSE(wrong_identity.timed_out);
    REQUIRE(wrong_identity.exit_code == 1);
    CHECK(wrong_identity.stdout_output.empty());
    CHECK(wrong_identity.stderr_output.find("pulp.inspect.error.v1") !=
          std::string::npos);
    CHECK(wrong_identity.stderr_output.find("session_selection_failed") !=
          std::string::npos);
    CHECK(fixture.seen.empty());

    auto result = run_pulp(
        {"inspect", "--command", "DOM.getDocument",
         "--session", record.session_id,
         "--instance", record.instance_id,
         "--publication", record.publication_id},
        10000);

    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stderr_output.find("Connected to inspector session " +
                                      fixture.session.info().session_id) !=
            std::string::npos);
    REQUIRE(result.stdout_output == R"({"discovered": true})"
                                    "\n");
    REQUIRE(fixture.seen.size() == 1);
    REQUIRE(fixture.seen[0].method == "DOM.getDocument");
    REQUIRE((fixture.seen[0].params_json.empty() ||
             fixture.seen[0].params_json == "{}"));
}

TEST_CASE("pulp inspect JSON output-file success uses a stable envelope",
          "[cli][shellout][inspect][json][output]") {
    REQUIRE(binary_exists());

    InspectServerFixture fixture;
    fixture.handler = [](const InspectorMessage& request) {
        return make_response(request.id, R"({"written":true})");
    };
    REQUIRE(fixture.publisher.record().has_value());
    const auto& record = *fixture.publisher.record();
    const auto out = fixture.temp / "inspect-json-response.json";
    const auto result = run_pulp(
        {"inspect", "--json",
         "--session", record.session_id,
         "--instance", record.instance_id,
         "--publication", record.publication_id,
         "--command", "State.getParameters",
         "--output", out.string()},
        10000);

    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_output.empty());
    CHECK(result.stdout_output.find("pulp.inspect.output.v1") !=
          std::string::npos);
    CHECK(result.stdout_output.find(out.string()) != std::string::npos);
    CHECK(result.stdout_output.find("Written to") == std::string::npos);
    REQUIRE(fs::exists(out));
    CHECK(compact_json_for_assertion(read_text_file(out)) ==
          R"({"written":true})");
}

TEST_CASE("pulp inspect one-shot prints a server response",
          "[cli][shellout][inspect]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    InspectServerFixture fixture;
    fixture.handler = [&](const InspectorMessage& request) {
        return make_response(request.id,
                             R"({"ok":true,"source":"inspect-test"})");
    };
    REQUIRE(fixture.publisher.record().has_value());
    const auto& record = *fixture.publisher.record();

    auto result = run_pulp({"inspect",
                            "--host", "localhost",
                            "--port", fixture.port_string(),
                            "--session", record.session_id,
                            "--instance", record.instance_id,
                            "--publication", record.publication_id,
                            "--command", "DOM.getDocument",
                            "--params", R"({"depth":2})"},
                           10000);

    INFO("stdout: " << result.stdout_output);
    INFO("stderr: " << result.stderr_output);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stderr_output.find("Connected to inspector session") !=
            std::string::npos);
    REQUIRE(result.stdout_output == R"({"ok": true, "source": "inspect-test"})"
                                    "\n");
    REQUIRE(fixture.seen.size() == 1);
    REQUIRE(fixture.seen[0].id >= 2);
    REQUIRE(fixture.seen[0].method == "DOM.getDocument");
    REQUIRE(compact_json_for_assertion(fixture.seen[0].params_json) ==
            R"({"depth":2})");
}

TEST_CASE("pulp inspect selects an exact instance within a shared session",
          "[cli][shellout][inspect][identity]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    InspectServerFixture first;
    first.handler = [&](const InspectorMessage& request) {
        return make_response(request.id, R"({"instance":"first"})");
    };

    InspectorDiscoveryPublisher second_publisher{first.temp};
    auto second_policy = fixture_policy();
    std::vector<InspectorMessage> second_seen;
    InspectorSession second_session{
        InspectorSessionInfo{
            first.session.info().session_id, "cli-shellout-instance-b",
            first.session.info().plugin_id, "1"},
        second_policy,
        [&](const InspectorMessage& request) {
            second_seen.push_back(request);
            return make_response(request.id, R"({"instance":"second"})");
        }};
    InspectorServer second_server;
    auto second_rpc = first.main_thread.make_rpc();
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = second_session.info().session_id;
    record.instance_id = second_session.info().instance_id;
    record.plugin_id = second_session.info().plugin_id;
    InspectorServerConfig second_config;
    second_config.session = &second_session;
    second_config.discovery = &second_publisher;
    second_config.record = record;
    second_config.token = *token;
    second_config.main_thread_rpc = second_rpc;
    REQUIRE(second_server.start_authenticated(std::move(second_config)));
    REQUIRE(second_publisher.record().has_value());

    const auto result = run_pulp(
        {"inspect",
         "--session", first.session.info().session_id,
         "--instance", second_session.info().instance_id,
         "--publication", second_publisher.record()->publication_id,
         "--command", "DOM.getDocument"},
        10000);

    INFO("stdout: " << result.stdout_output);
    INFO("stderr: " << result.stderr_output);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_output == R"({"instance": "second"})"
                                  "\n");
    CHECK(first.seen.empty());
    REQUIRE(second_seen.size() == 1);
    second_server.stop();
}

TEST_CASE("pulp inspect rejects a stale publication after server restart",
          "[cli][shellout][inspect][identity][generation]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    InspectServerFixture fixture;
    fixture.handler = [&](const InspectorMessage& request) {
        return make_response(request.id, "{}");
    };
    REQUIRE(fixture.publisher.record().has_value());
    const auto stale_publication =
        fixture.publisher.record()->publication_id;

    fixture.server.stop();
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = fixture.session.info().session_id;
    record.instance_id = fixture.session.info().instance_id;
    record.plugin_id = fixture.session.info().plugin_id;
    fixture.rpc = fixture.main_thread.make_rpc();
    InspectorServerConfig restarted_config;
    restarted_config.session = &fixture.session;
    restarted_config.discovery = &fixture.publisher;
    restarted_config.record = record;
    restarted_config.token = *token;
    restarted_config.main_thread_rpc = fixture.rpc;
    REQUIRE(fixture.server.start_authenticated(std::move(restarted_config)));
    fixture.port = static_cast<std::uint16_t>(fixture.server.port());
    REQUIRE(fixture.publisher.record().has_value());
    const auto current_publication =
        fixture.publisher.record()->publication_id;
    REQUIRE(current_publication != stale_publication);

    const auto stale = run_pulp(
        {"inspect",
         "--session", fixture.session.info().session_id,
         "--instance", fixture.session.info().instance_id,
         "--publication", stale_publication,
         "--command", "Session.getCapabilities"},
        10000);
    REQUIRE_FALSE(stale.timed_out);
    REQUIRE(stale.exit_code == 1);
    CHECK(stale.stderr_output.find("No live inspector session matches") !=
          std::string::npos);
    CHECK(fixture.seen.empty());

    const auto current = run_pulp(
        {"inspect",
         "--session", fixture.session.info().session_id,
         "--instance", fixture.session.info().instance_id,
         "--publication", current_publication,
         "--command", "Session.getCapabilities"},
        10000);
    REQUIRE_FALSE(current.timed_out);
    REQUIRE(current.exit_code == 0);
    CHECK(current.stdout_output.find(
              "\"publicationId\": \"" + current_publication + "\"") !=
          std::string::npos);
    CHECK(fixture.seen.empty());
}

TEST_CASE("pulp inspect one-shot acquires a controller for mutations",
          "[cli][shellout][inspect][controller]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    InspectServerFixture fixture;
    fixture.handler = [&](const InspectorMessage& request) {
        return make_response(request.id, R"({"applied":true})");
    };
    REQUIRE(fixture.publisher.record().has_value());
    const auto& record = *fixture.publisher.record();

    auto result = run_pulp({"inspect", "--port", fixture.port_string(),
                            "--session", record.session_id,
                            "--instance", record.instance_id,
                            "--publication", record.publication_id, "--command",
                            "State.setParameter", "--params", R"({"id":7,"value":0.75})"},
                           10000);

    INFO("stdout: " << result.stdout_output);
    INFO("stderr: " << result.stderr_output);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stdout_output == R"({"applied": true})"
                                    "\n");
    REQUIRE(fixture.seen.size() == 1);
    CHECK(fixture.seen.front().method == "State.setParameter");
    CHECK(compact_json_for_assertion(fixture.seen.front().params_json) ==
          R"({"id":7,"value":0.75})");

    result = run_pulp({"inspect", "--port", fixture.port_string(),
                       "--session", record.session_id,
                       "--instance", record.instance_id,
                       "--publication", record.publication_id, "--command",
                       "State.setParameter", "--params", R"({"id":7,"value":0.5})"},
                      10000);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    REQUIRE(fixture.seen.size() == 2);
    CHECK(compact_json_for_assertion(fixture.seen.back().params_json) == R"({"id":7,"value":0.5})");
}

#if PULP_TEST_INSPECT_DOMAIN_HANDLER
TEST_CASE("pulp inspect applies a typed parameter mutation through the production domain",
          "[cli][shellout][inspect][controller][state]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }

    pulp::state::StateStore store;
    pulp::state::ParamInfo gain;
    gain.id = 7;
    gain.name = "Gain";
    gain.range = {0.0f, 1.0f, 0.25f, 0.0f};
    store.add_parameter(gain);
    pulp::inspect::StateInspector state_inspector(store);
    pulp::inspect::DomainHandler domains;
    domains.set_state_inspector(&state_inspector);
    InspectServerFixture fixture;
    fixture.handler = [&](const InspectorMessage& request) { return domains.handle(request); };
    REQUIRE(fixture.publisher.record().has_value());
    const auto& record = *fixture.publisher.record();

    auto result = run_pulp({"inspect", "set-parameter", "--json",
                            "--id", "7", "--value", "0.75",
                            "--port", fixture.port_string(),
                            "--session", record.session_id,
                            "--instance", record.instance_id,
                            "--publication", record.publication_id},
                           10000);

    INFO("stdout: " << result.stdout_output);
    INFO("stderr: " << result.stderr_output);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    const auto envelope = choc::json::parse(result.stdout_output);
    REQUIRE(envelope["schemaVersion"].getString() ==
            "pulp.inspect.set-parameter.v1");
    REQUIRE(envelope["parameterId"].getWithDefault<int>(-1) == 7);
    REQUIRE(envelope["value"].getWithDefault<double>(-1.0) == 0.75);
    REQUIRE_FALSE(envelope["normalized"].getWithDefault<bool>(true));
    CHECK(store.get_value(7) == 0.75f);
    REQUIRE(fixture.seen.size() == 1);
    CHECK(fixture.seen.front().method == "State.setParameter");
    CHECK(compact_json_for_assertion(fixture.seen.front().params_json) ==
          R"({"id":7,"value":0.75,"normalized":false})");

    result = run_pulp({"inspect", "set-parameter", "--json",
                       "--id", "gain", "--value", "0.5",
                       "--port", fixture.port_string(),
                       "--session", record.session_id,
                       "--instance", record.instance_id,
                       "--publication", record.publication_id},
                      10000);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 2);
    CHECK(result.stderr_output.find("invalid --id value: gain") !=
          std::string::npos);
    CHECK(store.get_value(7) == 0.75f);
    REQUIRE(fixture.seen.size() == 1);
}
#endif

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
    REQUIRE(fixture.publisher.record().has_value());
    const auto& record = *fixture.publisher.record();

    const auto unwritable = run_pulp(
        {"inspect", "--json",
         "--port", fixture.port_string(),
         "--session", record.session_id,
         "--instance", record.instance_id,
         "--publication", record.publication_id,
         "--command", "State.setParameter",
         "--params", R"({"id":7,"value":0.5})",
         "--output", (fixture.temp / "missing" / "result.json").string()},
        10000);
    REQUIRE_FALSE(unwritable.timed_out);
    REQUIRE(unwritable.exit_code == 1);
    REQUIRE(unwritable.stderr_output.find("output_write_failed") !=
            std::string::npos);
    REQUIRE(fixture.seen.empty());

    auto written = run_pulp({"inspect",
                             "--port", fixture.port_string(),
                             "--session", record.session_id,
                             "--instance", record.instance_id,
                             "--publication", record.publication_id,
                             "--command", "State.getParameters",
                             "--output", out.string()},
                            10000);

    INFO("stdout: " << written.stdout_output);
    INFO("stderr: " << written.stderr_output);
    REQUIRE_FALSE(written.timed_out);
    REQUIRE(written.exit_code == 0);
    REQUIRE(written.stderr_output.find("Connected to inspector session") !=
            std::string::npos);
    REQUIRE(written.stdout_output.find("Written to " + out.string()) !=
            std::string::npos);
    REQUIRE(fs::exists(out));
    REQUIRE(compact_json_for_assertion(read_text_file(out)) ==
            R"({"file":true,"value":42})");
    REQUIRE(fixture.seen.size() == 1);
    REQUIRE(fixture.seen[0].method == "State.getParameters");

    auto failed = run_pulp({"inspect",
                            "--port", fixture.port_string(),
                            "--session", record.session_id,
                            "--instance", record.instance_id,
                            "--publication", record.publication_id,
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
        {{"inspect", "--publication"}, "--publication requires a value"},
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

    auto unmatched_port =
        run_pulp({"inspect", "--port", "1", "--command", "Motion.snapshot"},
                 10000);
    REQUIRE_FALSE(unmatched_port.timed_out);
    REQUIRE(unmatched_port.exit_code == 2);
    REQUIRE(unmatched_port.stderr_output.find("invalid_arguments") !=
            std::string::npos);
    REQUIRE(unmatched_port.stdout_output.empty());

    auto unpaired_publication =
        run_pulp({"inspect", "--publication", "publication-a"}, 10000);
    REQUIRE_FALSE(unpaired_publication.timed_out);
    REQUIRE(unpaired_publication.exit_code == 2);
    REQUIRE(unpaired_publication.stderr_output.find(
                "live inspect operations require --session, --instance, and --publication") !=
            std::string::npos);
}
