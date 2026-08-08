#include <catch2/catch_test_macros.hpp>

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/control_endpoint.hpp>
#include <pulp/inspect/control_host_connection.hpp>
#include <pulp/inspect/control_host_router.hpp>
#include <pulp/runtime/crypto.hpp>

#include "support/thread_progress.hpp"

#include <choc/text/choc_JSON.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace pulp::events;
using namespace pulp::inspect;

namespace {

struct EndpointDirectory {
    std::filesystem::path path;
    std::filesystem::path socket;

    EndpointDirectory() {
        const auto random = pulp::runtime::secure_random_bytes(8);
        REQUIRE(random.has_value());
        path = std::filesystem::temp_directory_path() /
               ("pulp-control-endpoint-" + pulp::runtime::hex_encode(*random));
        REQUIRE(std::filesystem::create_directory(path));
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        socket = path / "broker.sock";
    }

    ~EndpointDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

struct ReceivedEnvelope {
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<ControlEnvelope> envelope;

    void receive(const void* data, std::size_t size) {
        auto decoded =
            decode_control_envelope(std::string_view(static_cast<const char*>(data), size));
        {
            std::lock_guard lock(mutex);
            envelope = std::move(decoded);
        }
        ready.notify_all();
    }

    bool wait() {
        std::unique_lock lock(mutex);
        return ready.wait_for(lock, 2s, [&] { return envelope.has_value(); });
    }

    void clear() {
        std::lock_guard lock(mutex);
        envelope.reset();
    }
};

#ifdef __APPLE__
struct HostOpenBroker {
    InterprocessConnectionServer server;
    ReceivedEnvelope received;
    std::mutex mutex;
    std::condition_variable ready;
    std::unique_ptr<InterprocessConnection> peer;
    std::atomic<unsigned> message_count{0};
    bool disconnected = false;

    bool start(const std::filesystem::path& endpoint) {
        server.on_client_connected = [&](std::unique_ptr<InterprocessConnection> connection) {
            connection->set_on_message([&](const void* data, std::size_t size) {
                message_count.fetch_add(1, std::memory_order_relaxed);
                received.receive(data, size);
            });
            connection->set_on_disconnected([&] {
                {
                    std::lock_guard lock(mutex);
                    disconnected = true;
                }
                ready.notify_all();
            });
            {
                std::lock_guard lock(mutex);
                peer = std::move(connection);
            }
            ready.notify_all();
        };
        return server.start(endpoint.string(), IpcTransport::LocalSocket);
    }

    ControlHostOpenEnvelope wait_for_open() {
        REQUIRE(received.wait());
        const auto* open = std::get_if<ControlHostOpenEnvelope>(&received.envelope->payload);
        REQUIRE(open != nullptr);
        return *open;
    }

    bool send(ControlHostOpenResult result) {
        std::unique_lock lock(mutex);
        REQUIRE(ready.wait_for(lock, 2s, [&] { return peer != nullptr; }));
        auto* connected = peer.get();
        lock.unlock();
        return connected->send_message(
            encode_control_envelope(ControlEnvelope{.payload = std::move(result)}));
    }

    bool send_raw(std::string_view bytes) {
        std::unique_lock lock(mutex);
        REQUIRE(ready.wait_for(lock, 2s, [&] { return peer != nullptr; }));
        auto* connected = peer.get();
        lock.unlock();
        return connected->send_message(bytes);
    }

    void drop() {
        InterprocessConnection* connected = nullptr;
        {
            std::lock_guard lock(mutex);
            connected = peer.get();
        }
        if (connected)
            connected->disconnect();
    }

    bool wait_for_disconnect() {
        std::unique_lock lock(mutex);
        return ready.wait_for(lock, 2s, [&] { return disconnected; });
    }

