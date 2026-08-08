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
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace pulp::events;
using namespace pulp::inspect;

namespace {

struct SocketDirectory {
    std::filesystem::path path;

    SocketDirectory() {
        const auto random = pulp::runtime::secure_random_bytes(8);
        REQUIRE(random.has_value());
        path = std::filesystem::temp_directory_path() /
               ("pulp-control-client-" + pulp::runtime::hex_encode(*random));
        REQUIRE(std::filesystem::create_directory(path));
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
    }

    ~SocketDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

#ifdef __APPLE__
ControlPeerExpectation current_process_expectation(const std::filesystem::path& endpoint) {
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

class LocalControlServer {
  public:
    using Handler = std::function<bool(const ControlEnvelope&)>;

    LocalControlServer(std::filesystem::path endpoint, Handler handler)
        : endpoint_(std::move(endpoint)), handler_(std::move(handler)) {
        server_.on_client_connected = [this](std::unique_ptr<InterprocessConnection> connection) {
            connection->set_max_message_bytes(kControlMaximumEnvelopeBytes);
            connection->set_on_disconnected([this] {
                {
                    std::lock_guard lock(mutex_);
                    disconnected_ = true;
                }
                ready_.notify_all();
            });
            connection->set_on_message([this](const void* data, std::size_t size) {
                const auto envelope =
                    decode_control_envelope(std::string_view(static_cast<const char*>(data), size));
                if (!envelope || !handler_(*envelope))
                    handler_succeeded_.store(false, std::memory_order_relaxed);
            });
            {
                std::lock_guard lock(mutex_);
                disconnected_ = false;
                connection_ = std::move(connection);
            }
            ready_.notify_all();
        };
        REQUIRE(server_.start(endpoint_.string(), IpcTransport::LocalSocket));
    }

    ~LocalControlServer() {
        stop();
    }

    bool send(const ControlEnvelope& envelope) {
        const auto encoded = encode_control_envelope(envelope);
        std::lock_guard lock(mutex_);
        return connection_ && !encoded.empty() && connection_->send_message(encoded);
    }

    bool send_raw(std::string_view bytes) {
        std::lock_guard lock(mutex_);
        return connection_ && connection_->send_message(bytes);
    }

    bool wait_for_connection() {
        std::unique_lock lock(mutex_);
        return ready_.wait_for(lock, 2s, [&] { return connection_ != nullptr; });
    }

    bool wait_for_disconnect() {
        std::unique_lock lock(mutex_);
        return ready_.wait_for(lock, 2s, [&] { return disconnected_; });
    }

    bool handler_succeeded() const {
        return handler_succeeded_.load(std::memory_order_relaxed);
    }

    void stop() {
        std::unique_ptr<InterprocessConnection> connection;
        {
            std::lock_guard lock(mutex_);
            connection = std::move(connection_);
        }
        if (connection)
            connection->disconnect();
        server_.stop();
    }

  private:
    std::filesystem::path endpoint_;
    Handler handler_;
    InterprocessConnectionServer server_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::unique_ptr<InterprocessConnection> connection_;
    bool disconnected_ = false;
    std::atomic<bool> handler_succeeded_{true};
};

ControlClientConnectionConfig connection_config(const std::filesystem::path& endpoint,
                                                ControlPeerExpectation expectation) {
    return {
        .endpoint_path = endpoint,
        .expected_broker = std::move(expectation),
        .connect_timeout = 2s,
        .write_timeout = 2s,
        .frame_read_timeout = 2s,
    };
}

ControlSessionOpenResult open_session(ControlClientConnection& connection) {
    return connection.open_session("admission-a", 2s);
}
#endif

} // namespace

TEST_CASE("control client verifies the broker before opening a session",
          "[inspect][control][client][identity]") {
#ifdef __APPLE__
    SocketDirectory directory;
    const auto expectation = current_process_expectation(directory.path / "observer.sock");
    std::atomic<unsigned> messages{0};
    LocalControlServer* server_ptr = nullptr;
    LocalControlServer server{
        directory.path / "broker.sock",
        [&](const ControlEnvelope& envelope) {
            ++messages;
            const auto* open = std::get_if<ControlSessionOpenEnvelope>(&envelope.payload);
            if (open) {
                return server_ptr->send(ControlEnvelope{.payload = ControlSessionOpenResult{
                                                            .request_id = open->request_id,
                                                            .accepted = true,
                                                            .client_id = "client-a",
                                                        }});
            }
            return std::holds_alternative<ControlNegotiationOffer>(envelope.payload) &&
                   server_ptr->send(ControlEnvelope{.payload = ControlErrorEnvelope{
                                                        .request_id = "unknown",
                                                        .error_code = "negotiation-denied",
                                                        .explanation = "negotiation denied for test",
                                                    }});
        },
    };
    server_ptr = &server;

    auto wrong_expectation = expectation;
    wrong_expectation.evidence.process_start_id += "-wrong";
    ControlClientConnection rejected{
        connection_config(directory.path / "broker.sock", std::move(wrong_expectation))};
    CHECK_FALSE(rejected.connect());
    CHECK(rejected.last_error_code() == "broker-verification-failed");
    CHECK(messages.load() == 0);

    ControlClientConnection accepted{
        connection_config(directory.path / "broker.sock", expectation)};
    REQUIRE(accepted.connect());
    const auto opened = open_session(accepted);
    REQUIRE(opened.accepted);
    CHECK(opened.client_id == "client-a");
    CHECK(accepted.is_session_open());
    const auto denied = accepted.dispatch(
        encode_control_envelope(ControlEnvelope{.payload =
                                                    ControlNegotiationOffer{
                                                        .versions = {},
                                                        .mandatory_features = {"receipts"},
                                                    }}),
        2s);
    CHECK(denied.error_code == "negotiation-denied");
    CHECK(denied.explanation == "negotiation denied for test");
    CHECK(messages.load() == 2);
    CHECK(server.handler_succeeded());
#else
    SUCCEED("authenticated local peer verification is currently macOS-only");
#endif
}

TEST_CASE("control client serializes requests and routes progress separately",
          "[inspect][control][client][progress][thread]") {
#ifdef __APPLE__
    SocketDirectory directory;
    const auto expectation = current_process_expectation(directory.path / "observer.sock");
    std::mutex mutex;
    std::condition_variable ready;
    unsigned requests = 0;
    bool release_first = false;
    bool first_response_sent = false;
    std::atomic<bool> delayed_progress_sent{false};
    std::atomic<bool> delayed_response_sent{false};
    std::thread delayed_response;
    LocalControlServer* server_ptr = nullptr;
    LocalControlServer server{
        directory.path / "broker.sock",
        [&](const ControlEnvelope& envelope) {
            if (const auto* open = std::get_if<ControlSessionOpenEnvelope>(&envelope.payload)) {
                return server_ptr->send(ControlEnvelope{.payload = ControlSessionOpenResult{
                                                            .request_id = open->request_id,
                                                            .accepted = true,
                                                            .client_id = "client-a",
                                                        }});
            }
            const auto* cancellation = std::get_if<ControlCancelEnvelope>(&envelope.payload);
            if (cancellation == nullptr)
                return false;
            const auto request_id = cancellation->request_id;
            unsigned request_number = 0;
            {
                std::lock_guard lock(mutex);
                request_number = ++requests;
            }
            ready.notify_all();
            if (request_number == 1) {
                delayed_response = std::thread([&, request_id] {
                    {
                        std::unique_lock lock(mutex);
                        ready.wait_for(lock, 2s, [&] { return release_first; });
                    }
                    delayed_progress_sent =
                        server_ptr->send(ControlEnvelope{.payload = ControlProgressEnvelope{
                                                             .request_id = "request-a",
                                                             .receipt_id = "receipt-a",
                                                             .sequence = 1,
                                                             .current = 1,
                                                             .total = 1,
                                                         }});
                    delayed_response_sent =
                        server_ptr->send(ControlEnvelope{.payload = ControlErrorEnvelope{
                                                             .request_id = request_id,
                                                             .error_code = "test-denied",
                                                             .explanation = "test response",
                                                         }});
                    {
                        std::lock_guard lock(mutex);
                        first_response_sent = true;
                    }
                    ready.notify_all();
                });
                return true;
            } else {
                return first_response_sent &&
                       server_ptr->send(ControlEnvelope{.payload = ControlErrorEnvelope{
                                                            .request_id = cancellation->request_id,
                                                            .error_code = "test-denied",
                                                            .explanation = "test response",
                                                        }});
            }
        },
    };
    server_ptr = &server;
    ControlClientConnection connection{
        connection_config(directory.path / "broker.sock", expectation)};
    REQUIRE(connection.connect());
    REQUIRE(open_session(connection).accepted);
    std::vector<std::string> progress_requests;
    std::mutex progress_mutex;
    connection.set_progress_sink([&](const ControlProgressEnvelope& progress) {
        std::lock_guard lock(progress_mutex);
        progress_requests.push_back(progress.request_id);
    });
    const auto cancellation = encode_control_envelope(ControlEnvelope{
        .payload = ControlCancelEnvelope{.request_id = "request-a", .reason = "test"}});
    ControlTransportDispatchResult first;
    ControlTransportDispatchResult second;
    std::thread first_thread([&] { first = connection.dispatch(cancellation, 2s); });
    {
        std::unique_lock lock(mutex);
        REQUIRE(ready.wait_for(lock, 2s, [&] { return requests == 1; }));
    }
    std::thread second_thread([&] { second = connection.dispatch(cancellation, 2s); });
    std::this_thread::sleep_for(50ms);
    {
        std::lock_guard lock(mutex);
        CHECK(requests == 1);
        release_first = true;
    }
    ready.notify_all();
    first_thread.join();
    second_thread.join();
    delayed_response.join();

    CHECK(delayed_progress_sent.load());
    CHECK(delayed_response_sent.load());
    CHECK(first.error_code == "test-denied");
    CHECK(second.error_code == "test-denied");
    CHECK(requests == 2);
    CHECK(server.handler_succeeded());
    std::lock_guard progress_lock(progress_mutex);
    CHECK(progress_requests == std::vector<std::string>{"request-a"});
#else
    SUCCEED("authenticated local peer verification is currently macOS-only");
#endif
}

TEST_CASE("control client timeout disconnects and disconnect wakes a waiter",
          "[inspect][control][client][timeout][thread]") {
#ifdef __APPLE__
    SocketDirectory directory;
    const auto expectation = current_process_expectation(directory.path / "observer.sock");
    std::mutex mutex;
    std::condition_variable received;
    bool request_received = false;
    LocalControlServer* server_ptr = nullptr;
    LocalControlServer server{
        directory.path / "broker.sock",
        [&](const ControlEnvelope& envelope) {
            if (const auto* open = std::get_if<ControlSessionOpenEnvelope>(&envelope.payload)) {
                return server_ptr->send(ControlEnvelope{.payload = ControlSessionOpenResult{
                                                            .request_id = open->request_id,
                                                            .accepted = true,
                                                            .client_id = "client-a",
                                                        }});
            }
            {
                std::lock_guard lock(mutex);
                request_received = true;
            }
            received.notify_all();
            return true;
        },
    };
    server_ptr = &server;
    const auto offer =
        encode_control_envelope(ControlEnvelope{.payload = ControlNegotiationOffer{
                                                    .versions = {},
                                                    .mandatory_features = {"receipts"},
                                                }});

    SECTION("timeout poisons the connection") {
        ControlClientConnection connection{
            connection_config(directory.path / "broker.sock", expectation)};
        REQUIRE(connection.connect());
        REQUIRE(open_session(connection).accepted);
        const auto result = connection.dispatch(offer, 50ms);
        CHECK(result.error_code == "timeout");
        CHECK_FALSE(connection.is_connected());
    }

    SECTION("explicit disconnect wakes the response waiter") {
        ControlClientConnection connection{
            connection_config(directory.path / "broker.sock", expectation)};
        REQUIRE(connection.connect());
        REQUIRE(open_session(connection).accepted);
        ControlTransportDispatchResult result;
        std::thread waiter([&] { result = connection.dispatch(offer, 5s); });
        {
            std::unique_lock lock(mutex);
            REQUIRE(received.wait_for(lock, 2s, [&] { return request_received; }));
        }
        const auto start = std::chrono::steady_clock::now();
        connection.disconnect();
        waiter.join();
        CHECK(std::chrono::steady_clock::now() - start < 1s);
        CHECK(result.error_code == "connection-lost");
    }
    CHECK(server.handler_succeeded());
#else
    SUCCEED("authenticated local peer verification is currently macOS-only");
#endif
}

TEST_CASE("control client protocol violations poison and close the authenticated carrier",
          "[inspect][control][client][protocol][security]") {
#ifdef __APPLE__
    SocketDirectory directory;
    const auto expectation = current_process_expectation(directory.path / "observer.sock");
    LocalControlServer* server_ptr = nullptr;
    LocalControlServer server{
        directory.path / "broker.sock",
        [&](const ControlEnvelope& envelope) {
            const auto* open = std::get_if<ControlSessionOpenEnvelope>(&envelope.payload);
            return open != nullptr &&
                   server_ptr->send(ControlEnvelope{.payload = ControlSessionOpenResult{
                                                        .request_id = open->request_id,
                                                        .accepted = true,
                                                        .client_id = "client-a",
                                                    }});
        },
    };
    server_ptr = &server;
    ControlClientConnection connection{
        connection_config(directory.path / "broker.sock", expectation)};
    REQUIRE(connection.connect());
    REQUIRE(open_session(connection).accepted);
    REQUIRE(server.send_raw("{malformed"));

    REQUIRE(server.wait_for_disconnect());
    CHECK_FALSE(connection.is_connected());
    CHECK_FALSE(connection.is_session_open());
    CHECK(connection.last_error_code() == "malformed-response");
    CHECK(server.handler_succeeded());
#else
    SUCCEED("authenticated local peer verification is currently macOS-only");
#endif
}

TEST_CASE("control client rejects a response correlated to another request",
          "[inspect][control][client][protocol][correlation]") {
#ifdef __APPLE__
    SocketDirectory directory;
    const auto expectation = current_process_expectation(directory.path / "observer.sock");
    LocalControlServer* server_ptr = nullptr;
    LocalControlServer server{
        directory.path / "broker.sock",
        [&](const ControlEnvelope& envelope) {
            if (const auto* open = std::get_if<ControlSessionOpenEnvelope>(&envelope.payload)) {
                return server_ptr->send(ControlEnvelope{.payload = ControlSessionOpenResult{
                                                            .request_id = open->request_id,
                                                            .accepted = true,
                                                            .client_id = "client-a",
                                                        }});
            }
            return std::holds_alternative<ControlCancelEnvelope>(envelope.payload) &&
                   server_ptr->send(ControlEnvelope{.payload = ControlErrorEnvelope{
                                                        .request_id = "unrelated-request",
                                                        .error_code = "test-denied",
                                                        .explanation = "test response",
                                                    }});
        },
    };
    server_ptr = &server;
    ControlClientConnection connection{
        connection_config(directory.path / "broker.sock", expectation)};
    REQUIRE(connection.connect());
    REQUIRE(open_session(connection).accepted);
    const auto cancellation = encode_control_envelope(ControlEnvelope{
        .payload = ControlCancelEnvelope{.request_id = "cancel-1", .reason = "test"}});
    const auto result = connection.dispatch(cancellation, 2s);
    CHECK(result.error_code == "unexpected-response");
    REQUIRE(server.wait_for_disconnect());
    CHECK_FALSE(connection.is_connected());
    CHECK(server.handler_succeeded());
#else
    SUCCEED("authenticated local peer verification is currently macOS-only");
#endif
}

TEST_CASE("control client disconnect from a progress callback closes the carrier",
          "[inspect][control][client][progress][disconnect]") {
#ifdef __APPLE__
    SocketDirectory directory;
    const auto expectation = current_process_expectation(directory.path / "observer.sock");
    LocalControlServer* server_ptr = nullptr;
    LocalControlServer server{
        directory.path / "broker.sock",
        [&](const ControlEnvelope& envelope) {
            if (const auto* open = std::get_if<ControlSessionOpenEnvelope>(&envelope.payload)) {
                return server_ptr->send(ControlEnvelope{.payload = ControlSessionOpenResult{
                                                            .request_id = open->request_id,
                                                            .accepted = true,
                                                            .client_id = "client-a",
                                                        }});
            }
            const auto* cancellation = std::get_if<ControlCancelEnvelope>(&envelope.payload);
            return cancellation != nullptr &&
                   server_ptr->send(ControlEnvelope{.payload = ControlProgressEnvelope{
                                                        .request_id = cancellation->request_id,
                                                        .receipt_id = "receipt-a",
                                                        .sequence = 1,
                                                        .current = 0,
                                                        .total = 1,
                                                    }});
        },
    };
    server_ptr = &server;
    ControlClientConnection connection{
        connection_config(directory.path / "broker.sock", expectation)};
    REQUIRE(connection.connect());
    REQUIRE(open_session(connection).accepted);
    connection.set_progress_sink([&](const ControlProgressEnvelope&) { connection.disconnect(); });
    const auto cancellation = encode_control_envelope(ControlEnvelope{
        .payload = ControlCancelEnvelope{.request_id = "cancel-1", .reason = "test"}});
    const auto result = connection.dispatch(cancellation, 2s);
    CHECK(result.error_code == "connection-lost");
    REQUIRE(server.wait_for_disconnect());
    CHECK_FALSE(connection.is_connected());
    CHECK(server.handler_succeeded());
#else
    SUCCEED("authenticated local peer verification is currently macOS-only");
#endif
}

TEST_CASE("control client maps artifact wire metadata without dropping lineage",
          "[inspect][control][client][artifact]") {
#ifdef __APPLE__
    SocketDirectory directory;
    const auto expectation = current_process_expectation(directory.path / "observer.sock");
    LocalControlServer* server_ptr = nullptr;
    LocalControlServer server{
        directory.path / "broker.sock",
        [&](const ControlEnvelope& envelope) {
            if (const auto* open = std::get_if<ControlSessionOpenEnvelope>(&envelope.payload)) {
                return server_ptr->send(ControlEnvelope{.payload = ControlSessionOpenResult{
                                                            .request_id = open->request_id,
                                                            .accepted = true,
                                                            .client_id = "client-a",
                                                        }});
            }
            const auto* read = std::get_if<ControlArtifactReadEnvelope>(&envelope.payload);
            return read != nullptr && server_ptr->send(
                ControlEnvelope{.payload = ControlArtifactReadResponseEnvelope{
                                    .request_id = read->request_id,
                                    .status_id = "read",
                                    .metadata =
                                        ControlArtifactWireMetadata{
                                            .artifact_id = "artifact-a",
                                            .broker_id = "broker-a",
                                            .receipt_id = "receipt-a",
                                            .producer_client_id = "client-a",
                                            .producer_registration_id = "registration-a",
                                            .session_id = "session-a",
                                            .instance_id = "instance-a",
                                            .publication_id = "publication-a",
                                            .producer_capability_id = "capability-a",
                                            .producer_operation_id = "dev.pulp.state/read@1",
                                            .producer_operation_version = 7,
                                            .original_grant_id = "grant-a",
                                            .consent_decision_id = "decision-a",
                                            .manifest_digest = std::string(64, 'a'),
                                            .producer_artifact_digest = std::string(64, 'b'),
                                            .sha256 = std::string(64, 'c'),
                                            .byte_size = 4,
                                            .content_type = "application/octet-stream",
                                            .created_at_unix_ms = 100,
                                            .expires_at_unix_ms = 200,
                                            .sensitivity_id = "restricted",
                                            .deletion_state_id = "active",
                                            .redaction_state_id = "redacted",
                                        },
                                    .bytes_base64 = "AAEC/w==",
                                    .eof = true,
                                }});
        },
    };
    server_ptr = &server;
    ControlClientConnection connection{
        connection_config(directory.path / "broker.sock", expectation)};
    REQUIRE(connection.connect());
    REQUIRE(open_session(connection).accepted);
    const auto result = connection.read_artifact("artifact-a", 0, 4, 2s);
    REQUIRE(result.status == ControlArtifactStatus::Read);
    REQUIRE(result.metadata.has_value());
    const auto& metadata = *result.metadata;
    CHECK(metadata.artifact_id == "artifact-a");
    CHECK(metadata.lineage.broker_id == "broker-a");
    CHECK(metadata.lineage.receipt_id == "receipt-a");
    CHECK(metadata.lineage.producer_client_id == "client-a");
    CHECK(metadata.lineage.producer_registration_id == "registration-a");
    CHECK(metadata.lineage.session_id == "session-a");
    CHECK(metadata.lineage.instance_id == "instance-a");
    CHECK(metadata.lineage.publication_id == "publication-a");
    CHECK(metadata.lineage.producer_capability_id == "capability-a");
    CHECK(metadata.lineage.producer_operation_id == "dev.pulp.state/read@1");
    CHECK(metadata.lineage.producer_operation_version == 7);
    CHECK(metadata.lineage.original_grant_id == "grant-a");
    CHECK(metadata.lineage.consent_decision_id == "decision-a");
    CHECK(metadata.lineage.manifest_digest == std::string(64, 'a'));
    CHECK(metadata.lineage.producer_artifact_digest == std::string(64, 'b'));
    CHECK(metadata.sha256 == std::string(64, 'c'));
    CHECK(metadata.byte_size == 4);
    CHECK(metadata.content_type == "application/octet-stream");
    CHECK(metadata.created_at_unix_ms == 100);
    CHECK(metadata.expires_at_unix_ms == 200);
    CHECK(metadata.sensitivity == ControlArtifactSensitivity::Restricted);
    CHECK(metadata.deletion_state == ControlArtifactDeletionState::Active);
    CHECK(metadata.redaction_state == ControlArtifactRedactionState::Redacted);
    CHECK(result.bytes == std::vector<std::uint8_t>{0, 1, 2, 255});
    CHECK(result.eof);
    CHECK(server.handler_succeeded());

#else
    SUCCEED("authenticated local peer verification is currently macOS-only");
#endif
}
