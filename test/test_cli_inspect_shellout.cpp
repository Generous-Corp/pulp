// Focused shell-out coverage for `pulp inspect`.
//
// These tests launch the built CLI against a real authenticated, discoverable
// inspector session so command, output, and structured-error paths are covered.

#include "test_cli_shellout_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

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

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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
using pulp::inspect::generate_inspector_secret;
using pulp::inspect::InspectorCapability;
using pulp::inspect::InspectorDiscoveryPublisher;
using pulp::inspect::InspectorDiscoveryRecord;
using pulp::inspect::InspectorMessage;
using pulp::inspect::InspectorPolicyConfig;
using pulp::inspect::InspectorProfile;
using pulp::inspect::InspectorServer;
using pulp::inspect::InspectorServerConfig;
using pulp::inspect::InspectorSession;
using pulp::inspect::InspectorSessionInfo;
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
                work_cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
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
        InspectorCapability::SessionDescribe, InspectorCapability::SessionControl,
        InspectorCapability::StateRead,       InspectorCapability::StateWrite,
        InspectorCapability::TestInput,       InspectorCapability::UiRead,
        InspectorCapability::DiagnosticsRead, InspectorCapability::LogsRead,
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
    std::shared_ptr<pulp::inspect::InspectorMainThreadRpc> rpc = main_thread.make_rpc();
    InspectorSession session{InspectorSessionInfo{"cli-shellout-session", "cli-shellout-instance",
                                                  "com.pulp.cli-shellout", "1"},
                             policy, [this](const InspectorMessage& request) {
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

    std::string port_string() const {
        return std::to_string(port);
    }
};

std::string read_text_file(const fs::path& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string compact_json_for_assertion(std::string text) {
    text.erase(
        std::remove_if(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); }),
        text.end());
    return text;
}

} // namespace

TEST_CASE("pulp inspect one-shot prints a server response", "[cli][shellout][inspect]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }

    InspectServerFixture fixture;
    fixture.handler = [&](const InspectorMessage& request) {
        return make_response(request.id, R"({"ok":true,"source":"inspect-test"})");
    };

    auto result = run_pulp({"inspect", "--host", "localhost", "--port", fixture.port_string(),
                            "--command", "DOM.getDocument", "--params", R"({"depth":2})"},
                           10000);

    INFO("stdout: " << result.stdout_output);
    INFO("stderr: " << result.stderr_output);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stderr_output.find("Connecting to 127.0.0.1:" + fixture.port_string()) !=
            std::string::npos);
    REQUIRE(result.stderr_output.find("Connected to inspector") != std::string::npos);
    REQUIRE(result.stdout_output == R"({"ok": true, "source": "inspect-test"})"
                                    "\n");
    REQUIRE(fixture.seen.size() == 1);
    REQUIRE(fixture.seen[0].id >= 2);
    REQUIRE(fixture.seen[0].method == "DOM.getDocument");
    REQUIRE(compact_json_for_assertion(fixture.seen[0].params_json) == R"({"depth":2})");
}

TEST_CASE("pulp inspect one-shot can discover the advertised server port",
          "[cli][shellout][inspect]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }

    InspectServerFixture fixture;
    fixture.handler = [&](const InspectorMessage& request) {
        return make_response(request.id, R"({"discovered":true})");
    };

    auto result = run_pulp({"inspect", "--command", "DOM.getDocument"}, 10000);

    INFO("stdout: " << result.stdout_output);
    INFO("stderr: " << result.stderr_output);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stderr_output.find("Found inspector session " +
                                      fixture.session.info().session_id) != std::string::npos);
    REQUIRE(result.stderr_output.find("Connecting to 127.0.0.1:" + fixture.port_string()) !=
            std::string::npos);
    REQUIRE(result.stdout_output == R"({"discovered": true})"
                                    "\n");
    REQUIRE(fixture.seen.size() == 1);
    REQUIRE(fixture.seen[0].method == "DOM.getDocument");
    REQUIRE((fixture.seen[0].params_json.empty() || fixture.seen[0].params_json == "{}"));
}

