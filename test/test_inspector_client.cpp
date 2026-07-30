#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/authentication.hpp>
#include <pulp/inspect/client.hpp>
#include <pulp/inspect/inspector_server.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/runtime/socket.hpp>

#include <choc/text/choc_JSON.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>

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
using pulp::runtime::Socket;
using pulp::runtime::SocketType;

static_assert(!std::is_copy_constructible_v<InspectorServer>);
static_assert(!std::is_copy_assignable_v<InspectorServer>);
static_assert(!std::is_move_constructible_v<InspectorServer>);
static_assert(!std::is_move_assignable_v<InspectorServer>);

namespace {

bool send_all(Socket& socket, std::span<const std::uint8_t> bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto count = socket.send(bytes.data() + sent, bytes.size() - sent);
        if (count <= 0)
            return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

bool send_frame(Socket& socket, std::string_view payload) {
    if (payload.size() > std::numeric_limits<std::uint32_t>::max())
        return false;
    const auto size = static_cast<std::uint32_t>(payload.size());
    const std::array<std::uint8_t, 4> header{
        static_cast<std::uint8_t>(size),
        static_cast<std::uint8_t>(size >> 8),
        static_cast<std::uint8_t>(size >> 16),
        static_cast<std::uint8_t>(size >> 24),
    };
    return send_all(socket, header) &&
           send_all(socket, std::span(
               reinterpret_cast<const std::uint8_t*>(payload.data()),
               payload.size()));
}

std::optional<std::string> receive_frame(Socket& socket) {
    auto read_exact = [&socket](std::span<std::uint8_t> bytes) {
        std::size_t received = 0;
        while (received < bytes.size()) {
            const auto count =
                socket.receive(bytes.data() + received, bytes.size() - received);
            if (count <= 0)
                return false;
            received += static_cast<std::size_t>(count);
        }
        return true;
    };
    std::array<std::uint8_t, 4> header{};
    if (!read_exact(header))
        return std::nullopt;
    const auto size = static_cast<std::uint32_t>(header[0]) |
                      (static_cast<std::uint32_t>(header[1]) << 8) |
                      (static_cast<std::uint32_t>(header[2]) << 16) |
                      (static_cast<std::uint32_t>(header[3]) << 24);
    if (size > 1024u * 1024u)
        return std::nullopt;
    std::string payload(size, '\0');
    if (!read_exact(std::span(
            reinterpret_cast<std::uint8_t*>(payload.data()), payload.size())))
        return std::nullopt;
    return payload;
}

bool authenticate_raw(Socket& socket,
                      std::span<const std::uint8_t> token) {
    const auto challenge_frame = receive_frame(socket);
    if (!challenge_frame)
        return false;
    pulp::inspect::InspectorMessage challenge_message;
    if (!pulp::inspect::decode_message(*challenge_frame, challenge_message) ||
        challenge_message.method != "Session.authChallenge")
        return false;
    pulp::inspect::InspectorAuthChallenge challenge;
    try {
        const auto params = choc::json::parse(challenge_message.params_json);
        challenge.scheme = std::string(params["scheme"].getString());
        challenge.nonce_hex = std::string(params["nonce"].getString());
        challenge.session_id = std::string(params["sessionId"].getString());
        challenge.instance_id = std::string(params["instanceId"].getString());
        challenge.protocol_version =
            std::string(params["protocolVersion"].getString());
    } catch (...) {
        return false;
    }
    const auto proof =
        pulp::inspect::make_inspector_auth_proof(token, challenge);
    if (!proof)
        return false;
    const auto request = pulp::inspect::make_request(
        1, "Session.authenticate",
        std::string("{\"proof\":\"") + *proof + "\"}");
    if (!send_frame(socket, pulp::inspect::encode_message(request)))
        return false;
    const auto response_frame = receive_frame(socket);
    if (!response_frame)
        return false;
    pulp::inspect::InspectorMessage response;
    return pulp::inspect::decode_message(*response_frame, response) &&
           response.id == 1 && !response.is_error;
}

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

TEST_CASE("server rejects unsupported protocol versions before publishing",
          "[inspect][client][protocol-version]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
    };
    InspectorSession session(
        {"session-future-version", "instance", "plugin", "2"},
        policy,
        [](const auto& request) { return make_response(request.id, "{}"); });
    InspectorServer server;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    CHECK_FALSE(server.start_authenticated(
        InspectorServerConfig{&session, &publisher, record, *token}));
    CHECK(server.port() == 0);
    CHECK(reader.list().empty());
}

TEST_CASE("server rejects deeply nested JSON before authentication",
          "[inspect][client][authentication][resource-limit]") {
    AuthenticatedFixture fixture;

    Socket socket;
    REQUIRE(socket.create(SocketType::TCP));
    REQUIRE(socket.set_read_timeout(std::chrono::seconds(1)));
    REQUIRE(socket.connect("127.0.0.1",
                           static_cast<std::uint16_t>(
                               fixture.server.port())));
    REQUIRE(receive_frame(socket).has_value());

    constexpr std::size_t depth = 65;
    std::string params(depth, '[');
    params += '0';
    params.append(depth, ']');
    const auto request =
        std::string(R"({"id":1,"method":"Session.authenticate","params":)") +
        params + '}';
    REQUIRE(send_frame(socket, request));

    const auto response_frame = receive_frame(socket);
    REQUIRE(response_frame.has_value());
    pulp::inspect::InspectorMessage response;
    REQUIRE(pulp::inspect::decode_message(*response_frame, response));
    CHECK(response.is_error);
    CHECK(response.error_code == "message_too_deep");

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (fixture.server.client_count() != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    CHECK(fixture.server.client_count() == 0);
}

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

TEST_CASE("oversized inspector responses return a bounded protocol error",
          "[inspect][client][resource-limit]") {
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
        {"session-large-response", "instance", "plugin", "1"},
        policy,
        [](const auto& request) {
            return make_response(
                request.id,
                std::string(R"({"padding":")") +
                    std::string(4096, 'x') + R"("})");
        });
    InspectorServer server;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    InspectorServerConfig config{
        &session, &publisher, record, *token};
    config.max_message_bytes = 1024;
    REQUIRE(server.start_authenticated(std::move(config)));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);

    InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));
    const auto response = client.request(
        "State.getParameters", "{}", std::chrono::milliseconds(100));
    REQUIRE(response.is_error);
    CHECK(response.error_code == "response_too_large");
    CHECK(client.is_connected());
    CHECK_FALSE(
        client.request("Session.getCapabilities").is_error);
}

