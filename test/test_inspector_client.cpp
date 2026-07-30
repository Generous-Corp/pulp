#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/authentication.hpp>
#include <pulp/inspect/client.hpp>
#include <pulp/inspect/inspector_server.hpp>

#include <filesystem>
#include <fstream>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

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

TEST_CASE("server broadcasts only registered events granted by policy",
          "[inspect][client][events][policy]") {
    AuthenticatedFixture fixture;
    const auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> events;
    InspectorClient client;
    client.set_event_handler([&](const auto& event) {
        std::lock_guard lock(mutex);
        events.push_back(event.method);
        cv.notify_all();
    });
    REQUIRE(client.connect(records.front(), fixture.reader));

    fixture.server.broadcast(
        pulp::inspect::make_event("Audio.levels", R"({"peak":0.9})"));
    fixture.server.broadcast(
        pulp::inspect::make_event("Unknown.event", "{}"));
    fixture.server.broadcast(
        pulp::inspect::make_event("State.parameterChanged",
                                  R"({"id":"gain","value":0.75})"));

    std::unique_lock lock(mutex);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&] {
        return !events.empty();
    }));
    REQUIRE(events.size() == 1);
    CHECK(events.front() == "State.parameterChanged");
}

TEST_CASE("client event handlers can issue follow-up requests",
          "[inspect][client][events][reentrant]") {
    AuthenticatedFixture fixture;
    const auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);

    std::mutex mutex;
    std::condition_variable cv;
    std::optional<pulp::inspect::InspectorMessage> follow_up;
    InspectorClient client;
    client.set_event_handler([&](const auto&) {
        auto response = client.request("State.getParameters");
        {
            std::lock_guard lock(mutex);
            follow_up = std::move(response);
        }
        cv.notify_all();
    });
    REQUIRE(client.connect(records.front(), fixture.reader));
    fixture.server.broadcast(
        pulp::inspect::make_event("State.parameterChanged", "{}"));

    std::unique_lock lock(mutex);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&] {
        return follow_up.has_value();
    }));
    CHECK_FALSE(follow_up->is_error);
}

TEST_CASE("client can be released from its event handler",
          "[inspect][client][events][teardown]") {
    AuthenticatedFixture fixture;
    const auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);

    std::mutex mutex;
    std::condition_variable cv;
    bool released = false;
    auto client = std::make_unique<InspectorClient>();
    client->set_event_handler([&](const auto&) {
        client.reset();
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        cv.notify_all();
    });
    REQUIRE(client->connect(records.front(), fixture.reader));
    fixture.server.broadcast(
        pulp::inspect::make_event("State.parameterChanged", "{}"));

    std::unique_lock lock(mutex);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&] {
        return released;
    }));
    CHECK_FALSE(client);
}

TEST_CASE("server stop is reentrant from a request callback",
          "[inspect][client][teardown][reentrant]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig config;
    config.profile = InspectorProfile::Observe;
    config.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::StateRead,
    };
    InspectorServer server;
    InspectorSession session(
        {"session-reentrant-stop", "instance", "plugin", "1"},
        config,
        [&](const auto& request) {
            server.stop();
            return make_response(request.id, "{}");
        });
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    REQUIRE(server.start_authenticated(
        InspectorServerConfig{&session, &publisher, record, *token}));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));
    const auto response = client.request("State.getParameters");
    CHECK(response.is_error);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!reader.list().empty() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(reader.list().empty());
}

TEST_CASE("server stop releases leases before a session restart",
          "[inspect][client][teardown][lease]") {
    AuthenticatedFixture fixture;
    auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);

    InspectorClient first;
    REQUIRE(first.connect(records.front(), fixture.reader));
    REQUIRE_FALSE(first.request("Session.acquireController").is_error);

    fixture.server.stop();

    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = fixture.session.info().session_id;
    record.instance_id = fixture.session.info().instance_id;
    record.plugin_id = fixture.session.info().plugin_id;
    REQUIRE(fixture.server.start_authenticated(InspectorServerConfig{
        &fixture.session, &fixture.publisher, record, *token}));

    records = fixture.reader.list();
    REQUIRE(records.size() == 1);
    InspectorClient replacement;
    REQUIRE(replacement.connect(records.front(), fixture.reader));
    const auto acquired =
        replacement.request("Session.acquireController");
    CHECK_FALSE(acquired.is_error);
}

TEST_CASE("server stop waits for an active request callback",
          "[inspect][client][teardown][concurrency]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Develop;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::StateRead,
    };

    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    InspectorSession session(
        {"session-stop-waits", "instance", "plugin", "1"},
        policy,
        [&](const auto& request) {
            std::unique_lock lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release; });
            return make_response(request.id, "{}");
        });
    InspectorServer server;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    REQUIRE(server.start_authenticated(
        InspectorServerConfig{&session, &publisher, record, *token}));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);

    InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));
    std::thread requester([&] {
        (void)client.request("State.getParameters");
    });
    bool request_entered = false;
    {
        std::unique_lock lock(mutex);
        request_entered = cv.wait_for(
            lock, std::chrono::seconds(1), [&] { return entered; });
        if (!request_entered)
            release = true;
    }
    cv.notify_all();
    if (!request_entered) {
        requester.join();
        FAIL("request handler did not start");
    }

    std::atomic<bool> stop_returned{false};
    std::thread stopper([&] {
        server.stop();
        stop_returned.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(stop_returned.load(std::memory_order_acquire));
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    cv.notify_all();
    stopper.join();
    requester.join();
    CHECK(stop_returned.load(std::memory_order_acquire));
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

TEST_CASE("server bounds pending unauthenticated clients",
          "[inspect][authentication][resource-limit]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::StateRead,
    };
    InspectorSession session(
        {"session-client-cap", "instance", "plugin", "1"},
        policy,
        [](const auto& request) { return make_response(request.id, "{}"); });
    InspectorServer server;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    InspectorServerConfig config{
        &session, &publisher, record, *token};
    config.max_clients = 2;
    REQUIRE(server.start_authenticated(std::move(config)));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);

    std::vector<std::unique_ptr<pulp::events::InterprocessConnection>> pending;
    for (int index = 0; index < 2; ++index) {
        auto connection =
            std::make_unique<pulp::events::InterprocessConnection>();
        connection->set_on_text_message([](std::string_view) {});
        REQUIRE(connection->connect(
            records.front().endpoint,
            pulp::events::IpcTransport::Socket));
        pending.push_back(std::move(connection));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    InspectorClient rejected;
    CHECK_FALSE(rejected.connect(
        records.front(), reader, std::chrono::milliseconds(250)));

    pending.front()->disconnect();
    pending.erase(pending.begin());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    InspectorClient admitted;
    CHECK(admitted.connect(records.front(), reader));
}