TEST_CASE("pulp inspect exposes bounded typed MIDI and transport commands",
          "[cli][shellout][inspect][test-input]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }

    InspectServerFixture fixture;
    fixture.handler = [](const InspectorMessage& request) {
        if (request.method == pulp::inspect::methods::kTestInjectMidi)
            return make_response(request.id, R"({"accepted":true})");
        if (request.method == pulp::inspect::methods::kTestSetTransport)
            return make_response(request.id, R"({"applied":true})");
        if (request.method == pulp::inspect::methods::kStateSetParameter)
            return make_response(request.id, R"({"applied":true})");
        return make_response(request.id, "{}");
    };
    REQUIRE(fixture.publisher.record().has_value());
    const auto& record = *fixture.publisher.record();
    const std::vector<std::string> exact{"--session",     record.session_id,
                                         "--instance",    record.instance_id,
                                         "--publication", record.publication_id};
    auto run_exact = [&](std::vector<std::string> command) {
        command.insert(command.end(), exact.begin(), exact.end());
        return run_pulp(command, 10000);
    };

    const auto parameter = run_exact(
        {"inspect", "set-parameter", "--id", "7", "--value", "0.25", "--normalized", "--json"});
    REQUIRE_FALSE(parameter.timed_out);
    REQUIRE(parameter.exit_code == 0);
    CHECK(parameter.stdout_output.find("pulp.inspect.set-parameter.v1") != std::string::npos);
    CHECK(parameter.stdout_output.find("\"parameterId\": 7") != std::string::npos);
    CHECK(parameter.stdout_output.find("\"normalized\": true") != std::string::npos);

    const auto note_on =
        run_exact({"inspect", "inject-midi", "--kind", "note_on", "--channel", "1", "--note", "60",
                   "--velocity", "100", "--duration-ms", "10", "--json"});
    REQUIRE_FALSE(note_on.timed_out);
    REQUIRE(note_on.exit_code == 0);
    CHECK(note_on.stdout_output.find("pulp.inspect.inject-midi.v1") != std::string::npos);
    CHECK(note_on.stdout_output.find(record.publication_id) != std::string::npos);
    CHECK(note_on.stdout_output.find("\"accepted\": true") != std::string::npos);
    CHECK(note_on.stdout_output.find("\"durationMs\": 10") != std::string::npos);

    const auto note_off = run_exact({"inspect", "inject-midi", "--kind", "note_off", "--channel",
                                     "1", "--note", "60", "--json"});
    REQUIRE_FALSE(note_off.timed_out);
    REQUIRE(note_off.exit_code == 0);

    const auto transport = run_exact({"inspect", "set-transport", "--playing", "true",
                                      "--position-samples", "0", "--tempo-bpm", "120", "--json"});
    REQUIRE_FALSE(transport.timed_out);
    REQUIRE(transport.exit_code == 0);
    CHECK(transport.stdout_output.find("pulp.inspect.set-transport.v1") != std::string::npos);
    CHECK(transport.stdout_output.find("\"applied\": true") != std::string::npos);

    const auto missing_velocity = run_pulp(
        {"inspect", "inject-midi", "--kind", "note_on", "--channel", "1", "--note", "60", "--json"},
        10000);
    REQUIRE(missing_velocity.exit_code == 2);
    CHECK(missing_velocity.stderr_output.find("note_on requires --velocity") != std::string::npos);

    const auto missing_duration =
        run_pulp({"inspect", "inject-midi", "--kind", "note_on", "--channel", "1", "--note", "60",
                  "--velocity", "100", "--json"},
                 10000);
    REQUIRE(missing_duration.exit_code == 2);
    CHECK(missing_duration.stderr_output.find("note_on requires --duration-ms") !=
          std::string::npos);

    const auto empty_transport = run_pulp({"inspect", "set-transport", "--json"}, 10000);
    REQUIRE(empty_transport.exit_code == 2);
    CHECK(empty_transport.stderr_output.find(
              "set-transport requires --playing, --position-samples, or --tempo-bpm") !=
          std::string::npos);

    const auto bad_parameter =
        run_pulp({"inspect", "set-parameter", "--id", "gain", "--value", "0.5", "--json"}, 10000);
    REQUIRE(bad_parameter.exit_code == 2);
    CHECK(bad_parameter.stderr_output.find("invalid --id value: gain") != std::string::npos);
}