TEST_CASE("authenticated client rejects a challenge for another instance",
          "[inspect][client][authentication][instance]") {
    AuthenticatedFixture fixture;
    const auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);
    auto mismatched = records.front();
    mismatched.instance_id = "another-instance";

    InspectorClient client;
    CHECK_FALSE(client.connect(mismatched, fixture.reader));
    CHECK_FALSE(client.is_connected());
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

TEST_CASE("client reconnect discards queued events from the prior session",
          "[inspect][client][events][generation]") {
    AuthenticatedFixture fixture;
    const auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);

    std::mutex mutex;
    std::condition_variable cv;
    bool first_entered = false;
    bool release_first = false;
    std::vector<int> delivered;
    InspectorClient client;
    client.set_event_handler([&](const auto& event) {
        const auto params = choc::json::parse(event.params_json);
        const auto sequence =
            static_cast<int>(params["sequence"].getInt64());
        std::unique_lock lock(mutex);
        delivered.push_back(sequence);
        if (sequence == 1) {
            first_entered = true;
            cv.notify_all();
            cv.wait_for(lock, std::chrono::seconds(2),
                        [&] { return release_first; });
        }
        cv.notify_all();
    });
    REQUIRE(client.connect(records.front(), fixture.reader));
    fixture.server.broadcast(pulp::inspect::make_event(
        "State.parameterChanged", R"({"sequence":1})"));
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(1),
                            [&] { return first_entered; }));
    }
    fixture.server.broadcast(pulp::inspect::make_event(
        "State.parameterChanged", R"({"sequence":2})"));
    REQUIRE_FALSE(
        client.request("Session.getCapabilities").is_error);
    client.disconnect();
    REQUIRE(client.connect(records.front(), fixture.reader));
    {
        std::lock_guard lock(mutex);
        release_first = true;
    }
    cv.notify_all();
    fixture.server.broadcast(pulp::inspect::make_event(
        "State.parameterChanged", R"({"sequence":3})"));

    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&] {
            return delivered.size() >= 2;
        }));
        CHECK(delivered == std::vector<int>{1, 3});
    }
}