    ~HostOpenBroker() {
        drop();
        peer.reset();
        server.stop();
    }
};

ControlPeerEvidence observe_current_process(const std::filesystem::path& endpoint,
                                            ControlPeerRole role = ControlPeerRole::Client) {
    InterprocessConnectionServer observer;
    std::mutex mutex;
    std::condition_variable ready;
    std::unique_ptr<InterprocessConnection> accepted;
    observer.on_client_connected = [&](std::unique_ptr<InterprocessConnection> connection) {
        {
            std::lock_guard lock(mutex);
            accepted = std::move(connection);
        }
        ready.notify_all();
    };
    REQUIRE(observer.start(endpoint.string(), IpcTransport::LocalSocket));
    InterprocessConnection client;
    REQUIRE(client.connect(endpoint.string(), IpcTransport::LocalSocket, 2s));
    {
        std::unique_lock lock(mutex);
        REQUIRE(ready.wait_for(lock, 2s, [&] { return accepted != nullptr; }));
    }
    const auto evidence = observe_control_peer(*accepted, role);
    REQUIRE(evidence.has_value());
    client.disconnect();
    accepted.reset();
    observer.stop();
    return *evidence;
}

VerifiedControlPeerIdentity mint_verified(ControlPeerEvidence evidence) {
    ControlPeerVerifier verifier([](const ControlPeerEvidence&) { return true; });
    auto verified = verifier.verify(std::move(evidence));
    REQUIRE(verified.has_value());
    return std::move(*verified);
}

std::int64_t host_deadline() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch() + 2s)
        .count();
}
#endif

} // namespace

TEST_CASE("control endpoint health is observational and uses the canonical envelope",
          "[inspect][control][carrier][health]") {
#ifdef __APPLE__
    EndpointDirectory directory;
    ControlBroker broker;
    ControlService service{broker};
    std::size_t admission_calls = 0;
    ControlEndpoint endpoint{
        service,
        [&](std::string_view) -> std::optional<ControlConnectionAdmission> {
            ++admission_calls;
            return std::nullopt;
        },
        {
            .endpoint_path = directory.socket,
            .sdk_version = "0.791.0-test",
            .broker_id = "broker-test",
            .process_generation = 42,
        },
    };
    REQUIRE(endpoint.start());
    REQUIRE(endpoint.is_listening());

    ReceivedEnvelope received;
    InterprocessConnection client;
    client.set_max_message_bytes(kControlMaximumEnvelopeBytes);
    client.set_frame_read_timeout(2s);
    client.set_write_timeout(2s);
    client.set_on_message(
        [&](const void* data, std::size_t size) { received.receive(data, size); });
    REQUIRE(client.connect(directory.socket.string(), IpcTransport::LocalSocket, 2s));
    const auto request = encode_control_envelope(
        ControlEnvelope{.payload = ControlHealthEnvelope{.request_id = "health-1"}});
    REQUIRE(client.send_message(request));
    REQUIRE(received.wait());

    const auto* health = std::get_if<ControlHealthResult>(&received.envelope->payload);
    REQUIRE(health != nullptr);
    CHECK(health->request_id == "health-1");
    CHECK(health->sdk_version == "0.791.0-test");
    CHECK(health->broker_id == "broker-test");
    CHECK(health->process_generation == 42);
    CHECK(admission_calls == 0);

    client.disconnect();
    endpoint.stop();
    CHECK_FALSE(endpoint.is_listening());
#else
    SUCCEED("the authenticated control endpoint is currently macOS-only");
#endif
}