TEST_CASE("pulp inspect named commands expose stable human and JSON contracts",
          "[cli][shellout][inspect][commands]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }

    InspectServerFixture fixture;
    REQUIRE(fixture.publisher.record().has_value());
    const auto& publication = *fixture.publisher.record();

    const auto profiles = run_pulp({"inspect", "profiles", "--json"}, 10000);
    REQUIRE_FALSE(profiles.timed_out);
    REQUIRE(profiles.exit_code == 0);
    const auto profiles_json = choc::json::parse(profiles.stdout_output);
    CHECK(profiles_json["schemaVersion"].getInt64() == 1);
    CHECK(profiles_json["profiles"].size() == 3);

    const auto listed = run_pulp({"inspect", "list", "--json"}, 10000);
    REQUIRE_FALSE(listed.timed_out);
    REQUIRE(listed.exit_code == 0);
    const auto list_json = choc::json::parse(listed.stdout_output);
    CHECK(list_json["schemaVersion"].getInt64() == 1);
    REQUIRE(list_json["sessions"].size() == 1);
    CHECK(list_json["sessions"][0]["sessionId"].getString() == publication.session_id);
    CHECK(list_json["sessions"][0]["instanceId"].getString() == publication.instance_id);
    CHECK(list_json["sessions"][0]["publicationId"].getString() == publication.publication_id);

    const auto capabilities = run_pulp(
        {"inspect", "capabilities", "--json", "--session", publication.session_id, "--instance",
         publication.instance_id, "--publication", publication.publication_id},
        10000);
    INFO(capabilities.stderr_output);
    REQUIRE_FALSE(capabilities.timed_out);
    REQUIRE(capabilities.exit_code == 0);
    const auto capabilities_json = choc::json::parse(capabilities.stdout_output);
    CHECK(capabilities_json["schemaVersion"].getInt64() == 1);
    CHECK(capabilities_json["sessionId"].getString() == publication.session_id);
    CHECK(capabilities_json["publicationId"].getString() == publication.publication_id);

    const auto human_capabilities =
        run_pulp({"inspect", "capabilities", "--session", publication.session_id, "--instance",
                  publication.instance_id, "--publication", publication.publication_id},
                 10000);
    REQUIRE(human_capabilities.exit_code == 0);
    CHECK(human_capabilities.stdout_output.find("Available:\n") != std::string::npos);
    CHECK(human_capabilities.stdout_output.find("Effective:\n") != std::string::npos);

    const auto doctor = run_pulp({"inspect", "doctor", "--json"}, 10000);
    REQUIRE_FALSE(doctor.timed_out);
    REQUIRE(doctor.exit_code == 0);
    const auto doctor_json = choc::json::parse(doctor.stdout_output);
    CHECK(doctor_json["schemaVersion"].getInt64() == 1);
    CHECK(doctor_json["ok"].getBool());
    CHECK(doctor_json["sessionCount"].getInt64() == 1);

    const auto human = run_pulp({"inspect", "list"}, 10000);
    REQUIRE_FALSE(human.timed_out);
    REQUIRE(human.exit_code == 0);
    CHECK(human.stdout_output.find(publication.session_id) != std::string::npos);
    CHECK(human.stdout_output.find(publication.publication_id) != std::string::npos);
}

TEST_CASE("pulp inspect treats an absent runtime directory as no live sessions",
          "[cli][shellout][inspect][commands][doctor]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }

    ScopedEnvVar runtime_dir("PULP_INSPECTOR_RUNTIME_DIR");
    const auto missing = unique_temp_dir("pulp-cli-inspect-absent");
    runtime_dir.set(missing.string());
    REQUIRE_FALSE(std::filesystem::exists(missing));

    const auto listed = run_pulp({"inspect", "list", "--json"}, 10000);
    REQUIRE(listed.exit_code == 0);
    const auto list_json = choc::json::parse(listed.stdout_output);
    CHECK(list_json["sessions"].size() == 0);

    const auto doctor = run_pulp({"inspect", "doctor", "--json"}, 10000);
    REQUIRE(doctor.exit_code == 0);
    const auto doctor_json = choc::json::parse(doctor.stdout_output);
    CHECK(doctor_json["ok"].getBool());
    CHECK(doctor_json["sessionCount"].getInt64() == 0);
    CHECK(doctor_json["issues"].size() == 0);
}