TEST_CASE("response timeout fences may-have-applied requests",
          "[inspect][client][timeout][mutation]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Develop;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::SessionControl,
        InspectorCapability::StateWrite,
    };
    std::atomic<bool> applied{false};
    InspectorSession session(
        {"session-timeout", "instance", "plugin", "1"},
        policy,
        [&](const auto& request) {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            applied.store(true, std::memory_order_release);
            return make_response(request.id, R"({"applied":true})");
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
    REQUIRE_FALSE(client.request("Session.acquireController").is_error);
    const auto response = client.request(
        "State.setParameter",
        R"({"id":"gain","value":0.75})",
        std::chrono::milliseconds(10));
    REQUIRE(response.is_error);
    CHECK(response.error_code == "request_timeout");
    CHECK(response.error_data_json.find("\"mayHaveApplied\":true") !=
          std::string::npos);
    CHECK_FALSE(client.is_connected());

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!applied.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    CHECK(applied.load(std::memory_order_acquire));
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

TEST_CASE("client disconnect during event writes does not deadlock",
          "[inspect][client][events][teardown][concurrency]") {
    AuthenticatedFixture fixture;
    const auto records = fixture.reader.list();
    REQUIRE(records.size() == 1);
    const std::string payload =
        R"({"id":"gain","padding":")" + std::string(256 * 1024, 'x') +
        R"("})";

    for (int iteration = 0; iteration < 8; ++iteration) {
        InspectorClient client;
        REQUIRE(client.connect(records.front(), fixture.reader));
        std::atomic<bool> started{false};
        std::thread broadcaster([&] {
            started.store(true, std::memory_order_release);
            for (int event = 0; event < 16; ++event) {
                fixture.server.broadcast(pulp::inspect::make_event(
                    "State.parameterChanged", payload));
            }
        });
        while (!started.load(std::memory_order_acquire))
            std::this_thread::yield();
        client.disconnect();
        broadcaster.join();
    }

    InspectorClient replacement;
    REQUIRE(replacement.connect(records.front(), fixture.reader));
    CHECK_FALSE(
        replacement.request("State.getParameters").is_error);
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
    REQUIRE(response.is_error);
    CHECK(response.error_code == "connection_closed");
    CHECK(response.error_data_json.find("\"mayHaveApplied\":true") !=
          std::string::npos);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!reader.list().empty() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(reader.list().empty());
}

TEST_CASE("server can be released from a request callback",
          "[inspect][client][teardown][owner-lifetime]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig config;
    config.profile = InspectorProfile::Observe;
    config.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::StateRead,
    };
    std::unique_ptr<InspectorServer> server =
        std::make_unique<InspectorServer>();
    InspectorSession session(
        {"session-destroy-server", "instance", "plugin", "1"},
        config,
        [&](const auto& request) {
            server.reset();
            return make_response(request.id, "{}");
        });
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    REQUIRE(server->start_authenticated(
        InspectorServerConfig{&session, &publisher, record, *token}));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));
    CHECK(client.request("State.getParameters").is_error);
    CHECK_FALSE(server);
}

