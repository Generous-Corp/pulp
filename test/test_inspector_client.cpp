#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/authentication.hpp>
#include <pulp/inspect/client.hpp>
#include <pulp/inspect/inspector_server.hpp>

#include <filesystem>
#include <fstream>
#include <condition_variable>
#include <mutex>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using pulp::inspect::InspectorCapability;
using pulp::inspect::InspectorClient;
using pulp::inspect::InspectorDiscoveryPublisher;
using pulp::inspect::InspectorDiscoveryReader;
using pulp::inspect::InspectorDiscoveryRecord;
using pulp::inspect::InspectorPolicyConfig;
using pulp::inspect::InspectorProfile;
using pulp::inspect::InspectorServer;
using pulp::inspect::InspectorServerConfig;
using pulp::inspect::InspectorSession;
using pulp::inspect::InspectorSessionInfo;
using pulp::inspect::generate_inspector_secret;
using pulp::inspect::make_response;

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto token = generate_inspector_secret();
        REQUIRE(token.has_value());
        std::string suffix;
        for (std::size_t index = 0; index < 8; ++index)
            suffix += "0123456789abcdef"[(*token)[index] & 0xf];
        path = std::filesystem::temp_directory_path() /
               ("pulp-inspector-client-test-" + suffix);
        std::filesystem::create_directories(path);
#ifndef _WIN32
        REQUIRE(::chmod(path.c_str(), 0700) == 0);
#endif
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

struct AuthenticatedFixture {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher{temporary.path};
    InspectorDiscoveryReader reader{temporary.path};
    InspectorPolicyConfig policy;
    InspectorSession session;
    InspectorServer server;

    AuthenticatedFixture()
        : policy([] {
              InspectorPolicyConfig result;
              result.profile = InspectorProfile::Develop;
              result.available_capabilities = {
                  InspectorCapability::SessionDescribe,
                  InspectorCapability::SessionControl,
                  InspectorCapability::StateRead,
                  InspectorCapability::StateWrite,
              };
              return result;
          }()),
          session(
              InspectorSessionInfo{
                  "session-client-test",
                  "instance-client-test",
                  "com.pulp.client-test",
                  "1"},
              policy,
              [](const auto& request) {
                  return make_response(
                      request.id,
                      request.method == "State.getParameters"
                          ? R"({"parameters":[{"id":"gain","value":0.5}]})"
                          : R"({"applied":true})");
              }) {
        const auto token = generate_inspector_secret();
        REQUIRE(token.has_value());
        InspectorDiscoveryRecord record;
        record.session_id = session.info().session_id;
        record.instance_id = session.info().instance_id;
        record.plugin_id = session.info().plugin_id;
        REQUIRE(server.start_authenticated(InspectorServerConfig{
            &session, &publisher, record, *token}));
    }

    ~AuthenticatedFixture() {
        server.stop();
    }
};

} // namespace

TEST_CASE("authenticated client completes read and controlled mutation",
          "[inspect][client][authentication]") {
    AuthenticatedFixture fixture;
    const auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);

    InspectorClient client;
    REQUIRE(client.connect(records.front(), fixture.reader));
    const auto capabilities =
        client.request("Session.getCapabilities");
    REQUIRE_FALSE(capabilities.is_error);

    const auto read = client.request("State.getParameters");
    REQUIRE_FALSE(read.is_error);
    CHECK(read.params_json.find("\"gain\"") != std::string::npos);

    const auto denied =
        client.request("State.setParameter", R"({"id":"gain","value":0.75})");
    REQUIRE(denied.is_error);
    CHECK(denied.error_code == "controller_lease_required");

    REQUIRE_FALSE(client.request("Session.acquireController").is_error);
    REQUIRE_FALSE(
        client.request("State.setParameter",
                       R"({"id":"gain","value":0.75})")
            .is_error);
}

TEST_CASE("authentication rejects a replaced credential and teardown removes discovery",
          "[inspect][client][authentication][teardown]") {
    TemporaryDirectory temporary;
    {
        InspectorDiscoveryPublisher publisher(temporary.path);
        InspectorDiscoveryReader reader(temporary.path);
        InspectorPolicyConfig policy;
        policy.profile = InspectorProfile::Observe;
        policy.available_capabilities = {
            InspectorCapability::SessionDescribe,
            InspectorCapability::StateRead,
        };
        InspectorSession session(
            {"session-wrong-token", "instance", "plugin", "1"},
            policy,
            [](const auto& request) { return make_response(request.id, "{}"); });
        InspectorServer server;
        const auto token = generate_inspector_secret();
        REQUIRE(token.has_value());
        InspectorDiscoveryRecord record;
        record.session_id = session.info().session_id;
        record.instance_id = session.info().instance_id;
        record.plugin_id = session.info().plugin_id;
        REQUIRE(server.start_authenticated(
            InspectorServerConfig{&session, &publisher, record, *token}));
        auto records = reader.list();
        REQUIRE(records.size() == 1);
        {
            std::ofstream replaced(records.front().credential_path,
                                   std::ios::trunc);
            replaced << std::string(64, '0');
        }
#ifndef _WIN32
        REQUIRE(::chmod(records.front().credential_path.c_str(), 0600) == 0);
#endif
        InspectorClient client;
        CHECK_FALSE(client.connect(records.front(), reader));
        server.stop();
        CHECK(reader.list().empty());
    }
    CHECK(std::filesystem::is_empty(temporary.path));
}

TEST_CASE("unauthenticated connections are closed at the authentication deadline",
          "[inspect][authentication][timeout]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
    };
    InspectorSession session(
        {"session-auth-timeout", "instance", "plugin", "1"},
        policy,
        [](const auto& request) { return make_response(request.id, "{}"); });
    InspectorServer server;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    InspectorServerConfig config{&session, &publisher, record, *token};
    config.authentication_timeout = std::chrono::milliseconds(20);
    REQUIRE(server.start_authenticated(std::move(config)));

    std::mutex mutex;
    std::condition_variable cv;
    bool disconnected = false;
    pulp::events::InterprocessConnection connection;
    connection.set_on_text_message([](std::string_view) {});
    connection.set_on_disconnected([&] {
        std::lock_guard lock(mutex);
        disconnected = true;
        cv.notify_all();
    });
    REQUIRE(connection.connect(
        "127.0.0.1:" + std::to_string(server.port()),
        pulp::events::IpcTransport::Socket));
    std::unique_lock lock(mutex);
    CHECK(cv.wait_for(lock, std::chrono::seconds(1),
                      [&] { return disconnected; }));
}