TEST_CASE("control endpoint authenticates a host role and routes on the canonical carrier",
          "[inspect][control][carrier][host]") {
#ifdef __APPLE__
    EndpointDirectory directory;
    const auto host_evidence =
        observe_current_process(directory.path / "h.sock", ControlPeerRole::StandaloneHost);
    const auto broker_evidence =
        observe_current_process(directory.path / "b.sock", ControlPeerRole::TrustedHostBridge);
    ControlBroker broker;
    ControlHostRouter router;
    ControlService service{broker, router.executor()};
    std::optional<ControlConnectionAdmission> admission{ControlConnectionAdmission{
        .admission_id = "host-admission-1",
        .expected_peer = {.evidence = host_evidence},
        .principal = ControlHostConnectionPrincipal{ControlRegistrationId{"registration-1"}},
        .expires_at = std::chrono::steady_clock::now() + 1min,
    }};
    ControlEndpoint endpoint{
        service,
        [&](std::string_view id) -> std::optional<ControlConnectionAdmission> {
            if (!admission || admission->admission_id != id)
                return std::nullopt;
            auto consumed = std::move(admission);
            admission.reset();
            return consumed;
        },
        {
            .endpoint_path = directory.socket,
            .sdk_version = "0.791.0-test",
            .broker_id = broker.broker_id().value,
            .process_generation = 42,
        },
        &router,
    };
    REQUIRE(endpoint.start());

    std::atomic<unsigned> execution_count{0};
    std::atomic<bool> blocked_execution_started{false};
    std::atomic<bool> release_blocked_execution{false};
    ControlHostConnection host{
        {.endpoint_path = directory.socket, .expected_broker = {.evidence = broker_evidence}},
        [&](const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
            const ControlExecutionContext& context) {
            CHECK(plan.registration_id == ControlRegistrationId{"registration-1"});
            CHECK(request.client_id.empty());
            CHECK(request.grant_id.empty());
            const auto execution = execution_count.fetch_add(1);
            if (execution == 0)
                return ControlExecutionOutcome{
                    .terminal_state = ControlReceiptState::CompletedAfterRevocation,
                    .result = {.result_code = ControlResultCode::CompletedAfterRevocation}};
            if (execution == 1) {
                context.complete_deferred(ControlExecutionOutcome{
                    .terminal_state = ControlReceiptState::UnknownNeedsRefresh,
                    .result = {.result_code = ControlResultCode::UnknownNeedsRefresh}});
                return ControlExecutionOutcome{.deferred = true};
            }
            if (execution == 2)
                return ControlExecutionOutcome{
                    .terminal_state = ControlReceiptState::Completed,
                    .result = {.artifacts = {{.artifact_id = "unsupported-artifact"}}}};
            blocked_execution_started.store(true);
            pulp::test::wait_for_condition([&] { return release_blocked_execution.load(); });
            return ControlExecutionOutcome{.terminal_state = ControlReceiptState::Completed};
        }};
    REQUIRE(host.connect());
    const auto opened = host.open_host("host-admission-1");
    REQUIRE(opened.accepted);
    CHECK(opened.registration_id == "registration-1");
    REQUIRE(router.connected(ControlRegistrationId{"registration-1"}));

    ControlAdmissionPlan execution_plan;
    execution_plan.registration_id = ControlRegistrationId{"registration-1"};
    execution_plan.receipt_id = ControlReceiptId{"receipt-1"};
    execution_plan.deadline_unix_ms = host_deadline();
    ControlRequestEnvelope request{.request_id = "request-1",
                                   .operation_id = "session.describe",
                                   .operation_version = 1,
                                   .params_json = "{}"};
    const auto outcome = router.executor()(
        execution_plan, request,
        {.report_progress = [](std::uint64_t, std::uint64_t, std::string) { return true; },
         .checkpoint = [] { return ControlExecutionCheckpoint::Continue; },
         .complete_deferred = [](ControlExecutionOutcome) {}});
    CHECK(outcome.terminal_state == ControlReceiptState::Failed);
    CHECK(outcome.result.result_code == ControlResultCode::InternalError);
    CHECK(outcome.result.explanation == "host executor returned a non-authorable terminal result");

    execution_plan.receipt_id = ControlReceiptId{"receipt-2"};
    request.request_id = "request-2";
    const auto unknown = router.executor()(
        execution_plan, request,
        {.report_progress = [](std::uint64_t, std::uint64_t, std::string) { return true; },
         .checkpoint = [] { return ControlExecutionCheckpoint::Continue; },
         .complete_deferred = [](ControlExecutionOutcome) {}});
    CHECK(unknown.terminal_state == ControlReceiptState::Failed);
    CHECK(unknown.result.result_code == ControlResultCode::InternalError);
    CHECK(unknown.result.explanation == "host executor returned a non-authorable terminal result");

    execution_plan.receipt_id = ControlReceiptId{"receipt-3"};
    request.request_id = "request-3";
    const auto artifacts = router.executor()(
        execution_plan, request,
        {.report_progress = [](std::uint64_t, std::uint64_t, std::string) { return true; },
         .checkpoint = [] { return ControlExecutionCheckpoint::Continue; },
         .complete_deferred = [](ControlExecutionOutcome) {}});
    CHECK(artifacts.terminal_state == ControlReceiptState::Failed);
    CHECK(artifacts.result.result_code == ControlResultCode::InternalError);
    CHECK(artifacts.result.explanation ==
          "host result artifacts are not supported by this protocol revision");

    execution_plan.receipt_id = ControlReceiptId{"receipt-4"};
    request.request_id = "request-4";
    auto interrupted = std::async(std::launch::async, [&] {
        return router.executor()(
            execution_plan, request,
            {.report_progress = [](std::uint64_t, std::uint64_t, std::string) { return true; },
             .checkpoint = [] { return ControlExecutionCheckpoint::Continue; },
             .complete_deferred = [](ControlExecutionOutcome) {}});
    });
    for (unsigned attempt = 0; attempt != 100 && !blocked_execution_started.load(); ++attempt)
        std::this_thread::sleep_for(1ms);
    const bool execution_started = blocked_execution_started.load();
    endpoint.stop();
    release_blocked_execution.store(true);
    REQUIRE(execution_started);
    REQUIRE(interrupted.wait_for(1s) == std::future_status::ready);
    CHECK(interrupted.get().terminal_state == ControlReceiptState::UnknownNeedsRefresh);
    for (unsigned attempt = 0;
         attempt != 100 && host.last_error_code() != "host-completion-send-failed"; ++attempt)
        std::this_thread::sleep_for(1ms);
    CHECK(host.last_error_code() == "host-completion-send-failed");
    CHECK_FALSE(host.is_connected());

    host.disconnect();
#else
    SUCCEED("the authenticated control endpoint is currently macOS-only");
#endif
}