TEST_CASE("server can be destroyed by a callback while another thread stops it",
          "[inspect][client][teardown][owner-lifetime][concurrency]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig config;
    config.profile = InspectorProfile::Observe;
    config.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::StateRead,
    };
    std::unique_ptr<InspectorServer> server =
        std::make_unique<InspectorServer>();
    auto* server_raw = server.get();
    std::mutex mutex;
    std::condition_variable cv;
    bool callback_entered = false;
    bool destroy_server = false;
    InspectorSession session(
        {"session-concurrent-destroy", "instance", "plugin", "1"},
        config,
        [&](const auto& request) {
            {
                std::unique_lock lock(mutex);
                callback_entered = true;
                cv.notify_all();
                cv.wait(lock, [&] { return destroy_server; });
            }
            server.reset();
            return make_response(request.id, "{}");
        });
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    REQUIRE(server->start_authenticated(
        InspectorServerConfig{&session, &publisher, record, *token}));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));

    std::thread requester([&] {
        (void)client.request("State.getParameters");
    });
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(1),
                            [&] { return callback_entered; }));
    }
    std::atomic<bool> stop_returned{false};
    std::thread stopper([&] {
        server_raw->stop();
        stop_returned.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(stop_returned.load(std::memory_order_acquire));
    {
        std::lock_guard lock(mutex);
        destroy_server = true;
    }
    cv.notify_all();
    requester.join();
    stopper.join();
    CHECK_FALSE(server);
    CHECK(stop_returned.load(std::memory_order_acquire));
    CHECK(reader.list().empty());
}

TEST_CASE("concurrent callback stop requests do not deadlock",
          "[inspect][client][teardown][concurrency][reentrant]") {
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
    std::mutex barrier_mutex;
    std::condition_variable barrier_cv;
    int entered = 0;
    InspectorSession session(
        {"session-concurrent-stop", "instance", "plugin", "1"},
        config,
        [&](const auto& request) {
            {
                std::unique_lock lock(barrier_mutex);
                ++entered;
                barrier_cv.notify_all();
                barrier_cv.wait(lock, [&] { return entered == 2; });
            }
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
    InspectorClient first;
    InspectorClient second;
    REQUIRE(first.connect(records.front(), reader));
    REQUIRE(second.connect(records.front(), reader));
    std::thread first_request([&] {
        (void)first.request("State.getParameters");
    });
    std::thread second_request([&] {
        (void)second.request("State.getParameters");
    });
    first_request.join();
    second_request.join();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!reader.list().empty() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(reader.list().empty());
}

TEST_CASE("callback stop cancels a concurrent authenticated restart",
          "[inspect][client][teardown][restart][concurrency]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::StateRead,
    };
    InspectorServer server;
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool allow_callback_stop = false;
    InspectorSession session(
        {"session-restart-fence", "instance", "plugin", "1"},
        policy,
        [&](const auto& request) {
            {
                std::unique_lock lock(mutex);
                entered = true;
                cv.notify_all();
                cv.wait(lock, [&] { return allow_callback_stop; });
            }
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

    std::atomic<bool> request_returned{false};
    std::thread requester([&] {
        (void)client.request("State.getParameters");
        request_returned.store(true, std::memory_order_release);
    });
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(1),
                            [&] { return entered; }));
    }

    const auto replacement_token = generate_inspector_secret();
    REQUIRE(replacement_token.has_value());
    std::atomic<bool> restart_result{true};
    std::thread restarter([&] {
        restart_result.store(
            server.start_authenticated(InspectorServerConfig{
                &session, &publisher, record, *replacement_token}),
            std::memory_order_release);
    });

    const auto disconnect_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!request_returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < disconnect_deadline) {
        std::this_thread::yield();
    }
    REQUIRE(request_returned.load(std::memory_order_acquire));
    {
        std::lock_guard lock(mutex);
        allow_callback_stop = true;
    }
    cv.notify_all();

    requester.join();
    restarter.join();
    CHECK_FALSE(restart_result.load(std::memory_order_acquire));
    CHECK(server.port() == 0);
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

TEST_CASE("session restart serializes publication with heartbeat refresh",
          "[inspect][client][restart][heartbeat][concurrency]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
    };
    InspectorSession first(
        {"session-heartbeat-a", "instance-a", "plugin", "1"},
        policy,
        [](const auto& request) { return make_response(request.id, "{}"); });
    InspectorSession second(
        {"session-heartbeat-b", "instance-b", "plugin", "1"},
        policy,
        [](const auto& request) { return make_response(request.id, "{}"); });
    InspectorServer server;

    auto start = [&](InspectorSession& session) {
        const auto token = generate_inspector_secret();
        REQUIRE(token.has_value());
        InspectorDiscoveryRecord record;
        record.session_id = session.info().session_id;
        record.instance_id = session.info().instance_id;
        record.plugin_id = session.info().plugin_id;
        InspectorServerConfig config{
            &session, &publisher, record, *token};
        config.heartbeat_interval = std::chrono::milliseconds(1);
        REQUIRE(server.start_authenticated(std::move(config)));
    };

    for (int iteration = 0; iteration < 8; ++iteration) {
        auto& expected = iteration % 2 == 0 ? first : second;
        start(expected);
        // The cleanup worker polls at 50 ms, so this crosses a refresh
        // boundary before the next generation replaces the publication.
        std::this_thread::sleep_for(std::chrono::milliseconds(55));
        const auto records = reader.list();
        REQUIRE(records.size() == 1);
        CHECK(records.front().session_id == expected.info().session_id);
        CHECK(records.front().instance_id == expected.info().instance_id);
    }
}

