#include <pulp/inspect/control_endpoint.hpp>

#include <pulp/inspect/control_broker.hpp>
#include <pulp/inspect/control_carrier.hpp>
#include <pulp/inspect/control_host_router.hpp>

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/runtime/base64.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::inspect {
namespace {

using events::InterprocessConnection;
using events::InterprocessConnectionServer;
using events::IpcTransport;

std::string_view sensitivity_id(ControlArtifactSensitivity value) {
    switch (value) {
    case ControlArtifactSensitivity::Public:
        return "public";
    case ControlArtifactSensitivity::Internal:
        return "internal";
    case ControlArtifactSensitivity::Sensitive:
        return "sensitive";
    case ControlArtifactSensitivity::Restricted:
        return "restricted";
    }
    return "sensitive";
}

std::string_view deletion_state_id(ControlArtifactDeletionState value) {
    switch (value) {
    case ControlArtifactDeletionState::Active:
        return "active";
    case ControlArtifactDeletionState::Deleted:
        return "deleted";
    }
    return "deleted";
}

std::string_view redaction_state_id(ControlArtifactRedactionState value) {
    switch (value) {
    case ControlArtifactRedactionState::Original:
        return "original";
    case ControlArtifactRedactionState::Redacted:
        return "redacted";
    }
    return "redacted";
}

ControlArtifactWireMetadata to_wire_metadata(const ControlArtifactMetadata& metadata) {
    return {
        .artifact_id = metadata.artifact_id,
        .broker_id = metadata.lineage.broker_id,
        .receipt_id = metadata.lineage.receipt_id,
        .producer_client_id = metadata.lineage.producer_client_id,
        .producer_registration_id = metadata.lineage.producer_registration_id,
        .session_id = metadata.lineage.session_id,
        .instance_id = metadata.lineage.instance_id,
        .publication_id = metadata.lineage.publication_id,
        .producer_capability_id = metadata.lineage.producer_capability_id,
        .producer_operation_id = metadata.lineage.producer_operation_id,
        .producer_operation_version = metadata.lineage.producer_operation_version,
        .original_grant_id = metadata.lineage.original_grant_id,
        .consent_decision_id = metadata.lineage.consent_decision_id,
        .manifest_digest = metadata.lineage.manifest_digest,
        .producer_artifact_digest = metadata.lineage.producer_artifact_digest,
        .sha256 = metadata.sha256,
        .byte_size = metadata.byte_size,
        .content_type = metadata.content_type,
        .created_at_unix_ms = metadata.created_at_unix_ms,
        .expires_at_unix_ms = metadata.expires_at_unix_ms,
        .sensitivity_id = std::string(sensitivity_id(metadata.sensitivity)),
        .deletion_state_id = std::string(deletion_state_id(metadata.deletion_state)),
        .redaction_state_id = std::string(redaction_state_id(metadata.redaction_state)),
    };
}

ControlEnvelope error_envelope(std::string request_id, std::string code, std::string explanation) {
    if (request_id.empty())
        request_id = "unknown";
    return {.payload = ControlErrorEnvelope{
                .request_id = std::move(request_id),
                .error_code = std::move(code),
                .explanation = std::move(explanation),
            }};
}

bool same_peer(const ControlPeerEvidence& left, const ControlPeerEvidence& right) {
    return left.role == right.role && left.user_id == right.user_id &&
           left.process_id == right.process_id && left.process_start_id == right.process_start_id &&
           left.executable_identity == right.executable_identity &&
           left.publisher_id == right.publisher_id;
}

std::string service_request_id(const ControlEnvelope& envelope) {
    if (const auto* request = std::get_if<ControlRequestEnvelope>(&envelope.payload))
        return request->request_id;
    if (const auto* cancellation = std::get_if<ControlCancelEnvelope>(&envelope.payload))
        return cancellation->request_id;
    return "unknown";
}

} // namespace

struct ControlEndpoint::Impl {
    struct ConnectionState {
        std::uint64_t id = 0;
        std::unique_ptr<InterprocessConnection> connection;
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<std::string> frames;
        std::size_t queued_bytes = 0;
        bool closing = false;
        std::atomic<bool> finished{false};
        std::optional<ControlService::Session> session;
        std::optional<ControlRegistrationId> host_registration;
        std::optional<VerifiedControlPeerIdentity> enrolled_peer;
        std::thread worker;
    };

    ControlService& service;
    ControlAdmissionConsumer consume_admission;
    ControlEndpointConfig config;
    ControlHostRouter* host_router = nullptr;
    ControlEndpointEnrollmentContext* enrollment_context = nullptr;
    InterprocessConnectionServer server;
    std::atomic<bool> stopping{false};
    std::atomic<std::uint64_t> next_connection_id{1};
    std::mutex admission_mutex;
    mutable std::mutex connections_mutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<ConnectionState>> connections;

    Impl(ControlService& service_in, ControlAdmissionConsumer consumer,
         ControlEndpointConfig config_in, ControlHostRouter* router,
         ControlEndpointEnrollmentContext* enrollment)
        : service(service_in), consume_admission(std::move(consumer)), config(std::move(config_in)),
          host_router(router), enrollment_context(enrollment) {}

    bool send(ConnectionState& state, const ControlEnvelope& envelope) {
        const auto encoded = encode_control_envelope(envelope);
        return !encoded.empty() && state.connection->send_message(encoded);
    }

    bool send_error(ConnectionState& state, std::string request_id, std::string code,
                    std::string explanation) {
        return send(state,
                    error_envelope(std::move(request_id), std::move(code), std::move(explanation)));
    }

    void handle_health(ConnectionState& state, const ControlHealthEnvelope& request) {
        if (!send(state, ControlEnvelope{.payload = ControlHealthResult{
                                             .request_id = request.request_id,
                                             .sdk_version = config.sdk_version,
                                             .protocol_versions = {kControlProtocolVersion,
                                                                   kControlProtocolVersion},
                                             .broker_id = config.broker_id,
                                             .process_generation = config.process_generation,
                                         }}))
            close(state);
    }

    void handle_open(ConnectionState& state, const ControlSessionOpenEnvelope& request,
                     const std::shared_ptr<ConnectionState>& shared_state) {
        if (state.session || state.host_registration) {
            send_error(state, request.request_id, "session-already-open",
                       "the connection already owns a control session");
            return;
        }

        std::optional<ControlConnectionAdmission> admission;
        {
            std::lock_guard lock(admission_mutex);
            admission = consume_admission ? consume_admission(request.admission_id) : std::nullopt;
        }
        if (!admission || admission->admission_id != request.admission_id ||
            admission->expires_at <= std::chrono::steady_clock::now()) {
            send(state,
                 ControlEnvelope{.payload = ControlSessionOpenResult{
                                     .request_id = request.request_id,
                                     .accepted = false,
                                     .error_code = "admission-denied",
                                     .explanation = "admission is missing, expired, or replayed",
                                 }});
            return;
        }

        const auto* principal =
            std::get_if<ControlClientConnectionPrincipal>(&admission->principal);
        if (!principal) {
            send(state,
                 ControlEnvelope{.payload = ControlSessionOpenResult{
                                     .request_id = request.request_id,
                                     .accepted = false,
                                     .error_code = "principal-role-mismatch",
                                     .explanation = "the admission belongs to a host principal",
                                 }});
            return;
        }

        auto peer = verify_control_peer(*state.connection, admission->expected_peer);
        if (!peer) {
            send(state,
                 ControlEnvelope{.payload = ControlSessionOpenResult{
                                     .request_id = request.request_id,
                                     .accepted = false,
                                     .error_code = "peer-verification-failed",
                                     .explanation = "the live local peer did not match admission",
                                 }});
            return;
        }

        std::weak_ptr<ConnectionState> weak_state = shared_state;
        auto session = service.open_session(
            *peer, principal->client_id, [weak_state](const ControlProgressEnvelope& progress) {
                const auto locked = weak_state.lock();
                if (!locked)
                    return false;
                {
                    std::lock_guard guard(locked->mutex);
                    if (locked->closing)
                        return false;
                }
                if (!locked->connection->is_connected())
                    return false;
                const auto encoded = encode_control_envelope(ControlEnvelope{.payload = progress});
                return !encoded.empty() && locked->connection->send_message(encoded);
            });
        if (!session.is_open()) {
            send(state, ControlEnvelope{
                            .payload = ControlSessionOpenResult{
                                .request_id = request.request_id,
                                .accepted = false,
                                .error_code = "client-identity-mismatch",
                                .explanation = "the client identity does not belong to this peer",
                            }});
            return;
        }

        state.session.emplace(std::move(session));
        if (!send(state, ControlEnvelope{.payload = ControlSessionOpenResult{
                                             .request_id = request.request_id,
                                             .accepted = true,
                                             .client_id = principal->client_id.value,
                                         }}))
            close(state);
    }

    void handle_host_enrollment(ConnectionState& state, const ControlHostOpenEnvelope& request,
                                const std::shared_ptr<ConnectionState>& shared_state) {
        ControlHostOpenResult response{.request_id = request.request_id};
        if (!enrollment_context) {
            response.error_code = "enrollment-denied";
            response.explanation = "host enrollment is not enabled for this endpoint";
            (void)send(state, ControlEnvelope{.payload = std::move(response)});
            return;
        }

        // This is deliberately the first authority operation for a claim. A
        // malformed, mismatched, or racing claimant permanently burns it.
        auto plan = enrollment_context->enrollments.consume(request.enrollment_id);
        if (!plan) {
            response.error_code = "enrollment-denied";
            response.explanation = "enrollment is missing, expired, or replayed";
            (void)send(state, ControlEnvelope{.payload = std::move(response)});
            return;
        }
        if (state.session || state.host_registration || !host_router ||
            plan->broker_generation() != config.process_generation ||
            plan->expires_at() <= std::chrono::steady_clock::now()) {
            response.error_code = "enrollment-denied";
            response.explanation = "the endpoint cannot accept this enrollment";
            (void)send(state, ControlEnvelope{.payload = std::move(response)});
            return;
        }

        auto peer = verify_control_peer(*state.connection, plan->expected_peer());
        if (!peer) {
            response.error_code = "peer-verification-failed";
            response.explanation = "the live local peer did not match the launched host";
            (void)send(state, ControlEnvelope{.payload = std::move(response)});
            return;
        }

        auto registered =
            enrollment_context->broker.register_instance(*peer, plan->snapshot().registration());
        if (registered.status != ControlIdentityStatus::Accepted || !registered.registration) {
            response.error_code = "registration-denied";
            response.explanation = "the broker rejected the trusted host registration";
            (void)send(state, ControlEnvelope{.payload = std::move(response)});
            return;
        }
        const auto registration_id = registered.registration->registration_id;
        auto rollback_registration = [&] {
            (void)enrollment_context->broker.unregister_instance(registration_id, *peer,
                                                                 "endpoint-enrollment-rollback");
        };

        auto issued = enrollment_context->admissions.issue(
            ControlPeerExpectation{.evidence = peer->evidence()},
            ControlHostConnectionPrincipal{registration_id});
        if (issued.status != ControlConnectionAdmissionStatus::Issued || !issued.ticket) {
            rollback_registration();
            response.error_code = "admission-denied";
            response.explanation = "the endpoint could not bind the registered host admission";
            (void)send(state, ControlEnvelope{.payload = std::move(response)});
            return;
        }
        auto admission = enrollment_context->admissions.consume(issued.ticket->admission_id);
        const auto* principal =
            admission ? std::get_if<ControlHostConnectionPrincipal>(&admission->principal)
                      : nullptr;
        if (!admission || admission->admission_id != issued.ticket->admission_id ||
            admission->expires_at <= std::chrono::steady_clock::now() || !principal ||
            principal->registration_id != registration_id ||
            !same_peer(admission->expected_peer.evidence, peer->evidence())) {
            rollback_registration();
            response.error_code = "admission-denied";
            response.explanation = "the endpoint could not consume the exact host admission";
            (void)send(state, ControlEnvelope{.payload = std::move(response)});
            return;
        }

        std::weak_ptr<ConnectionState> weak_state = shared_state;
        if (!host_router->attach(
                registration_id, state.id, [weak_state](const ControlEnvelope& envelope) {
                    const auto locked = weak_state.lock();
                    if (!locked || !locked->connection->is_connected())
                        return false;
                    const auto encoded = encode_control_envelope(envelope);
                    return !encoded.empty() && locked->connection->send_message(encoded);
                })) {
            rollback_registration();
            response.error_code = "host-already-connected";
            response.explanation = "the registration could not attach to the host router";
            (void)send(state, ControlEnvelope{.payload = std::move(response)});
            return;
        }

        state.host_registration = registration_id;
        state.enrolled_peer = std::move(*peer);
        response.accepted = true;
        response.registration_id = registration_id.value;
        if (!send(state, ControlEnvelope{.payload = std::move(response)}))
            close(state);
    }

    void handle_host_open(ConnectionState& state, const ControlHostOpenEnvelope& request,
                          const std::shared_ptr<ConnectionState>& shared_state) {
        if (!request.enrollment_id.empty()) {
            handle_host_enrollment(state, request, shared_state);
            return;
        }

        ControlHostOpenResult response{.request_id = request.request_id};
        if (state.session || state.host_registration || !host_router) {
            response.error_code = "host-routing-unavailable";
            response.explanation = "the endpoint is not accepting a host connection";
            (void)send(state, ControlEnvelope{.payload = std::move(response)});
            return;
        }

        std::optional<ControlConnectionAdmission> admission;
        {
            std::lock_guard lock(admission_mutex);
            admission = consume_admission ? consume_admission(request.admission_id) : std::nullopt;
        }
        if (!admission || admission->admission_id != request.admission_id ||
            admission->expires_at <= std::chrono::steady_clock::now()) {
            response.error_code = "admission-denied";
            response.explanation = "admission is missing, expired, or replayed";
            (void)send(state, ControlEnvelope{.payload = std::move(response)});
            return;
        }
        const auto* principal = std::get_if<ControlHostConnectionPrincipal>(&admission->principal);
        if (!principal) {
            response.error_code = "principal-role-mismatch";
            response.explanation = "the admission belongs to a client principal";
            (void)send(state, ControlEnvelope{.payload = std::move(response)});
            return;
        }
        if (!verify_control_peer(*state.connection, admission->expected_peer)) {
            response.error_code = "peer-verification-failed";
            response.explanation = "the live local peer did not match admission";
            (void)send(state, ControlEnvelope{.payload = std::move(response)});
            return;
        }
        std::weak_ptr<ConnectionState> weak_state = shared_state;
        if (!host_router->attach(principal->registration_id, state.id,
                                 [weak_state](const ControlEnvelope& envelope) {
                                     const auto locked = weak_state.lock();
                                     if (!locked || !locked->connection->is_connected())
                                         return false;
                                     const auto encoded = encode_control_envelope(envelope);
                                     return !encoded.empty() &&
                                            locked->connection->send_message(encoded);
                                 })) {
            response.error_code = "host-already-connected";
            response.explanation = "the registration already owns a live host connection";
            (void)send(state, ControlEnvelope{.payload = std::move(response)});
            return;
        }
        state.host_registration = principal->registration_id;
        response.accepted = true;
        response.registration_id = principal->registration_id.value;
        if (!send(state, ControlEnvelope{.payload = std::move(response)}))
            close(state);
    }

    void handle_artifact_read(ConnectionState& state, const ControlArtifactReadEnvelope& request) {
        if (!state.session) {
            send_error(state, request.request_id, "session-required",
                       "artifact reads require an authenticated session");
            return;
        }
        const auto result = state.session->read_artifact(request.artifact_id, request.offset,
                                                         request.maximum_bytes);
        std::optional<ControlArtifactWireMetadata> metadata;
        if (result.metadata)
            metadata = to_wire_metadata(*result.metadata);
        if (!send(state,
                  ControlEnvelope{
                      .payload = ControlArtifactReadResponseEnvelope{
                          .request_id = request.request_id,
                          .status_id = std::string(control_artifact_status_id(result.status)),
                          .metadata = std::move(metadata),
                          .bytes_base64 =
                              runtime::base64_encode(result.bytes.data(), result.bytes.size()),
                          .eof = result.eof,
                          .explanation = result.explanation,
                      }}))
            close(state);
    }

    void handle_service_dispatch(ConnectionState& state, const ControlEnvelope& request,
                                 std::string_view encoded) {
        const auto request_id = service_request_id(request);
        if (!state.session) {
            send_error(state, request_id, "session-required",
                       "control requests require an authenticated session");
            return;
        }
        const auto result = state.session->dispatch(encoded);
        if (result.response) {
            if (!send(state, *result.response))
                close(state);
            return;
        }
        send_error(state, request_id, std::string(control_service_status_id(result.status)),
                   result.explanation);
    }

    void handle_frame(ConnectionState& state, const std::shared_ptr<ConnectionState>& shared_state,
                      std::string_view encoded) {
        ControlProtocolDiagnostics diagnostics;
        const auto envelope = decode_control_envelope(encoded, &diagnostics);
        if (!envelope) {
            close(state);
            return;
        }
        if (!control_envelope_allowed(*envelope, ControlEnvelopeDirection::ClientToBroker) &&
            !control_envelope_allowed(*envelope, ControlEnvelopeDirection::HostToBroker)) {
            close(state);
            return;
        }
        if (const auto* health = std::get_if<ControlHealthEnvelope>(&envelope->payload)) {
            handle_health(state, *health);
        } else if (const auto* open = std::get_if<ControlSessionOpenEnvelope>(&envelope->payload)) {
            handle_open(state, *open, shared_state);
        } else if (const auto* open = std::get_if<ControlHostOpenEnvelope>(&envelope->payload)) {
            handle_host_open(state, *open, shared_state);
        } else if (state.host_registration) {
            if (!host_router ||
                !host_router->receive(*state.host_registration, state.id, *envelope))
                close(state);
        } else if (control_envelope_allowed(*envelope, ControlEnvelopeDirection::HostToBroker)) {
            close(state);
        } else if (const auto* read =
                       std::get_if<ControlArtifactReadEnvelope>(&envelope->payload)) {
            handle_artifact_read(state, *read);
        } else {
            handle_service_dispatch(state, *envelope, encoded);
        }
    }

    void close(ConnectionState& state) {
        {
            std::lock_guard lock(state.mutex);
            state.closing = true;
            state.frames.clear();
            state.queued_bytes = 0;
        }
        state.ready.notify_all();
    }

    void run_connection(const std::shared_ptr<ConnectionState>& state) {
        for (;;) {
            std::string frame;
            {
                std::unique_lock lock(state->mutex);
                state->ready.wait(lock, [&] { return state->closing || !state->frames.empty(); });
                if (state->closing)
                    break;
                frame = std::move(state->frames.front());
                state->frames.pop_front();
                state->queued_bytes -= frame.size();
            }
            handle_frame(*state, state, frame);
        }
        state->connection->disconnect();
        if (state->host_registration && host_router)
            host_router->detach(*state->host_registration, state->id);
        if (state->host_registration && state->enrolled_peer && enrollment_context)
            (void)enrollment_context->broker.unregister_instance(
                *state->host_registration, *state->enrolled_peer,
                "endpoint-enrolled-host-disconnected");
        state->host_registration.reset();
        state->enrolled_peer.reset();
        state->session.reset();
        state->finished.store(true, std::memory_order_release);
    }

    void reap_finished() {
        std::vector<std::shared_ptr<ConnectionState>> finished;
        {
            std::lock_guard lock(connections_mutex);
            for (auto it = connections.begin(); it != connections.end();) {
                if (it->second->finished.load(std::memory_order_acquire)) {
                    finished.push_back(std::move(it->second));
                    it = connections.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& state : finished)
            if (state->worker.joinable())
                state->worker.join();
    }

    void accept(std::unique_ptr<InterprocessConnection> connection) {
        reap_finished();
        if (stopping.load(std::memory_order_acquire)) {
            connection->disconnect();
            return;
        }

        auto state = std::make_shared<ConnectionState>();
        state->id = next_connection_id.fetch_add(1, std::memory_order_relaxed);
        state->connection = std::move(connection);
        std::weak_ptr<ConnectionState> weak_state = state;
        state->connection->set_on_message([this, weak_state](const void* data, std::size_t size) {
            const auto locked = weak_state.lock();
            if (!locked || stopping.load(std::memory_order_acquire))
                return;
            {
                std::lock_guard guard(locked->mutex);
                if (locked->closing)
                    return;
                if (locked->frames.size() >= config.maximum_queued_frames_per_connection ||
                    size > config.maximum_queued_bytes_per_connection -
                               std::min(config.maximum_queued_bytes_per_connection,
                                        locked->queued_bytes)) {
                    locked->closing = true;
                    locked->frames.clear();
                    locked->queued_bytes = 0;
                } else {
                    locked->frames.emplace_back(static_cast<const char*>(data), size);
                    locked->queued_bytes += size;
                }
            }
            locked->ready.notify_one();
        });
        state->connection->set_on_disconnected([weak_state] {
            if (const auto locked = weak_state.lock()) {
                {
                    std::lock_guard guard(locked->mutex);
                    locked->closing = true;
                    locked->frames.clear();
                    locked->queued_bytes = 0;
                }
                locked->ready.notify_all();
            }
        });

        bool rejected = false;
        {
            std::lock_guard lock(connections_mutex);
            if (connections.size() >= config.maximum_connections) {
                rejected = true;
            } else {
                connections.emplace(state->id, state);
            }
        }
        if (rejected) {
            state->connection->disconnect();
            return;
        }
        state->worker = std::thread([this, state] { run_connection(state); });
    }

    bool start() {
        if (config.endpoint_path.empty() || !config.endpoint_path.is_absolute() ||
            config.maximum_connections == 0 || config.maximum_queued_frames_per_connection == 0 ||
            config.maximum_queued_bytes_per_connection < kControlMaximumEnvelopeBytes ||
            config.sdk_version.empty() || config.broker_id.empty() ||
            config.process_generation == 0 || !consume_admission)
            return false;
        if (!prepare_control_runtime_directory(config.endpoint_path.parent_path()))
            return false;
        stopping.store(false, std::memory_order_release);
        server.set_max_message_bytes(kControlMaximumEnvelopeBytes);
        server.set_write_timeout(config.write_timeout);
        server.set_frame_read_timeout(config.frame_read_timeout);
        server.on_client_connected = [this](std::unique_ptr<InterprocessConnection> connection) {
            accept(std::move(connection));
        };
        return server.start(config.endpoint_path.string(), IpcTransport::LocalSocket);
    }

    void stop() noexcept {
        if (stopping.exchange(true, std::memory_order_acq_rel))
            return;
        server.stop();
        server.on_client_connected = {};

        std::vector<std::shared_ptr<ConnectionState>> snapshot;
        {
            std::lock_guard lock(connections_mutex);
            for (auto& [id, state] : connections) {
                (void)id;
                snapshot.push_back(state);
            }
            connections.clear();
        }
        for (const auto& state : snapshot) {
            state->connection->set_on_message({});
            state->connection->set_on_disconnected({});
            close(*state);
            state->connection->disconnect();
        }
        for (const auto& state : snapshot)
            if (state->worker.joinable())
                state->worker.join();
    }
};

ControlEndpoint::ControlEndpoint(ControlService& service,
                                 ControlAdmissionConsumer consume_admission,
                                 ControlEndpointConfig config, ControlHostRouter* host_router,
                                 ControlEndpointEnrollmentContext* enrollment_context)
    : impl_(std::make_unique<Impl>(service, std::move(consume_admission), std::move(config),
                                   host_router, enrollment_context)) {}

ControlEndpoint::~ControlEndpoint() {
    stop();
}

bool ControlEndpoint::start() {
    return !impl_->server.is_running() && impl_->start();
}

void ControlEndpoint::stop() noexcept {
    impl_->stop();
}

bool ControlEndpoint::is_listening() const noexcept {
    return impl_->server.is_running();
}

const std::filesystem::path& ControlEndpoint::endpoint_path() const noexcept {
    return impl_->config.endpoint_path;
}

} // namespace pulp::inspect