TEST_CASE("host connection reliably poisons malformed broker input off the reader thread",
          "[inspect][control][carrier][host][security]") {
#ifdef __APPLE__
    EndpointDirectory directory;
    const auto broker_evidence =
        observe_current_process(directory.path / "p.sock", ControlPeerRole::TrustedHostBridge);
    InterprocessConnectionServer server;
    std::mutex mutex;
    std::condition_variable ready;
    std::unique_ptr<InterprocessConnection> peer;
    bool disconnected = false;
    server.on_client_connected = [&](std::unique_ptr<InterprocessConnection> connection) {
        connection->set_on_disconnected([&] {
            {
                std::lock_guard lock(mutex);
                disconnected = true;
            }
            ready.notify_all();
        });
        {
            std::lock_guard lock(mutex);
            peer = std::move(connection);
        }
        ready.notify_all();
    };
    REQUIRE(server.start(directory.socket.string(), IpcTransport::LocalSocket));
    ControlHostConnection host{
        {.endpoint_path = directory.socket, .expected_broker = {.evidence = broker_evidence}},
        [](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
           const ControlExecutionContext&) {
            return ControlExecutionOutcome{.terminal_state = ControlReceiptState::Completed};
        }};
    REQUIRE(host.connect());
    InterprocessConnection* connected_peer = nullptr;
    {
        std::unique_lock lock(mutex);
        REQUIRE(ready.wait_for(lock, 2s, [&] { return peer != nullptr; }));
        connected_peer = peer.get();
    }
    REQUIRE(connected_peer->send_message("not-json"));
    {
        std::unique_lock lock(mutex);
        REQUIRE(ready.wait_for(lock, 2s, [&] { return disconnected; }));
    }
    CHECK(host.last_error_code() == "malformed-host-frame");
    CHECK_FALSE(host.is_connected());
    host.disconnect();
    peer.reset();
    server.stop();
#else
    SUCCEED("the authenticated control endpoint is currently macOS-only");
#endif
}