#if !defined(_WIN32)
TEST_CASE("pulp inspect doctor rejects an insecure runtime directory",
          "[cli][shellout][inspect][commands][doctor]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }

    InspectServerFixture fixture;
    REQUIRE(::chmod(fixture.temp.c_str(), 0755) == 0);
    const auto doctor = run_pulp({"inspect", "doctor", "--json"}, 10000);
    REQUIRE(::chmod(fixture.temp.c_str(), 0700) == 0);

    REQUIRE_FALSE(doctor.timed_out);
    REQUIRE(doctor.exit_code == 1);
    const auto result = choc::json::parse(doctor.stdout_output);
    CHECK_FALSE(result["ok"].getBool());
    REQUIRE(result["issues"].size() == 1);
    CHECK(result["issues"][0].getString() == "runtime directory is not an owner-private directory");

    REQUIRE(::chmod(fixture.temp.c_str(), 0755) == 0);
    const auto listed = run_pulp({"inspect", "list", "--json"}, 10000);
    REQUIRE(::chmod(fixture.temp.c_str(), 0700) == 0);
    REQUIRE(listed.exit_code == 1);
    const auto list_result = choc::json::parse(listed.stdout_output);
    CHECK_FALSE(list_result["ok"].getBool());
    CHECK(list_result["error"]["code"].getString() == "discovery_unavailable");
}
#endif

TEST_CASE("pulp inspect capabilities requires one exact publication",
          "[cli][shellout][inspect][commands][identity]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }

    ScopedEnvVar update_disabled("PULP_UPDATE_CHECK_DISABLED");
    update_disabled.set("1");
    const auto result = run_pulp({"inspect", "capabilities", "--json"}, 10000);
    REQUIRE_FALSE(result.timed_out);
    CHECK(result.exit_code == 2);
    CHECK(result.stderr_output.find("requires --session, --instance, and --publication") !=
          std::string::npos);

    ScopedEnvVar runtime_dir("PULP_INSPECTOR_RUNTIME_DIR");
    runtime_dir.set(unique_temp_dir("pulp-cli-inspect-empty").string());
    const auto missing = run_pulp({"inspect", "capabilities", "--json", "--session", "missing",
                                   "--instance", "missing", "--publication", "missing"},
                                  10000);
    REQUIRE_FALSE(missing.timed_out);
    REQUIRE(missing.exit_code == 1);
    const auto error = choc::json::parse(missing.stdout_output);
    CHECK(error["schemaVersion"].getInt64() == 1);
    CHECK_FALSE(error["ok"].getBool());
    CHECK(error["error"]["code"].getString() == "selection_failed");
    CHECK(missing.stderr_output.empty());
}

