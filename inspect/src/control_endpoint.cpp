#include <pulp/inspect/control_endpoint.hpp>

#if defined(_MSC_VER)
#define PULP_CONTROL_COMPONENT_MARKER __declspec(dllexport)
#else
#define PULP_CONTROL_COMPONENT_MARKER __attribute__((used, visibility("default")))
#endif

extern "C" PULP_CONTROL_COMPONENT_MARKER const volatile char
    pulp_control_endpoint_shipping_marker_v1[] =
        "PULP_INSPECT_SHIPPING_MANIFEST_V1";

#undef PULP_CONTROL_COMPONENT_MARKER

#include <pulp/inspect/control_broker.hpp>
#include <pulp/inspect/control_carrier.hpp>
#include <pulp/inspect/control_host_router.hpp>
#include <pulp/inspect/control_manifest.hpp>

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/runtime/base64.hpp>

#include <choc/text/choc_JSON.h>

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

bool has_only_members(const choc::value::Value& object,
                      std::initializer_list<std::string_view> allowed) {
    for (std::uint32_t index = 0; index < object.size(); ++index) {
        const auto name = object.getObjectMemberAt(index).name;
        if (std::ranges::find(allowed, name) == allowed.end())
            return false;
    }
    return true;
}

std::optional<ControlTrustedHostLaunchIntent>
trusted_host_intent_from_json(const choc::value::Value& params) {
    if (!has_only_members(params, {"executable", "arguments", "working_directory", "host_tier"}) ||
        !params.hasObjectMember("executable") || !params["executable"].isString() ||
        !params.hasObjectMember("arguments") || !params["arguments"].isArray() ||
        !params.hasObjectMember("working_directory") ||
        !params["working_directory"].isString() || !params.hasObjectMember("host_tier") ||
        !params["host_tier"].isString() || params["arguments"].size() > 128) {
        return std::nullopt;
    }
    const auto executable = std::string(params["executable"].getString());
    const auto working_directory = std::string(params["working_directory"].getString());
    if (executable.size() > 4096 || working_directory.size() > 4096)
        return std::nullopt;
    ControlTrustedHostLaunchIntent intent{
        .executable = executable,
        .working_directory = working_directory,
    };
    std::size_t argument_bytes = 0;
    for (std::uint32_t index = 0; index < params["arguments"].size(); ++index) {
        const auto argument = params["arguments"][index];
        if (!argument.isString())
            return std::nullopt;
        auto value = std::string(argument.getString());
        argument_bytes += value.size();
        if (argument_bytes > 64u * 1024u)
            return std::nullopt;
        intent.arguments.push_back(std::move(value));
    }
    const auto tier = params["host_tier"].getString();
    if (tier == control_host_tier_id(ControlHostTier::Standalone))
        intent.host_tier = ControlHostTier::Standalone;
    else if (tier == control_host_tier_id(ControlHostTier::OfflineJob))
        intent.host_tier = ControlHostTier::OfflineJob;
    else
        return std::nullopt;
    return intent;
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
        std::optional<ControlRegistration> pending_host_registration;
        std::optional<VerifiedControlPeerIdentity> enrolled_peer;
        // Retains the broker-owned executable/runtime snapshot for the live
        // host connection so pinned lazy dependencies remain available after
        // the one-use enrollment claim has been consumed.
        std::optional<ControlHostEnrollmentPlan> host_enrollment;
        std::thread worker;
    };

    ControlService& service;
    ControlAdmissionConsumer consume_admission;
    ControlEndpointConfig config;
    ControlHostRouter* host_router = nullptr;
    ControlEndpointEnrollmentContext* enrollment_context = nullptr;
    ControlBroker* management_broker = nullptr;
    InterprocessConnectionServer server;
    std::atomic<bool> stopping{false};
    std::atomic<std::uint64_t> next_connection_id{1};
    std::mutex admission_mutex;
    mutable std::mutex connections_mutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<ConnectionState>> connections;

    Impl(ControlService& service_in, ControlAdmissionConsumer consumer,
         ControlEndpointConfig config_in, ControlHostRouter* router,
         ControlEndpointEnrollmentContext* enrollment, ControlBroker* management)
        : service(service_in), consume_admission(std::move(consumer)), config(std::move(config_in)),
          host_router(router), enrollment_context(enrollment),
          management_broker(management   ? management
                            : enrollment ? &enrollment->broker
                                         : nullptr) {}

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
        if (state.session || state.host_registration || state.pending_host_registration ||
            !host_router ||
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

        auto registered = enrollment_context->broker.register_instance(
            *peer, plan->snapshot().registration(), false);
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

        state.pending_host_registration = *registered.registration;
        state.enrolled_peer = std::move(*peer);
        state.host_enrollment = std::move(*plan);
        response.accepted = true;
        response.registration_id = registration_id.value;
        response.broker_id = registered.registration->broker_id.value;
        response.session_id = registered.registration->session_id;
        response.instance_id = registered.registration->instance_id;
        response.publication_id = registered.registration->publication_id;
        response.instance_generation = registered.registration->publication_id;
        response.manifest_digest = registered.registration->manifest_digest;
        response.producer_artifact_digest = registered.registration->artifact_digest;
        if (!send(state, ControlEnvelope{.payload = std::move(response)}))
            close(state);
    }

    void handle_host_ready(ConnectionState& state, const ControlHostReadyEnvelope& request,
                           const std::shared_ptr<ConnectionState>& shared_state) {
        ControlHostReadyResult response{.request_id = request.request_id};
        if (!state.pending_host_registration || !state.enrolled_peer || state.host_registration ||
            !host_router || !enrollment_context ||
            state.pending_host_registration->registration_id.value != request.registration_id) {
            response.error_code = "host-ready-denied";
            response.explanation = "host readiness does not match a pending enrollment";
            (void)send(state, ControlEnvelope{.payload = std::move(response)});
            close(state);
            return;
        }
        const auto registration = *state.pending_host_registration;
        std::weak_ptr<ConnectionState> weak_state = shared_state;
        if (!host_router->attach_slot(
                registration.registration_id, state.id, registration.instance_id,
                registration.publication_id,
                [weak_state](const ControlEnvelope& envelope) {
                    const auto locked = weak_state.lock();
                    if (!locked || !locked->connection->is_connected())
                        return false;
                    const auto encoded = encode_control_envelope(envelope);
                    return !encoded.empty() && locked->connection->send_message(encoded);
                })) {
            response.error_code = "host-already-connected";
            response.explanation = "the ready host could not attach to its exact route";
            (void)send(state, ControlEnvelope{.payload = std::move(response)});
            close(state);
            return;
        }
        if (!enrollment_context->broker.publish_instance(registration.registration_id,
                                                         *state.enrolled_peer)) {
            host_router->detach(registration.registration_id, state.id);
            response.error_code = "host-publish-denied";
            response.explanation = "the broker could not publish the ready host";
            (void)send(state, ControlEnvelope{.payload = std::move(response)});
            close(state);
            return;
        }
        state.host_registration = registration.registration_id;
        state.pending_host_registration.reset();
        response.accepted = true;
        response.liveness_generation = registration.liveness_generation;
        if (!send(state, ControlEnvelope{.payload = std::move(response)}))
            close(state);
    }

    void handle_host_heartbeat(ConnectionState& state,
                               const ControlHostHeartbeatEnvelope& request) {
        ControlHostHeartbeatResult response{.request_id = request.request_id};
        if (!state.host_registration || !state.enrolled_peer || !enrollment_context ||
            *state.host_registration != ControlRegistrationId{request.registration_id}) {
            response.error_code = "heartbeat-denied";
            response.explanation = "heartbeat does not match the enrolled host";
        } else {
            const auto current = enrollment_context->broker.registration(*state.host_registration);
            if (!current || current->liveness_generation != request.liveness_generation ||
                !enrollment_context->broker.heartbeat(*state.host_registration,
                                                      *state.enrolled_peer)) {
                response.error_code = "heartbeat-denied";
                response.explanation = "host heartbeat generation is stale";
            } else {
                const auto refreshed =
                    enrollment_context->broker.registration(*state.host_registration);
                response.accepted = static_cast<bool>(refreshed);
                response.liveness_generation = refreshed ? refreshed->liveness_generation : 0;
            }
        }
        const bool accepted = response.accepted;
        if (!send(state, ControlEnvelope{.payload = std::move(response)}) || !accepted)
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
        const auto registration = management_broker
                                      ? management_broker->registration(principal->registration_id)
                                      : std::optional<ControlRegistration>{};
        if (!registration) {
            host_router->detach(principal->registration_id, state.id);
            state.host_registration.reset();
            response = {.request_id = request.request_id,
                        .error_code = "registration-denied",
                        .explanation = "the host registration is no longer published"};
        } else {
            response.broker_id = registration->broker_id.value;
            response.session_id = registration->session_id;
            response.instance_id = registration->instance_id;
            response.publication_id = registration->publication_id;
            response.instance_generation = registration->publication_id;
            response.manifest_digest = registration->manifest_digest;
            response.producer_artifact_digest = registration->artifact_digest;
        }
        if (!send(state, ControlEnvelope{.payload = std::move(response)}))
            close(state);
    }

    void handle_management(ConnectionState& state, const ControlManagementEnvelope& request,
                           const std::shared_ptr<ConnectionState>& shared_state) {
        auto reply = [&](std::string status, std::string data = "{}",
                         std::string explanation = {}) {
            return send(state, ControlEnvelope{.payload = ControlManagementResult{
                                                   .request_id = request.request_id,
                                                   .status_id = std::move(status),
                                                   .data_json = std::move(data),
                                                   .explanation = std::move(explanation)}});
        };
        if (!management_broker) {
            (void)reply("unavailable", "{}", "broker management is unavailable");
            return;
        }
        if (request.command == "enroll") {
            if (state.session || request.params_json != "{}" || !config.authorize_client) {
                (void)reply("enrollment-denied", "{}", "client enrollment is denied");
                return;
            }
            auto evidence = observe_control_peer(*state.connection, ControlPeerRole::Client);
            if (!evidence || !config.authorize_client(*evidence)) {
                (void)reply("enrollment-denied", "{}",
                            "the local client did not satisfy broker policy");
                return;
            }
            const auto durable_principal = config.durable_client_principal
                                               ? config.durable_client_principal(*evidence)
                                               : std::optional<
                                                     ControlEndpointConfig::DurableClientPrincipal>{};
            if (config.durable_client_principal &&
                (!durable_principal || durable_principal->value.empty())) {
                (void)reply("enrollment-denied", "{}",
                            "the broker could not derive a durable client principal");
                return;
            }
            ControlPeerVerifier verifier([](const ControlPeerEvidence&) { return true; });
            auto peer = verifier.verify(std::move(*evidence));
            auto bootstrap =
                peer ? management_broker->issue_bootstrap(*peer) : ControlBootstrapResult{};
            auto client =
                bootstrap.ticket
                    ? management_broker->redeem_bootstrap(bootstrap.ticket->ticket_id,
                                                          bootstrap.ticket->secret.bytes(), *peer,
                                                          durable_principal
                                                              ? durable_principal->value
                                                              : std::string_view{},
                                                          durable_principal
                                                              ? durable_principal->lifetime
                                                              : ControlDurableClientLifetime::Broker)
                    : ControlClientResult{};
            if (!client.client) {
                (void)reply("enrollment-denied", "{}", "the broker rejected client identity");
                return;
            }
            std::weak_ptr<ConnectionState> weak_state = shared_state;
            auto session = service.open_session(
                *peer, client.client->client_id,
                [weak_state](const ControlProgressEnvelope& progress) {
                    const auto locked = weak_state.lock();
                    if (!locked || !locked->connection->is_connected())
                        return false;
                    const auto encoded =
                        encode_control_envelope(ControlEnvelope{.payload = progress});
                    return !encoded.empty() && locked->connection->send_message(encoded);
                },
                !durable_principal.has_value());
            if (!session.is_open()) {
                (void)management_broker->disconnect_client(client.client->client_id, *peer,
                                                           "enrollment-rollback");
                (void)reply("enrollment-denied", "{}", "the broker could not open a session");
                return;
            }
            const auto id = client.client->client_id.value;
            state.enrolled_peer = *peer;
            state.session.emplace(std::move(session));
            auto data = choc::value::createObject("");
            data.addMember("client_id", choc::value::createString(id));
            data.addMember("schema", choc::value::createString("pulp.control.enrollment.v1"));
            if (!reply("accepted", choc::json::toString(data, false)))
                close(state);
            return;
        }
        if (!state.session) {
            (void)reply("session-required", "{}", "management requires an enrolled client");
            return;
        }
        const auto current_client = management_broker->client(state.session->client_id());
        if (!current_client ||
            current_client->peer_fingerprint != state.session->peer().fingerprint()) {
            (void)reply("session-superseded", "{}",
                        "a newer authenticated process owns this durable client session");
            return;
        }
        if (request.command == "instances") {
            if (request.params_json != "{}") {
                (void)reply("invalid-request", "{}", "instances accepts no parameters");
                return;
            }
            auto instances = choc::value::createEmptyArray();
            for (const auto& registration : management_broker->registrations()) {
                auto item = choc::value::createObject("");
                item.addMember("artifact_digest",
                               choc::value::createString(registration.artifact_digest));
                auto capabilities = choc::value::createEmptyArray();
                for (const auto capability : registration.capabilities)
                    capabilities.addArrayElement(
                        choc::value::createString(capability_contract_id(capability)));
                item.addMember("capabilities", capabilities);
                item.addMember("instance_id", choc::value::createString(registration.instance_id));
                item.addMember("manifest_digest",
                               choc::value::createString(registration.manifest_digest));
                item.addMember("plugin_id", choc::value::createString(registration.plugin_id));
                item.addMember("profile",
                               choc::value::createString(control_profile_id(registration.profile)));
                item.addMember("publication_id",
                               choc::value::createString(registration.publication_id));
                item.addMember("registration_id",
                               choc::value::createString(registration.registration_id.value));
                item.addMember("session_id", choc::value::createString(registration.session_id));
                instances.addArrayElement(item);
            }
            auto data = choc::value::createObject("");
            data.addMember("instances", instances);
            data.addMember("schema", choc::value::createString("pulp.control.instances.v1"));
            (void)reply("completed", choc::json::toString(data, false));
            return;
        }
        choc::value::Value params;
        try {
            params = choc::json::parse(request.params_json);
        } catch (...) {
            (void)reply("invalid-request", "{}", "management parameters are invalid JSON");
            return;
        }
        if (!params.isObject()) {
            (void)reply("invalid-request", "{}", "management parameters must be an object");
            return;
        }
        if (request.command == "host-prepare") {
            if (!config.trusted_hosts.prepare) {
                (void)reply("unavailable", "{}", "trusted host preparation is unavailable");
                return;
            }
            const auto intent = trusted_host_intent_from_json(params);
            if (!intent) {
                (void)reply("invalid-request", "{}",
                            "host-prepare requires a bounded exact launch intent");
                return;
            }
            const auto prepared = config.trusted_hosts.prepare(*intent);
            auto data = choc::value::createObject("");
            if (prepared.ticket)
                data.addMember("inventory_id",
                               choc::value::createString(prepared.ticket->inventory_id));
            data.addMember("schema", choc::value::createString("pulp.control.host-prepare.v1"));
            (void)reply(std::string(control_trusted_host_inventory_status_id(prepared.status)),
                        choc::json::toString(data, false));
            return;
        }
        if (request.command == "host-prepare-installed") {
            if (!config.trusted_hosts.prepare_installed ||
                !has_only_members(params, {"host_id"}) ||
                !params.hasObjectMember("host_id") || !params["host_id"].isString()) {
                (void)reply("invalid-request", "{}",
                            "host-prepare-installed requires one broker-owned host_id");
                return;
            }
            const auto host_id = std::string(params["host_id"].getString());
            if (host_id.empty() || host_id.size() > 128) {
                (void)reply("invalid-request", "{}", "the installed host id is invalid");
                return;
            }
            const auto prepared = config.trusted_hosts.prepare_installed(host_id);
            auto data = choc::value::createObject("");
            data.addMember("host_id", choc::value::createString(host_id));
            if (prepared.ticket)
                data.addMember("inventory_id",
                               choc::value::createString(prepared.ticket->inventory_id));
            data.addMember("schema",
                           choc::value::createString(
                               "pulp.control.host-prepare-installed.v1"));
            (void)reply(std::string(control_trusted_host_inventory_status_id(prepared.status)),
                        choc::json::toString(data, false));
            return;
        }
        if (request.command == "host-launch") {
            if (!config.trusted_hosts.launch || !has_only_members(params, {"inventory_id"}) ||
                !params.hasObjectMember("inventory_id") ||
                !params["inventory_id"].isString()) {
                (void)reply("invalid-request", "{}",
                            "host-launch requires one broker inventory id");
                return;
            }
            const auto inventory_id = std::string(params["inventory_id"].getString());
            if (inventory_id.empty() || inventory_id.size() > 256) {
                (void)reply("invalid-request", "{}", "the inventory id is invalid");
                return;
            }
            const auto launched = config.trusted_hosts.launch(inventory_id);
            auto data = choc::value::createObject("");
            data.addMember("inventory_id", choc::value::createString(inventory_id));
            data.addMember("schema", choc::value::createString("pulp.control.host-launch.v1"));
            (void)reply(std::string(control_trusted_host_launch_status_id(launched.status)),
                        choc::json::toString(data, false), launched.explanation);
            return;
        }
        if (request.command == "revoke") {
            if (!params.hasObjectMember("grant_id") || !params["grant_id"].isString()) {
                (void)reply("invalid-request", "{}", "revoke requires grant_id");
                return;
            }
            const ControlGrantId grant_id{std::string(params["grant_id"].getString())};
            const auto grant = management_broker->grant(grant_id);
            if (!grant || grant->client_id != state.session->client_id()) {
                (void)reply("not-found", "{}", "grant is unavailable to this client");
                return;
            }
            const auto status = management_broker->revoke_grant(grant_id, "cli-revoke");
            auto data = choc::value::createObject("");
            data.addMember("grant_id", choc::value::createString(grant_id.value));
            data.addMember("schema", choc::value::createString("pulp.control.revoke.v1"));
            (void)reply(std::string(control_grant_status_id(status)),
                        choc::json::toString(data, false));
            return;
        }
        if (request.command == "grant-request") {
            if (!params.hasObjectMember("instance_id") || !params["instance_id"].isString() ||
                (params.hasObjectMember("profile") == params.hasObjectMember("operation_id")) ||
                (params.hasObjectMember("profile") && !params["profile"].isString()) ||
                (params.hasObjectMember("operation_id") && !params["operation_id"].isString())) {
                (void)reply("invalid-request", "{}",
                            "grant-request requires instance_id and exactly one of profile or operation_id");
                return;
            }
            const auto instance_id = std::string(params["instance_id"].getString());
            std::vector<ControlRegistration> matches;
            for (const auto& registration : management_broker->registrations())
                if (registration.instance_id == instance_id)
                    matches.push_back(registration);
            if (matches.size() != 1) {
                (void)reply(matches.empty() ? "not-found" : "ambiguous-instance", "{}",
                            matches.empty() ? "the exact instance is not live"
                                            : "the instance selector is ambiguous");
                return;
            }
            ControlGrantRequest grant_request{.client_id = state.session->client_id(),
                                              .registration_id = matches.front().registration_id};
            ControlGrantSelectorKind selector_kind = ControlGrantSelectorKind::Profile;
            std::string selector_id;
            if (params.hasObjectMember("operation_id")) {
                const auto operation_id = std::string(params["operation_id"].getString());
                selector_kind = ControlGrantSelectorKind::Operation;
                selector_id = operation_id;
                const auto* operation = resolve_control_operation(operation_id, 1);
                if (!operation || !capability_is_grantable(operation->capability)) {
                    (void)reply("invalid-request", "{}", "the grant operation is unknown or not grantable");
                    return;
                }
                if (std::ranges::find(matches.front().capabilities, operation->capability) !=
                    matches.front().capabilities.end())
                    grant_request.capabilities.push_back(operation->capability);
            } else {
                const auto profile_id_value = std::string(params["profile"].getString());
                selector_id = profile_id_value;
                const auto profile = profile_id_value == "inspect-readonly"
                                         ? std::optional{InspectorProfile::Observe}
                                         : profile_from_id(profile_id_value);
                if (!profile || *profile == InspectorProfile::Off ||
                    *profile == InspectorProfile::Custom) {
                    (void)reply("invalid-request", "{}", "the grant profile is unknown");
                    return;
                }
                for (const auto capability : profile_capabilities(*profile))
                    if (std::ranges::find(matches.front().capabilities, capability) !=
                        matches.front().capabilities.end())
                        grant_request.capabilities.push_back(capability);
            }
            if (grant_request.capabilities.empty()) {
                (void)reply("capability-unavailable", "{}",
                            "the instance exposes none of the requested profile");
                return;
            }
            const auto consent = config.decide_consent
                                     ? config.decide_consent(ControlGrantConsentRequest{
                                           .client_peer = state.session->peer().evidence(),
                                           .client_peer_fingerprint =
                                               std::string(state.session->peer().fingerprint()),
                                           .grant = grant_request,
                                           .registration = matches.front(),
                                           .selector_kind = selector_kind,
                                           .selector_id = std::move(selector_id),
                                       })
                                     : ControlConsentDecision{};
            const auto issued =
                management_broker->issue_grant(state.session->peer(), grant_request, consent);
            auto data = choc::value::createObject("");
            if (issued.grant) {
                data.addMember("grant_id", choc::value::createString(issued.grant->grant_id.value));
                data.addMember("instance_id", choc::value::createString(issued.grant->instance_id));
            }
            data.addMember("schema", choc::value::createString("pulp.control.grant.v1"));
            (void)reply(std::string(control_grant_status_id(issued.status)),
                        choc::json::toString(data, false),
                        issued.status == ControlGrantStatus::ConsentRequired
                            ? "broker-owned consent is required"
                            : std::string{});
            return;
        }
        (void)reply("invalid-request", "{}", "unsupported management command");
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
        } else if (const auto* ready = std::get_if<ControlHostReadyEnvelope>(&envelope->payload)) {
            handle_host_ready(state, *ready, shared_state);
        } else if (const auto* heartbeat =
                       std::get_if<ControlHostHeartbeatEnvelope>(&envelope->payload)) {
            handle_host_heartbeat(state, *heartbeat);
        } else if (const auto* management =
                       std::get_if<ControlManagementEnvelope>(&envelope->payload)) {
            handle_management(state, *management, shared_state);
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
        if (state->pending_host_registration && state->enrolled_peer && enrollment_context)
            (void)enrollment_context->broker.unregister_instance(
                state->pending_host_registration->registration_id, *state->enrolled_peer,
                "endpoint-unready-host-disconnected");
        state->host_registration.reset();
        state->pending_host_registration.reset();
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
                                 ControlEndpointEnrollmentContext* enrollment_context,
                                 ControlBroker* management_broker)
    : impl_(std::make_unique<Impl>(service, std::move(consume_admission), std::move(config),
                                   host_router, enrollment_context, management_broker)) {}

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