TEST_CASE("host connection enrollment open validates its exact exchange",
          "[inspect][control][carrier][host][enrollment][security]") {
#ifdef __APPLE__
    EndpointDirectory directory;
    const auto broker_evidence =
        observe_current_process(directory.path / "eb.sock", ControlPeerRole::TrustedHostBridge);
    HostOpenBroker broker;
    REQUIRE(broker.start(directory.socket));
    ControlHostConnection host{
        {.endpoint_path = directory.socket, .expected_broker = {.evidence = broker_evidence}},
        [](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
           const ControlExecutionContext&) {
            return ControlExecutionOutcome{.terminal_state = ControlReceiptState::Completed};
        }};
    REQUIRE(host.connect());

    auto result =
        std::async(std::launch::async, [&] { return host.open_host_enrollment("enrollment-one"); });
    const auto request = broker.wait_for_open();
    CHECK(request.admission_id.empty());
    CHECK(request.enrollment_id == "enrollment-one");
    REQUIRE(broker.send({.request_id = request.request_id,
                         .accepted = true,
                         .registration_id = "broker-minted-registration"}));
    REQUIRE(result.wait_for(2s) == std::future_status::ready);
    const auto opened = result.get();
    CHECK(opened.accepted);
    CHECK(opened.request_id == request.request_id);
    CHECK(opened.registration_id == "broker-minted-registration");
    CHECK(host.is_host_open());

    const auto enrollment_replay = host.open_host_enrollment("enrollment-one");
    CHECK_FALSE(enrollment_replay.accepted);
    CHECK(enrollment_replay.error_code == "invalid-host-open");
    const auto cross_mode_replay = host.open_host("admission-one");
    CHECK_FALSE(cross_mode_replay.accepted);
    CHECK(cross_mode_replay.error_code == "invalid-host-open");
    std::this_thread::sleep_for(20ms);
    CHECK(broker.message_count.load(std::memory_order_relaxed) == 1);
    host.disconnect();
#endif
}

TEST_CASE("host connection enrollment failures poison or close the carrier",
          "[inspect][control][carrier][host][enrollment][security]") {
#ifdef __APPLE__
    const auto run = [](auto respond, std::chrono::milliseconds timeout = 2s) {
        EndpointDirectory directory;
        const auto broker_evidence =
            observe_current_process(directory.path / "fb.sock", ControlPeerRole::TrustedHostBridge);
        HostOpenBroker broker;
        REQUIRE(broker.start(directory.socket));
        ControlHostConnection host{
            {.endpoint_path = directory.socket, .expected_broker = {.evidence = broker_evidence}},
            [](const ControlAdmissionPlan&, const ControlRequestEnvelope&,
               const ControlExecutionContext&) {
                return ControlExecutionOutcome{.terminal_state = ControlReceiptState::Completed};
            }};
        REQUIRE(host.connect());
        auto result = std::async(std::launch::async,
                                 [&] { return host.open_host_enrollment("enrollment", timeout); });
        const auto request = broker.wait_for_open();
        respond(broker, request);
        REQUIRE(result.wait_for(2s) == std::future_status::ready);
        const auto opened = result.get();
        CHECK_FALSE(opened.accepted);
        host.disconnect();
        return std::pair{opened.error_code, host.last_error_code()};
    };

    SECTION("mismatched request identity") {
        const auto [result_error, connection_error] =
            run([](HostOpenBroker& broker, const ControlHostOpenEnvelope& request) {
                REQUIRE(broker.send({.request_id = request.request_id + "-wrong",
                                     .accepted = true,
                                     .registration_id = "broker-registration"}));
                REQUIRE(broker.wait_for_disconnect());
            });
        CHECK(result_error == "unexpected-host-opened");
        CHECK(connection_error == "unexpected-host-opened");
    }
    SECTION("malformed result") {
        const auto [result_error, connection_error] =
            run([](HostOpenBroker& broker, const ControlHostOpenEnvelope&) {
                REQUIRE(broker.send_raw("not-json"));
                REQUIRE(broker.wait_for_disconnect());
            });
        CHECK(result_error == "malformed-host-frame");
        CHECK(connection_error == "malformed-host-frame");
    }
    SECTION("peer disconnect") {
        const auto [result_error, connection_error] =
            run([](HostOpenBroker& broker, const ControlHostOpenEnvelope&) { broker.drop(); });
        CHECK(result_error == "connection-lost");
        CHECK(connection_error == "connection-lost");
    }
    SECTION("timeout") {
        const auto [result_error, connection_error] =
            run([](HostOpenBroker&, const ControlHostOpenEnvelope&) {}, 20ms);
        CHECK(result_error == "timeout");
        CHECK(connection_error == "timeout");
    }
#endif
}

