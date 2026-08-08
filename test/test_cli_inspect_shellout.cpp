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
        InspectorCapability::AuthoringTweaks, InspectorCapability::CaptureImage,
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

} // namespace

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
        return make_response(request.id, "{}");
    };

    InspectorDiscoveryPublisher second_publisher{first.temp};
    auto second_policy = fixture_policy();
    std::vector<InspectorMessage> second_seen;
    InspectorSession second_session{
        InspectorSessionInfo{first.session.info().session_id, "cli-shellout-instance-b",
                             first.session.info().plugin_id, "1"},
        second_policy, [&](const InspectorMessage& request) {
            second_seen.push_back(request);
            return make_response(request.id, "{}");
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
        {"inspect", "capabilities", "--session", first.session.info().session_id, "--instance",
         second_session.info().instance_id, "--publication",
         second_publisher.record()->publication_id, "--json"},
        10000);

    INFO("stdout: " << result.stdout_output);
    INFO("stderr: " << result.stderr_output);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_output.find(second_publisher.record()->publication_id) !=
          std::string::npos);
    CHECK(first.seen.empty());
    CHECK(second_seen.empty());
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

    const auto stale = run_pulp({"inspect", "capabilities", "--session", fixture.session.info().session_id,
                                 "--instance", fixture.session.info().instance_id, "--publication",
                                 stale_publication},
                                10000);
    REQUIRE_FALSE(stale.timed_out);
    REQUIRE(stale.exit_code == 1);
    CHECK(stale.stderr_output.find("No live inspector session matches") != std::string::npos);
    CHECK(fixture.seen.empty());

    const auto current =
        run_pulp({"inspect", "capabilities", "--session", fixture.session.info().session_id, "--instance",
                  fixture.session.info().instance_id, "--publication", current_publication,
                  "--json"},
                 10000);
    REQUIRE_FALSE(current.timed_out);
    REQUIRE(current.exit_code == 0);
    CHECK(current.stdout_output.find("\"publicationId\": \"" + current_publication + "\"") !=
          std::string::npos);
    CHECK(fixture.seen.empty());
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
        {{"inspect", "--session"}, "--session requires a value"},
        {{"inspect", "--publication"}, "--publication requires a value"},
    };

    for (const auto& c : cases) {
        INFO(c.diagnostic);
        auto result = run_pulp(c.args, 10000);
        REQUIRE_FALSE(result.timed_out);
        REQUIRE(result.exit_code == 2);
        REQUIRE(result.stderr_output.find(c.diagnostic) != std::string::npos);
        REQUIRE(result.stdout_output.find("Connecting to") == std::string::npos);
    }

    for (const char* removed : {"--host", "--port", "--command", "--params", "--output"}) {
        auto result = run_pulp({"inspect", removed}, 10000);
        REQUIRE_FALSE(result.timed_out);
        REQUIRE(result.exit_code == 2);
        REQUIRE(result.stderr_output.find("unknown inspect argument") != std::string::npos);
    }

    auto unpaired_publication =
        run_pulp({"inspect", "capabilities", "--publication", "publication-a"}, 10000);
    REQUIRE_FALSE(unpaired_publication.timed_out);
    REQUIRE(unpaired_publication.exit_code == 2);
    REQUIRE(unpaired_publication.stderr_output.find(
                "--publication requires --session and --instance") != std::string::npos);
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
    CHECK(help.stdout_output.find("audit ARTIFACT [--json]") !=
          std::string::npos);

    const auto missing_argument = run_pulp({"inspect", "audit"}, 10000);
    REQUIRE_FALSE(missing_argument.timed_out);
    REQUIRE(missing_argument.exit_code == 2);
    CHECK(missing_argument.stderr_output.find("requires ARTIFACT") !=
          std::string::npos);

    const auto temp = unique_temp_dir("pulp-cli-inspect-audit");
    const auto absent = temp / "not-an-artifact";
    const auto blocked = run_pulp(
        {"inspect", "audit", absent.string(), "--json"}, 10000);
    REQUIRE_FALSE(blocked.timed_out);
    REQUIRE(blocked.exit_code == 1);
    const auto result = choc::json::parse(blocked.stdout_output);
    CHECK(result["schema"].getString() == "pulp.control.audit.v1");
    CHECK_FALSE(result["ok"].getBool());
    CHECK(result["verdict"].getString() == "block");
    CHECK_FALSE(fs::exists(absent));
    fs::remove_all(temp);
}