TEST_CASE("discovery lifetime remains longer than a configured heartbeat",
          "[inspect][client][heartbeat][discovery]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
    };
    InspectorSession session(
        {"session-long-heartbeat", "instance", "plugin", "1"},
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
    config.heartbeat_interval = std::chrono::seconds(40);
    const auto started_at = std::chrono::duration_cast<
        std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    REQUIRE(server.start_authenticated(std::move(config)));

    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    // A 40-second heartbeat would outlive the publisher's historical
    // 30-second default. The server derives a three-interval TTL instead.
    CHECK(records.front().expires_at_unix_ms - started_at >= 119'000);
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
    for (const auto& entry :
         std::filesystem::directory_iterator(temporary.path)) {
        CHECK(entry.path().extension() == ".lock");
    }
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

TEST_CASE("authenticated partial frames expire without consuming the client pool",
          "[inspect][authentication][resource-limit][timeout]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
    };
    InspectorSession session(
        {"session-frame-timeout", "instance", "plugin", "1"},
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
    config.max_clients = 1;
    config.frame_read_timeout = std::chrono::milliseconds(40);
    REQUIRE(server.start_authenticated(std::move(config)));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);

    Socket partial;
    REQUIRE(partial.create(SocketType::TCP));
    REQUIRE(partial.set_read_timeout(std::chrono::seconds(1)));
    REQUIRE(partial.connect("127.0.0.1",
                            static_cast<std::uint16_t>(server.port())));
    REQUIRE(authenticate_raw(partial, *token));
    REQUIRE(server.client_count() == 1);

    // Declare a 64-byte frame but send only one payload byte. The timeout is
    // cumulative from the first header byte, so drip-feeding cannot retain
    // this sole slot indefinitely.
    const std::array<std::uint8_t, 5> partial_frame{64, 0, 0, 0, '{'};
    REQUIRE(send_all(partial, partial_frame));
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (server.client_count() != 0 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    REQUIRE(server.client_count() == 0);

    InspectorClient admitted;
    CHECK(admitted.connect(records.front(), reader));
}