TEST_CASE("control endpoint rejects malformed first frames without consuming admission",
          "[inspect][control][carrier][security]") {
#ifdef __APPLE__
    EndpointDirectory directory;
    ControlBroker broker;
    ControlService service{broker};
    std::size_t admission_calls = 0;
    ControlEndpoint endpoint{
        service,
        [&](std::string_view) -> std::optional<ControlConnectionAdmission> {
            ++admission_calls;
            return std::nullopt;
        },
        {
            .endpoint_path = directory.socket,
            .sdk_version = "0.791.0-test",
            .broker_id = "broker-test",
            .process_generation = 42,
        },
    };
    REQUIRE(endpoint.start());

    std::mutex mutex;
    std::condition_variable disconnected;
    bool lost = false;
    InterprocessConnection client;
    client.set_on_disconnected([&] {
        {
            std::lock_guard lock(mutex);
            lost = true;
        }
        disconnected.notify_all();
    });
    REQUIRE(client.connect(directory.socket.string(), IpcTransport::LocalSocket, 2s));
    REQUIRE(client.send_message("not-json"));
    {
        std::unique_lock lock(mutex);
        REQUIRE(disconnected.wait_for(lock, 2s, [&] { return lost; }));
    }
    CHECK(admission_calls == 0);
    endpoint.stop();
#else
    SUCCEED("the authenticated control endpoint is currently macOS-only");
#endif
}

TEST_CASE("control endpoint consumes one admission and binds the live peer to its client",
          "[inspect][control][carrier][identity]") {
#ifdef __APPLE__
    EndpointDirectory directory;
    const auto evidence = observe_current_process(directory.path / "observer.sock");
    const auto peer = mint_verified(evidence);
    ControlBroker broker;
    auto ticket = broker.issue_bootstrap(peer);
    REQUIRE(ticket.ticket.has_value());
    auto redeemed =
        broker.redeem_bootstrap(ticket.ticket->ticket_id, ticket.ticket->secret.bytes(), peer);
    REQUIRE(redeemed.client.has_value());
    const auto client_id = redeemed.client->client_id;

    ControlService service{broker};
    std::optional<ControlConnectionAdmission> admission{ControlConnectionAdmission{
        .admission_id = "admission-1",
        .expected_peer = {.evidence = evidence},
        .principal = ControlClientConnectionPrincipal{client_id},
        .expires_at = std::chrono::steady_clock::now() + 1min,
    }};
    std::size_t admission_calls = 0;
    ControlEndpoint endpoint{
        service,
        [&](std::string_view id) -> std::optional<ControlConnectionAdmission> {
            ++admission_calls;
            if (!admission || admission->admission_id != id)
                return std::nullopt;
            auto consumed = std::move(admission);
            admission.reset();
            return consumed;
        },
        {
            .endpoint_path = directory.socket,
            .sdk_version = "0.791.0-test",
            .broker_id = broker.broker_id().value,
            .process_generation = 42,
        },
    };
    REQUIRE(endpoint.start());

    ReceivedEnvelope received;
    InterprocessConnection client;
    client.set_on_message(
        [&](const void* data, std::size_t size) { received.receive(data, size); });
    REQUIRE(client.connect(directory.socket.string(), IpcTransport::LocalSocket, 2s));
    REQUIRE(client.send_message(
        encode_control_envelope(ControlEnvelope{.payload = ControlSessionOpenEnvelope{
                                                    .request_id = "open-1",
                                                    .admission_id = "admission-1",
                                                }})));
    REQUIRE(received.wait());
    const auto* opened = std::get_if<ControlSessionOpenResult>(&received.envelope->payload);
    REQUIRE(opened != nullptr);
    CHECK(opened->accepted);
    CHECK(opened->client_id == client_id.value);
    CHECK(admission_calls == 1);

    client.disconnect();

    ReceivedEnvelope replay_received;
    InterprocessConnection replay_client;
    replay_client.set_on_message(
        [&](const void* data, std::size_t size) { replay_received.receive(data, size); });
    REQUIRE(replay_client.connect(directory.socket.string(), IpcTransport::LocalSocket, 2s));
    REQUIRE(replay_client.send_message(
        encode_control_envelope(ControlEnvelope{.payload = ControlSessionOpenEnvelope{
                                                    .request_id = "open-replay",
                                                    .admission_id = "admission-1",
                                                }})));
    REQUIRE(replay_received.wait());
    const auto* replay = std::get_if<ControlSessionOpenResult>(&replay_received.envelope->payload);
    REQUIRE(replay != nullptr);
    CHECK_FALSE(replay->accepted);
    CHECK(replay->error_code == "admission-denied");
    CHECK(admission_calls == 2);
    replay_client.disconnect();

    endpoint.stop();
    CHECK_FALSE(broker.client(client_id).has_value());
#else
    SUCCEED("the authenticated control endpoint is currently macOS-only");
#endif
}

