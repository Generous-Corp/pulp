#include <catch2/catch_test_macros.hpp>

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/control_client_connection.hpp>
#include <pulp/runtime/crypto.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

using namespace std::chrono_literals;
using namespace pulp::events;
using namespace pulp::inspect;

namespace {

struct HealthSocketDirectory {
    std::filesystem::path path;

    HealthSocketDirectory() {
        const auto random = pulp::runtime::secure_random_bytes(8);
        REQUIRE(random.has_value());
        path = std::filesystem::temp_directory_path() /
               ("pulp-control-health-" + pulp::runtime::hex_encode(*random));
        REQUIRE(std::filesystem::create_directory(path));
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
    }

    ~HealthSocketDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

#ifdef __APPLE__
ControlPeerExpectation health_process_expectation(const std::filesystem::path& endpoint) {
    InterprocessConnectionServer server;
    std::mutex mutex;
    std::condition_variable ready;
    std::unique_ptr<InterprocessConnection> accepted;
    server.on_client_connected = [&](std::unique_ptr<InterprocessConnection> connection) {
        {
            std::lock_guard lock(mutex);
            accepted = std::move(connection);
        }
        ready.notify_all();
    };
    REQUIRE(server.start(endpoint.string(), IpcTransport::LocalSocket));
    InterprocessConnection client;
    REQUIRE(client.connect(endpoint.string(), IpcTransport::LocalSocket, 2s));
    {
        std::unique_lock lock(mutex);
        REQUIRE(ready.wait_for(lock, 2s, [&] { return accepted != nullptr; }));
    }
    const auto evidence = observe_control_peer(client, ControlPeerRole::TrustedHostBridge);
    REQUIRE(evidence.has_value());
    client.disconnect();
    accepted.reset();
    server.stop();
    return {.evidence = *evidence};
}

class HealthServer {
  public:
    using Handler = std::function<void(const ControlHealthEnvelope&)>;

    HealthServer(std::filesystem::path endpoint, Handler handler) : handler_(std::move(handler)) {
        server_.on_client_connected = [this](std::unique_ptr<InterprocessConnection> connection) {
            connection->set_on_message([this](const void* data, std::size_t size) {
                ++messages;
                const auto envelope =
                    decode_control_envelope(std::string_view(static_cast<const char*>(data), size));
                REQUIRE(envelope.has_value());
                const auto* health = std::get_if<ControlHealthEnvelope>(&envelope->payload);
                REQUIRE(health != nullptr);
                handler_(*health);
            });
            std::lock_guard lock(mutex_);
            connection_ = std::move(connection);
        };
        REQUIRE(server_.start(endpoint.string(), IpcTransport::LocalSocket));
    }

    ~HealthServer() {
        std::unique_ptr<InterprocessConnection> connection;
        {
            std::lock_guard lock(mutex_);
            connection = std::move(connection_);
        }
        if (connection)
            connection->disconnect();
        server_.stop();
    }

    bool send(const ControlEnvelope& envelope) {
        const auto encoded = encode_control_envelope(envelope);
        std::lock_guard lock(mutex_);
        return connection_ && !encoded.empty() && connection_->send_message(encoded);
    }

    bool send_raw(std::string_view frame) {
        std::lock_guard lock(mutex_);
        return connection_ && connection_->send_message(frame);
    }

    std::atomic<unsigned> messages{0};