TEST_CASE("pulp inspect selects an exact instance within a shared session",
          "[cli][shellout][inspect][identity]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }

    InspectServerFixture first;
    first.handler = [&](const InspectorMessage& request) {
        return make_response(request.id, R"({"instance":"first"})");
    };

    InspectorDiscoveryPublisher second_publisher{first.temp};
    auto second_policy = fixture_policy();
    std::vector<InspectorMessage> second_seen;
    InspectorSession second_session{
        InspectorSessionInfo{first.session.info().session_id, "cli-shellout-instance-b",
                             first.session.info().plugin_id, "1"},
        second_policy, [&](const InspectorMessage& request) {
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

    const auto result =
        run_pulp({"inspect", "--session", first.session.info().session_id, "--instance",
                  second_session.info().instance_id, "--command", "DOM.getDocument"},
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
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }

    InspectServerFixture fixture;
    fixture.handler = [&](const InspectorMessage& request) {
        return make_response(request.id, "{}");
    };
    REQUIRE(fixture.publisher.record().has_value());
    const auto stale_publication = fixture.publisher.record()->publication_id;

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
    const auto current_publication = fixture.publisher.record()->publication_id;
    REQUIRE(current_publication != stale_publication);

    const auto stale = run_pulp({"inspect", "--session", fixture.session.info().session_id,
                                 "--instance", fixture.session.info().instance_id, "--publication",
                                 stale_publication, "--command", "Session.getCapabilities"},
                                10000);
    REQUIRE_FALSE(stale.timed_out);
    REQUIRE(stale.exit_code == 1);
    CHECK(stale.stderr_output.find("No live inspector session matches") != std::string::npos);
    CHECK(fixture.seen.empty());

    const auto current =
        run_pulp({"inspect", "--session", fixture.session.info().session_id, "--instance",
                  fixture.session.info().instance_id, "--publication", current_publication,
                  "--command", "Session.getCapabilities"},
                 10000);
    REQUIRE_FALSE(current.timed_out);
    REQUIRE(current.exit_code == 0);
    CHECK(current.stdout_output.find("\"publicationId\": \"" + current_publication + "\"") !=
          std::string::npos);
    CHECK(fixture.seen.empty());
}

TEST_CASE("pulp inspect one-shot acquires a controller for mutations",
          "[cli][shellout][inspect][controller]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }

    InspectServerFixture fixture;
    fixture.handler = [&](const InspectorMessage& request) {
        return make_response(request.id, R"({"applied":true})");
    };

    auto result = run_pulp({"inspect", "--port", fixture.port_string(), "--command",
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

    result = run_pulp({"inspect", "--port", fixture.port_string(), "--command",
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

    auto result = run_pulp({"inspect", "--port", fixture.port_string(), "--command",
                            "State.setParameter", "--params", R"({"id":7,"value":0.75})"},
                           10000);

    INFO("stdout: " << result.stdout_output);
    INFO("stderr: " << result.stderr_output);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stdout_output == R"({"ok": true})"
                                    "\n");
    CHECK(store.get_value(7) == 0.75f);
    REQUIRE(fixture.seen.size() == 1);
    CHECK(fixture.seen.front().method == "State.setParameter");
    CHECK(compact_json_for_assertion(fixture.seen.front().params_json) ==
          R"({"id":7,"value":0.75})");

    result = run_pulp({"inspect", "--port", fixture.port_string(), "--command",
                       "State.setParameter", "--params", R"({"id":"gain","value":0.5})"},
                      10000);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 1);
    CHECK(result.stderr_output.find("Invalid params for State.setParameter") != std::string::npos);
    CHECK(store.get_value(7) == 0.75f);
    REQUIRE(fixture.seen.size() == 2);
}
#endif

TEST_CASE("pulp inspect one-shot writes output files and propagates errors",
          "[cli][shellout][inspect]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }

    InspectServerFixture fixture;
    const auto out = fixture.temp / "inspect-response.json";

    fixture.handler = [&](const InspectorMessage& request) {
        if (request.method == "Inspector.getInfo")
            return make_error(request.id, "server rejected request");
        return make_response(request.id, R"({"file":true,"value":42})");
    };

    auto written = run_pulp({"inspect", "--port", fixture.port_string(), "--command",
                             "State.getParameters", "--output", out.string()},
                            10000);

    INFO("stdout: " << written.stdout_output);
    INFO("stderr: " << written.stderr_output);
    REQUIRE_FALSE(written.timed_out);
    REQUIRE(written.exit_code == 0);
    REQUIRE(written.stderr_output.find("Connecting to 127.0.0.1:" + fixture.port_string()) !=
            std::string::npos);
    REQUIRE(written.stderr_output.find("Connected to inspector") != std::string::npos);
    REQUIRE(written.stdout_output.find("Written to " + out.string()) != std::string::npos);
    REQUIRE(fs::exists(out));
    REQUIRE(compact_json_for_assertion(read_text_file(out)) == R"({"file":true,"value":42})");
    REQUIRE(fixture.seen.size() == 1);
    REQUIRE(fixture.seen[0].method == "State.getParameters");

    auto failed =
        run_pulp({"inspect", "--port", fixture.port_string(), "--command", "Inspector.getInfo",
                  "--output", (fixture.temp / "failed.json").string()},
                 10000);

    REQUIRE_FALSE(failed.timed_out);
    REQUIRE(failed.exit_code == 1);
    REQUIRE(failed.stderr_output.find("server rejected request") != std::string::npos);
    REQUIRE_FALSE(fs::exists(fixture.temp / "failed.json"));
    REQUIRE(fixture.seen.size() == 2);
    REQUIRE(fixture.seen[1].method == "Inspector.getInfo");
}

TEST_CASE("pulp inspect validates missing values before connecting", "[cli][shellout][inspect]") {
    if (!binary_exists()) {
        SUCCEED("skipped: pulp not built");
        return;
    }

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
    REQUIRE(invalid_negative.stderr_output.find("invalid --port value: -1") != std::string::npos);
    REQUIRE(invalid_negative.stdout_output.find("Connecting to") == std::string::npos);

    auto unmatched_port =
        run_pulp({"inspect", "--port", "1", "--command", "Motion.snapshot"}, 10000);
    REQUIRE_FALSE(unmatched_port.timed_out);
    REQUIRE(unmatched_port.exit_code == 1);
    REQUIRE(unmatched_port.stderr_output.find("requested port 1") != std::string::npos);
    REQUIRE(unmatched_port.stdout_output.empty());

    auto unpaired_publication = run_pulp({"inspect", "--publication", "publication-a"}, 10000);
    REQUIRE_FALSE(unpaired_publication.timed_out);
    REQUIRE(unpaired_publication.exit_code == 2);
    REQUIRE(unpaired_publication.stderr_output.find(
                "--publication requires --session and --instance") != std::string::npos);
}