TEST_CASE("control endpoint serializes one-time admission consumption",
          "[inspect][control][carrier][admission][thread]") {
#ifdef __APPLE__
    EndpointDirectory directory;
    ControlBroker broker;
    ControlService service{broker};
    std::atomic<unsigned> active_calls{0};
    std::atomic<unsigned> maximum_active_calls{0};
    std::atomic<unsigned> total_calls{0};
    ControlEndpoint endpoint{
        service,
        [&](std::string_view) -> std::optional<ControlConnectionAdmission> {
            const auto active = active_calls.fetch_add(1) + 1;
            auto maximum = maximum_active_calls.load();
            while (active > maximum &&
                   !maximum_active_calls.compare_exchange_weak(maximum, active)) {
            }
            std::this_thread::sleep_for(100ms);
            active_calls.fetch_sub(1);
            total_calls.fetch_add(1);
            return std::nullopt;
        },
        {
            .endpoint_path = directory.socket,
            .sdk_version = "0.791.0-test",
            .broker_id = "broker-test",
            .process_generation = 42,
        },
    };
    REQUIRE(endpoint.start());

    ReceivedEnvelope first_received;
    ReceivedEnvelope second_received;
    InterprocessConnection first;
    InterprocessConnection second;
    first.set_on_message(
        [&](const void* data, std::size_t size) { first_received.receive(data, size); });
    second.set_on_message(
        [&](const void* data, std::size_t size) { second_received.receive(data, size); });
    REQUIRE(first.connect(directory.socket.string(), IpcTransport::LocalSocket, 2s));
    REQUIRE(second.connect(directory.socket.string(), IpcTransport::LocalSocket, 2s));
    const auto first_open = encode_control_envelope(
        ControlEnvelope{.payload = ControlSessionOpenEnvelope{.request_id = "open-first",
                                                              .admission_id = "shared-admission"}});
    const auto second_open = encode_control_envelope(
        ControlEnvelope{.payload = ControlSessionOpenEnvelope{.request_id = "open-second",
                                                              .admission_id = "shared-admission"}});
    REQUIRE(first.send_message(first_open));
    REQUIRE(second.send_message(second_open));
    REQUIRE(first_received.wait());
    REQUIRE(second_received.wait());
    CHECK(total_calls.load() == 2);
    CHECK(maximum_active_calls.load() == 1);

    first.disconnect();
    second.disconnect();
    endpoint.stop();
#else
    SUCCEED("the authenticated control endpoint is currently macOS-only");
#endif
}