  private:
    Handler handler_;
    InterprocessConnectionServer server_;
    std::mutex mutex_;
    std::unique_ptr<InterprocessConnection> connection_;
};

ControlHealthProbeConfig probe_config(const std::filesystem::path& endpoint) {
    return {
        .endpoint_path = endpoint,
        .connect_timeout = 2s,
        .write_timeout = 2s,
        .frame_read_timeout = 2s,
    };
}
#endif

} // namespace

TEST_CASE("control health never authenticates a broker without an expectation",
          "[inspect][control][health][identity]") {
#ifdef __APPLE__
    HealthSocketDirectory directory;
    const auto expectation = health_process_expectation(directory.path / "observer.sock");
    HealthServer server{directory.path / "broker.sock", [](const ControlHealthEnvelope&) {
                            FAIL("an unverified health probe must not send a request");
                        }};

    const auto unverified = probe_control_broker(probe_config(directory.path / "broker.sock"), 2s);
    CHECK(unverified.status == ControlBrokerHealthProbeStatus::ReachableUnverified);
    CHECK_FALSE(unverified.health.has_value());
    CHECK(server.messages.load() == 0);

    auto wrong = expectation;
    wrong.evidence.process_start_id += "-wrong";
    auto config = probe_config(directory.path / "broker.sock");
    config.expected_broker = std::move(wrong);
    const auto mismatch = probe_control_broker(config, 2s);
    CHECK(mismatch.status == ControlBrokerHealthProbeStatus::ReachableUnverified);
    CHECK_FALSE(mismatch.health.has_value());
    CHECK(server.messages.load() == 0);
#else
    SUCCEED("authenticated local peer verification is currently macOS-only");
#endif
}

TEST_CASE("control health reports verified compatible and incompatible brokers",
          "[inspect][control][health][compatibility]") {
#ifdef __APPLE__
    HealthSocketDirectory directory;
    const auto expectation = health_process_expectation(directory.path / "observer.sock");

    SECTION("compatible") {
        HealthServer* server_ptr = nullptr;
        HealthServer server{
            directory.path / "compatible.sock",
            [&](const ControlHealthEnvelope& request) {
                REQUIRE(server_ptr->send(ControlEnvelope{.payload = ControlHealthResult{
                                                             .request_id = request.request_id,
                                                             .sdk_version = "0.791.0-test",
                                                             .protocol_versions = {1, 1},
                                                             .broker_id = "broker-a",
                                                             .process_generation = 42,
                                                         }}));
            },
        };
        server_ptr = &server;
        auto config = probe_config(directory.path / "compatible.sock");
        config.expected_broker = expectation;
        const auto result = probe_control_broker(config, 2s);
        REQUIRE(result.status == ControlBrokerHealthProbeStatus::HealthyVerified);
        REQUIRE(result.healthy());
        REQUIRE(result.health.has_value());
        CHECK(result.health->sdk_version == "0.791.0-test");
        CHECK(result.health->protocol_versions == ControlProtocolRange{1, 1});
        CHECK(result.health->broker_id == "broker-a");
        CHECK(result.health->process_generation == 42);
    }

    SECTION("incompatible") {
        HealthServer* server_ptr = nullptr;
        HealthServer server{
            directory.path / "incompatible.sock",
            [&](const ControlHealthEnvelope& request) {
                REQUIRE(server_ptr->send(ControlEnvelope{.payload = ControlHealthResult{
                                                             .request_id = request.request_id,
                                                             .sdk_version = "0.900.0-test",
                                                             .protocol_versions = {2, 3},
                                                             .broker_id = "broker-b",
                                                             .process_generation = 43,
                                                         }}));
            },
        };
        server_ptr = &server;
        auto config = probe_config(directory.path / "incompatible.sock");
        config.expected_broker = expectation;
        const auto result = probe_control_broker(config, 2s);
        CHECK(result.status == ControlBrokerHealthProbeStatus::Incompatible);
        CHECK_FALSE(result.healthy());
        REQUIRE(result.health.has_value());
        CHECK(result.health->protocol_versions == ControlProtocolRange{2, 3});
        CHECK(result.error_code == "incompatible-protocol");
    }
#else
    SUCCEED("authenticated local peer verification is currently macOS-only");
#endif
}

TEST_CASE("control health distinguishes unavailable and malformed carriers",
          "[inspect][control][health][failure]") {
#ifdef __APPLE__
    HealthSocketDirectory directory;
    const auto expectation = health_process_expectation(directory.path / "observer.sock");

    auto unavailable_config = probe_config(directory.path / "missing.sock");
    unavailable_config.expected_broker = expectation;
    const auto unavailable = probe_control_broker(unavailable_config, 100ms);
    CHECK(unavailable.status == ControlBrokerHealthProbeStatus::Unavailable);
    CHECK_FALSE(unavailable.health.has_value());

    HealthServer* server_ptr = nullptr;
    HealthServer malformed_server{
        directory.path / "malformed.sock",
        [&](const ControlHealthEnvelope&) { REQUIRE(server_ptr->send_raw("not-json")); },
    };
    server_ptr = &malformed_server;
    auto malformed_config = probe_config(directory.path / "malformed.sock");
    malformed_config.expected_broker = expectation;
    const auto malformed = probe_control_broker(malformed_config, 2s);
    CHECK(malformed.status == ControlBrokerHealthProbeStatus::Malformed);
    CHECK_FALSE(malformed.health.has_value());
    CHECK(malformed.error_code == "malformed-response");
#else
    SUCCEED("authenticated local peer verification is currently macOS-only");
#endif
}