TEST_CASE("control endpoint enrolls an authenticated local client and returns broker inventory",
          "[inspect][control][carrier][management]") {
#ifdef __APPLE__
    EndpointDirectory directory;
    ControlBroker broker;
    const auto host_peer = mint_verified(
        observe_current_process(directory.path / "m.sock", ControlPeerRole::StandaloneHost));
    ControlManifest manifest;
    manifest.profile = ControlBuildProfile::DeveloperLocal;
    manifest.target = "Fixture";
    manifest.product_name = "Fixture";
    manifest.bundle_id = "dev.pulp.fixture";
    manifest.build_id = "build:0123456789abcdef0123456789abcdef";
    manifest.endpoint_included = true;
    manifest.capabilities = {InspectorCapability::SessionDescribe, InspectorCapability::StateRead};
    const auto registered = broker.register_instance(
        host_peer,
        ControlRegistrationRequest{ControlHostTier::Standalone, "session-a", "instance-a",
                                   "publication-a", std::move(manifest), std::string(64, 'a')});
    REQUIRE(registered.registration.has_value());
    ControlService service{broker};
    ControlEndpoint endpoint{
        service,
        [](std::string_view) -> std::optional<ControlConnectionAdmission> { return std::nullopt; },
        {
            .endpoint_path = directory.socket,
            .sdk_version = "0.791.0-test",
            .broker_id = broker.broker_id().value,
            .process_generation = 42,
            .authorize_client =
                [](const ControlPeerEvidence& peer) {
                    return peer.role == ControlPeerRole::Client;
                },
        },
        nullptr,
        nullptr,
        &broker};
    REQUIRE(endpoint.start());

    ReceivedEnvelope received;
    InterprocessConnection client;
    client.set_on_message(
        [&](const void* data, std::size_t size) { received.receive(data, size); });
    REQUIRE(client.connect(directory.socket.string(), IpcTransport::LocalSocket, 2s));
    REQUIRE(client.send_message(encode_control_envelope(
        ControlEnvelope{.payload = ControlManagementEnvelope{.request_id = "management-enroll",
                                                             .command = "enroll"}})));
    REQUIRE(received.wait());
    const auto* enrolled = std::get_if<ControlManagementResult>(&received.envelope->payload);
    REQUIRE(enrolled != nullptr);
    CHECK(enrolled->request_id == "management-enroll");
    CHECK(enrolled->status_id == "accepted");

    received.clear();
    REQUIRE(client.send_message(encode_control_envelope(
        ControlEnvelope{.payload = ControlManagementEnvelope{.request_id = "management-instances",
                                                             .command = "instances"}})));
    REQUIRE(received.wait());
    const auto* inventory = std::get_if<ControlManagementResult>(&received.envelope->payload);
    REQUIRE(inventory != nullptr);
    CHECK(inventory->request_id == "management-instances");
    CHECK(inventory->status_id == "completed");
    const auto inventory_data = choc::json::parse(inventory->data_json);
    REQUIRE(inventory_data.isObject());
    CHECK(inventory_data["schema"].getString() == "pulp.control.instances.v1");
    CHECK(inventory_data["instances"].size() == 1);
    CHECK(inventory_data["instances"][0]["instance_id"].getString() == "instance-a");

    received.clear();
    REQUIRE(client.send_message(encode_control_envelope(ControlEnvelope{
        .payload = ControlManagementEnvelope{
            .request_id = "management-grant",
            .command = "grant-request",
            .params_json = R"({"instance_id":"instance-a","profile":"inspect-readonly"})"}})));
    REQUIRE(received.wait());
    const auto* grant = std::get_if<ControlManagementResult>(&received.envelope->payload);
    REQUIRE(grant != nullptr);
    CHECK(grant->request_id == "management-grant");
    CHECK(grant->status_id == "consent-required");

    client.disconnect();
    endpoint.stop();
#else
    SUCCEED("the authenticated control endpoint is currently macOS-only");
#endif
}
