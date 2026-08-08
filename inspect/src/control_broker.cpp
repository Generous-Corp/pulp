#include <pulp/inspect/control_broker.hpp>

#include "control_protocol_internal.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <ranges>
#include <utility>

namespace pulp::inspect {
namespace {

std::int64_t wall_clock_unix_ms(const ControlOperationStore::WallClock& clock) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock().time_since_epoch())
        .count();
}

bool receipt_matches_plan(const ControlOperationReceipt& receipt,
                          const ControlAdmissionPlan& plan) {
    return receipt.receipt_id == plan.receipt_id &&
           receipt.binding.authority_binding() == plan.authority_binding();
}

ControlArtifactLineage artifact_lineage_from_receipt(const ControlOperationReceipt& receipt) {
    const auto& authority = receipt.binding.authority_binding();
    return {
        .broker_id = authority.broker_id.value,
        .receipt_id = receipt.receipt_id.value,
        .producer_client_id = authority.client_id.value,
        .producer_registration_id = authority.registration_id.value,
        .session_id = authority.session_id,
        .instance_id = authority.instance_id,
        .publication_id = authority.publication_id,
        .producer_capability_id = std::string(capability_contract_id(authority.capability)),
        .producer_operation_id = authority.operation_id,
        .producer_operation_version = authority.operation_version,
        .original_grant_id = authority.grant_id.value,
        .consent_decision_id = authority.consent_decision_id,
        .manifest_digest = authority.manifest_digest,
        .producer_artifact_digest = authority.producer_artifact_digest,
    };
}

bool artifact_lineage_matches_receipt(const ControlArtifactLineage& lineage,
                                      const ControlOperationReceipt& receipt) {
    const auto expected = artifact_lineage_from_receipt(receipt);
    return lineage.broker_id == expected.broker_id && lineage.receipt_id == expected.receipt_id &&
           lineage.producer_client_id == expected.producer_client_id &&
           lineage.producer_registration_id == expected.producer_registration_id &&
           lineage.session_id == expected.session_id &&
           lineage.instance_id == expected.instance_id &&
           lineage.publication_id == expected.publication_id &&
           lineage.producer_capability_id == expected.producer_capability_id &&
           lineage.producer_operation_id == expected.producer_operation_id &&
           lineage.producer_operation_version == expected.producer_operation_version &&
           lineage.original_grant_id == expected.original_grant_id &&
           lineage.consent_decision_id == expected.consent_decision_id &&
           lineage.manifest_digest == expected.manifest_digest &&
           lineage.producer_artifact_digest == expected.producer_artifact_digest;
}

bool produced_artifact_result_matches(const ControlOperationDescriptor& operation,
                                      const ControlOperationResult& result,
                                      const ControlArtifactHandle& handle,
                                      const ControlArtifactMetadata& metadata) {
    const auto& binding = operation.artifact_binding;
    if (!binding.produced)
        return true;
    const auto detail = control_protocol_detail::parse_bounded_control_json(
        result.detail_json, kControlMaximumResultDetailBytes,
        control_protocol_detail::kMaximumResultJsonNodes);
    if (!detail || !detail->isObject())
        return false;
    const auto string_matches = [&](std::string_view field, std::string_view expected) {
        return !field.empty() && detail->hasObjectMember(field) && (*detail)[field].isString() &&
               (*detail)[field].getString() == expected;
    };
    if (!string_matches(binding.artifact_id_field, metadata.artifact_id) ||
        !string_matches(binding.sha256_field, metadata.sha256) ||
        handle.artifact_id != metadata.artifact_id || handle.media_type != metadata.content_type ||
        handle.byte_size != metadata.byte_size)
        return false;
    if (!binding.byte_count_field.empty()) {
        const auto count = (*detail)[binding.byte_count_field];
        if (!count.isInt() || count.getInt64() < 0 ||
            static_cast<std::uint64_t>(count.getInt64()) != metadata.byte_size)
            return false;
    }
    return binding.media_type_field.empty() ||
           string_matches(binding.media_type_field, metadata.content_type);
}

bool receipt_result_matches(const ControlOperationDescriptor& operation,
                            const ControlOperationResult& result,
                            std::string_view expected_receipt_id) {
    const auto& binding = operation.receipt_binding;
    if (!binding.bound)
        return true;
    const auto detail = control_protocol_detail::parse_bounded_control_json(
        result.detail_json, kControlMaximumResultDetailBytes,
        control_protocol_detail::kMaximumResultJsonNodes);
    return detail && detail->isObject() && !binding.receipt_id_field.empty() &&
           detail->hasObjectMember(binding.receipt_id_field) &&
           (*detail)[binding.receipt_id_field].isString() &&
           (*detail)[binding.receipt_id_field].getString() == expected_receipt_id;
}

} // namespace

ControlBroker::ControlBroker(ControlBrokerConfig config,
                             std::shared_ptr<ControlSecurityAuditLog> audit_log, Clock clock)
    : audit_log_(audit_log ? std::move(audit_log) : std::make_shared<ControlSecurityAuditLog>()),
      identities_(std::move(config.identities), audit_log_, clock),
      grants_(identities_, audit_log_, std::move(config.grants), std::move(clock)),
      admission_policy_(std::move(config.admission)),
      wall_clock_(config.wall_clock ? std::move(config.wall_clock)
                                    : ControlOperationStore::WallClock{[] {
                                          return std::chrono::system_clock::now();
                                      }}),
      operation_store_(config.operation_store ? std::make_unique<ControlOperationStore>(
                                                    std::move(*config.operation_store), wall_clock_)
                                              : nullptr),
      artifact_store_(config.artifact_store ? std::make_unique<ControlArtifactStore>(
                                                  std::move(*config.artifact_store), wall_clock_)
                                            : nullptr) {
    if (operation_store_)
        (void)operation_store_->open();
}

ControlBroker::~ControlBroker() = default;

const ControlBrokerId& ControlBroker::broker_id() const {
    return identities_.broker_id();
}

ControlBootstrapResult
ControlBroker::issue_bootstrap(const VerifiedControlPeerIdentity& client_peer) {
    std::lock_guard coordination_lock(coordination_mutex_);
    auto result = identities_.issue_bootstrap(client_peer);
    grants_.sweep_expired();
    return result;
}

ControlClientResult
ControlBroker::redeem_bootstrap(std::string_view ticket_id, std::span<const std::uint8_t> secret,
                                const VerifiedControlPeerIdentity& client_peer) {
    std::lock_guard coordination_lock(coordination_mutex_);
    auto result = identities_.redeem_bootstrap(ticket_id, secret, client_peer);
    grants_.sweep_expired();
    return result;
}

bool ControlBroker::refresh_client(const ControlClientId& client_id,
                                   const VerifiedControlPeerIdentity& client_peer) {
    std::lock_guard coordination_lock(coordination_mutex_);
    if (!owns_client_peer(client_id, client_peer)) {
        audit_denial("client.refresh", client_peer, "identity-mismatch", client_id);
        return false;
    }
    const bool refreshed = identities_.refresh_client(client_id, client_peer);
    grants_.sweep_expired();
    return refreshed;
}

ControlBrokerLifecycleResult
ControlBroker::disconnect_client(const ControlClientId& client_id,
                                 const VerifiedControlPeerIdentity& client_peer,
                                 std::string_view decision_id) {
    ControlBrokerLifecycleResult result;
    {
        std::lock_guard coordination_lock(coordination_mutex_);
        if (decision_id.empty()) {
            audit_denial("client.disconnect", client_peer, "invalid-request", client_id);
            return {};
        }
        if (!owns_client_peer(client_id, client_peer)) {
            audit_denial("client.disconnect", client_peer, "identity-mismatch", client_id);
            return {};
        }
        if (!identities_.disconnect_client(client_id))
            return {};
        result = {
            .identity_removed = true,
            .grants_revoked = grants_.revoke_client(client_id, decision_id),
        };
    }
    request_active_cancellation(client_id, {}, {}, "client-disconnected");
    return result;
}

ControlRegistrationResult
ControlBroker::register_instance(const VerifiedControlPeerIdentity& host_peer,
                                 ControlRegistrationRequest request) {
    std::lock_guard coordination_lock(coordination_mutex_);
    auto result = identities_.register_instance(host_peer, std::move(request));
    grants_.sweep_expired();
    return result;
}

bool ControlBroker::heartbeat(const ControlRegistrationId& registration_id,
                              const VerifiedControlPeerIdentity& host_peer) {
    std::lock_guard coordination_lock(coordination_mutex_);
    if (!owns_registration_peer(registration_id, host_peer)) {
        audit_denial("registration.heartbeat", host_peer, "identity-mismatch", {}, registration_id);
        return false;
    }
    const bool refreshed = identities_.heartbeat(registration_id, host_peer);
    grants_.sweep_expired();
    return refreshed;
}

ControlBrokerLifecycleResult
ControlBroker::unregister_instance(const ControlRegistrationId& registration_id,
                                   const VerifiedControlPeerIdentity& host_peer,
                                   std::string_view decision_id) {
    ControlBrokerLifecycleResult result;
    {
        std::lock_guard coordination_lock(coordination_mutex_);
        if (decision_id.empty()) {
            audit_denial("registration.unregister", host_peer, "invalid-request", {},
                         registration_id);
            return {};
        }
        if (!owns_registration_peer(registration_id, host_peer)) {
            audit_denial("registration.unregister", host_peer, "identity-mismatch", {},
                         registration_id);
            return {};
        }
        if (!identities_.unregister_instance(registration_id, host_peer))
            return {};
        result = {
            .identity_removed = true,
            .grants_revoked = grants_.revoke_registration(registration_id, decision_id),
        };
    }
    request_active_cancellation({}, registration_id, {}, "instance-unregistered");
    return result;
}

ControlGrantResult ControlBroker::issue_grant(const VerifiedControlPeerIdentity& client_peer,
                                              ControlGrantRequest request,
                                              ControlConsentDecision consent) {
    std::lock_guard coordination_lock(coordination_mutex_);
    grants_.sweep_expired();
    if (!owns_client_peer(request.client_id, client_peer)) {
        audit_denial("grant.issue", client_peer, "client-unavailable", request.client_id,
                     request.registration_id);
        return ControlGrantResult{.status = ControlGrantStatus::ClientUnavailable};
    }
    return grants_.issue(std::move(request), std::move(consent));
}

ControlGrantStatus ControlBroker::revoke_grant(const ControlGrantId& grant_id,
                                               std::string_view decision_id) {
    ControlGrantStatus status;
    {
        std::lock_guard coordination_lock(coordination_mutex_);
        status = grants_.revoke(grant_id, decision_id);
    }
    if (status == ControlGrantStatus::Revoked) {
        request_active_cancellation({}, {}, grant_id, "grant-revoked");
    }
    return status;
}

bool ControlBroker::is_granted(const ControlGrantId& grant_id, const ControlClientId& client_id,
                               const ControlRegistrationId& registration_id,
                               InspectorCapability capability) {
    std::lock_guard coordination_lock(coordination_mutex_);
    return grants_.is_granted(grant_id, client_id, registration_id, capability);
}

ControlAdmissionResult
ControlBroker::admit_operation(const VerifiedControlPeerIdentity& client_peer,
                               const ControlRequestEnvelope& envelope) {
    const auto request = control_admission_request(envelope);
    if (!request) {
        ControlAdmissionResult result;
        audit_denial("operation.admit", client_peer, "invalid-request",
                     ControlClientId{envelope.client_id},
                     ControlRegistrationId{envelope.registration_id});
        return result;
    }
    return admit_verified_operation(client_peer, *request);
}

ControlAdmissionResult
ControlBroker::replay_operation(const VerifiedControlPeerIdentity& client_peer,
                                const ControlRequestEnvelope& envelope) {
    const auto request = control_admission_request(envelope);
    if (!request)
        return {};
    return admit_verified_operation(client_peer, *request, false);
}

ControlAdmissionResult
ControlBroker::admit_verified_operation(const VerifiedControlPeerIdentity& client_peer,
                                        const ControlAdmissionRequest& request, bool allow_new) {
    std::unique_lock coordination_lock(coordination_mutex_);
    ControlAdmissionResult result;
    if (!request.client_id || !request.registration_id || !request.grant_id ||
        request.request_id.empty() || request.operation_id.empty() ||
        request.idempotency_key.empty() || request.canonical_request_hash.size() != 64 ||
        request.instance_generation.empty()) {
        audit_denial("operation.admit", client_peer, "invalid-request", request.client_id,
                     request.registration_id);
        return result;
    }
    if (!operation_store_ || !operation_store_->is_open()) {
        result.status = ControlAdmissionStatus::DurableStoreUnavailable;
        audit_denial("operation.admit", client_peer, "durable-store-unavailable", request.client_id,
                     request.registration_id);
        return result;
    }
    if (request.deadline_unix_ms <= wall_clock_unix_ms(wall_clock_)) {
        result.status = ControlAdmissionStatus::DeadlineExceeded;
        audit_denial("operation.admit", client_peer, "deadline-exceeded", request.client_id,
                     request.registration_id);
        return result;
    }

    const auto* operation =
        resolve_control_operation(request.operation_id, request.operation_version);
    if (!operation) {
        result.status = ControlAdmissionStatus::UnknownOperation;
        audit_denial("operation.admit", client_peer, "unknown-operation", request.client_id,
                     request.registration_id);
        return result;
    }
    if (operation->capability == InspectorCapability::ArtifactRead) {
        result.status = ControlAdmissionStatus::ArtifactLineageRequired;
        audit_denial("operation.admit", client_peer, "artifact-lineage-required", request.client_id,
                     request.registration_id);
        return result;
    }
    ControlJsonSchemaDiagnostics schema_diagnostics;
    if (!validate_control_json_schema(request.params_json, operation->input_schema_json,
                                      &schema_diagnostics)) {
        result.status = ControlAdmissionStatus::InvalidRequest;
        audit_denial("operation.admit", client_peer, "invalid-params", request.client_id,
                     request.registration_id);
        return result;
    }

    const auto client = identities_.client(request.client_id);
    const auto registration = identities_.registration(request.registration_id);
    if (!client || !registration || client->peer_fingerprint != client_peer.fingerprint() ||
        request.instance_generation != registration->publication_id) {
        result.status = ControlAdmissionStatus::IdentityMismatch;
        audit_denial("operation.admit", client_peer, "identity-mismatch", request.client_id,
                     request.registration_id);
        return result;
    }

    const bool built =
        std::find(registration->capabilities.begin(), registration->capabilities.end(),
                  operation->capability) != registration->capabilities.end();
    const bool host_available = admission_policy_.host_available &&
                                admission_policy_.host_available(*registration, *operation);
    const bool activated =
        admission_policy_.activated && admission_policy_.activated(*registration, *operation);
    const bool policy_eligible = admission_policy_.policy_eligible &&
                                 admission_policy_.policy_eligible(*registration, *operation);
    const bool client_granted = grants_.is_granted(request.grant_id, request.client_id,
                                                   request.registration_id, operation->capability);
    const auto grant = grants_.grant(request.grant_id);
    result.permission = evaluate_control_permission({
        .implemented = true,
        .built = built,
        .host_available = host_available,
        .activated = activated,
        .policy_eligible = policy_eligible,
        .client_granted = client_granted,
        .session_live = true,
    });
    if (!result.permission.allowed) {
        result.status = ControlAdmissionStatus::PermissionDenied;
        audit_denial("operation.admit", client_peer,
                     result.permission.denial ? control_denial_reason_id(*result.permission.denial)
                                              : std::string_view{"permission-denied"},
                     request.client_id, request.registration_id);
        return result;
    }

    if (!grant) {
        result.status = ControlAdmissionStatus::PermissionDenied;
        result.permission = evaluate_control_permission({
            .implemented = true,
            .built = built,
            .host_available = host_available,
            .activated = activated,
            .policy_eligible = policy_eligible,
            .client_granted = false,
            .session_live = true,
        });
        audit_denial("operation.admit", client_peer, "grant-required", request.client_id,
                     request.registration_id);
        return result;
    }

    ControlAuthorityBinding authority{
        .broker_id = identities_.broker_id(),
        .client_principal = client->peer_fingerprint,
        .client_id = request.client_id,
        .registration_id = request.registration_id,
        .grant_id = request.grant_id,
        .session_id = registration->session_id,
        .instance_id = registration->instance_id,
        .publication_id = registration->publication_id,
        .instance_generation = request.instance_generation,
        .capability = operation->capability,
        .operation_id = request.operation_id,
        .operation_version = request.operation_version,
        .consent_decision_id = grant->consent_decision_id,
        .manifest_digest = registration->manifest_digest,
        .producer_artifact_digest = registration->artifact_digest,
        .deadline_unix_ms = request.deadline_unix_ms,
        .expected_state_generation = request.expected_state_generation,
    };
    result.plan.emplace();
    static_cast<ControlAuthorityBinding&>(*result.plan) = authority;

    ControlOperationBinding binding;
    static_cast<ControlAuthorityBinding&>(binding) = authority;
    binding.request_id = request.request_id;
    binding.idempotency_key = request.idempotency_key;
    binding.canonical_request_hash = request.canonical_request_hash;
    coordination_lock.unlock();

    const auto stored = allow_new ? operation_store_->admit(std::move(binding))
                                  : operation_store_->replay(std::move(binding));
    result.receipt = stored.receipt;
    if (stored.status == ControlOperationStoreStatus::IdempotencyConflict) {
        result.status = ControlAdmissionStatus::IdempotencyConflict;
        result.plan.reset();
        return result;
    }
    if (stored.status == ControlOperationStoreStatus::RequestIdConflict) {
        result.status = ControlAdmissionStatus::RequestIdConflict;
        result.plan.reset();
        return result;
    }
    if (stored.status == ControlOperationStoreStatus::ReplayWindowExpired) {
        result.status = ControlAdmissionStatus::ReplayWindowExpired;
        result.plan.reset();
        return result;
    }
    if (stored.status == ControlOperationStoreStatus::ResourceExhausted) {
        result.status = ControlAdmissionStatus::ResourceExhausted;
        result.plan.reset();
        return result;
    }
    if ((stored.status != ControlOperationStoreStatus::Admitted &&
         stored.status != ControlOperationStoreStatus::Replay) ||
        !stored.receipt) {
        result.status = ControlAdmissionStatus::DurableStoreUnavailable;
        result.plan.reset();
        return result;
    }
    result.plan->receipt_id = stored.receipt->receipt_id;
    static_cast<ControlAuthorityBinding&>(*result.plan) =
        stored.receipt->binding.authority_binding();
    result.status = ControlAdmissionStatus::Admitted;
    result.should_dispatch = stored.status == ControlOperationStoreStatus::Admitted;

    if (result.should_dispatch && !revalidate_operation(client_peer, *result.plan)) {
        ControlOperationResult cancelled;
        cancelled.result_code = ControlResultCode::Cancelled;
        cancelled.explanation = "authority changed before dispatch";
        cancelled.cancellation_reason = "admission-revalidation";
        const auto transitioned =
            operation_store_->transition(result.plan->receipt_id, ControlReceiptState::Admitted,
                                         ControlReceiptState::Cancelled, std::move(cancelled));
        result.status = ControlAdmissionStatus::CancelledBeforeDispatch;
        result.receipt = transitioned.receipt;
        result.plan.reset();
        result.should_dispatch = false;
    }
    return result;
}

bool ControlBroker::revalidate_operation(const VerifiedControlPeerIdentity& client_peer,
                                         const ControlAdmissionPlan& plan) {
    std::lock_guard coordination_lock(coordination_mutex_);
    return revalidate_operation_locked(client_peer, plan);
}

bool ControlBroker::revalidate_operation_locked(const VerifiedControlPeerIdentity& client_peer,
                                                const ControlAdmissionPlan& plan) {
    if (plan.broker_id != identities_.broker_id() ||
        plan.client_principal != client_peer.fingerprint()) {
        return false;
    }
    const auto client = identities_.client(plan.client_id);
    const auto registration = identities_.registration(plan.registration_id);
    if (!client || !registration || client->peer_fingerprint != plan.client_principal ||
        registration->session_id != plan.session_id ||
        registration->instance_id != plan.instance_id ||
        registration->publication_id != plan.publication_id ||
        plan.instance_generation != registration->publication_id ||
        registration->manifest_digest != plan.manifest_digest) {
        return false;
    }
    const auto* operation = resolve_control_operation(plan.operation_id, plan.operation_version);
    if (!operation || operation->capability != plan.capability ||
        plan.capability == InspectorCapability::ArtifactRead) {
        return false;
    }
    if (!admission_policy_.host_available ||
        !admission_policy_.host_available(*registration, *operation) ||
        !admission_policy_.activated || !admission_policy_.activated(*registration, *operation) ||
        !admission_policy_.policy_eligible ||
        !admission_policy_.policy_eligible(*registration, *operation)) {
        return false;
    }
    return grants_.is_granted(plan.grant_id, plan.client_id, plan.registration_id, plan.capability);
}

bool ControlBroker::operation_store_ready() const {
    return operation_store_ && operation_store_->is_open();
}

bool ControlBroker::artifact_store_ready() const {
    return artifact_store_ && artifact_store_->is_ready();
}

ControlOperationStoreResult
ControlBroker::begin_operation(const VerifiedControlPeerIdentity& client_peer,
                               const ControlAdmissionPlan& plan) {
    if (!operation_store_ || !operation_store_->is_open() || !plan.receipt_id) {
        return {
            .status = ControlOperationStoreStatus::StoreUnavailable,
            .error = "durable operation store is unavailable",
        };
    }
    std::lock_guard coordination_lock(coordination_mutex_);
    const auto current = operation_store_->receipt(plan.receipt_id);
    if (!current || current->state != ControlReceiptState::Admitted ||
        !receipt_matches_plan(*current, plan)) {
        return {
            .status = ControlOperationStoreStatus::InvalidRequest,
            .error = "admission plan does not match the durable receipt",
        };
    }
    if (!revalidate_operation_locked(client_peer, plan)) {
        ControlOperationResult cancelled;
        cancelled.result_code = ControlResultCode::Cancelled;
        cancelled.explanation = "authority changed before execution";
        cancelled.cancellation_reason = "executor-revalidation";
        return operation_store_->transition(plan.receipt_id, ControlReceiptState::Admitted,
                                            ControlReceiptState::Cancelled, std::move(cancelled));
    }
    return operation_store_->begin(plan.receipt_id);
}

ControlOperationStoreResult
ControlBroker::finish_operation(const VerifiedControlPeerIdentity& client_peer,
                                const ControlAdmissionPlan& plan,
                                ControlReceiptState terminal_state, ControlOperationResult result) {
    if (!operation_store_ || !operation_store_->is_open() || !plan.receipt_id) {
        return {
            .status = ControlOperationStoreStatus::StoreUnavailable,
            .error = "durable operation store is unavailable",
        };
    }
    if (!control_receipt_state_is_terminal(terminal_state)) {
        return {
            .status = ControlOperationStoreStatus::InvalidRequest,
            .error = "finish requires a live terminal state",
        };
    }
    std::lock_guard coordination_lock(coordination_mutex_);
    const auto current = operation_store_->receipt(plan.receipt_id);
    if (!current || current->state != ControlReceiptState::Running ||
        !receipt_matches_plan(*current, plan) ||
        current->binding.client_principal != client_peer.fingerprint()) {
        return {
            .status = ControlOperationStoreStatus::InvalidRequest,
            .error = "admission plan does not match the running receipt",
        };
    }
    const bool success_like = terminal_state == ControlReceiptState::Completed ||
                              terminal_state == ControlReceiptState::CompletedAfterRevocation;
    const bool authority_live = success_like && revalidate_operation_locked(client_peer, plan);
    if (success_like) {
        const auto* operation = resolve_control_operation(current->binding.operation_id,
                                                          current->binding.operation_version);
        ControlJsonSchemaDiagnostics diagnostics;
        if (!operation || !validate_control_output_json_schema(
                              result.detail_json, operation->output_schema_json, &diagnostics)) {
            return {
                .status = ControlOperationStoreStatus::InvalidRequest,
                .receipt = current,
                .error = "operation result does not satisfy its output schema",
            };
        }
        if (!receipt_result_matches(*operation, result, plan.receipt_id.value)) {
            return {
                .status = ControlOperationStoreStatus::InvalidRequest,
                .receipt = current,
                .error = "operation result receipt id does not match the durable receipt",
            };
        }
    }
    const auto* operation = success_like
                                ? resolve_control_operation(current->binding.operation_id,
                                                            current->binding.operation_version)
                                : nullptr;
    if (!result.artifacts.empty() &&
        (!operation || !operation->artifact_binding.produced)) {
        return {
            .status = ControlOperationStoreStatus::InvalidRequest,
            .error = "only successful artifact-producing operations may retain artifacts",
        };
    }
    if (operation && operation->artifact_binding.produced) {
        if (!artifact_store_) {
            return {
                .status = ControlOperationStoreStatus::StoreUnavailable,
                .error = "artifact store is unavailable",
            };
        }
        if (operation && operation->artifact_binding.produced && result.artifacts.size() != 1) {
            return {
                .status = ControlOperationStoreStatus::InvalidRequest,
                .error = "artifact result must bind exactly one durable artifact",
            };
        }
        for (const auto& handle : result.artifacts) {
            const auto metadata = artifact_store_->metadata(handle.artifact_id);
            if (!metadata || metadata->content_type != handle.media_type ||
                metadata->byte_size != handle.byte_size ||
                !artifact_lineage_matches_receipt(metadata->lineage, *current) ||
                (operation &&
                 !produced_artifact_result_matches(*operation, result, handle, *metadata))) {
                return {
                    .status = ControlOperationStoreStatus::InvalidRequest,
                    .error = "artifact was not durably published for this receipt",
                };
            }
        }
    }
    if (success_like) {
        if (authority_live) {
            terminal_state = ControlReceiptState::Completed;
            result.result_code.reset();
            result.retry = ControlRetryClassification::Never;
            if (current->cancellation_requested) {
                result.cancellation_reason = current->cancellation_reason;
                if (result.explanation.empty())
                    result.explanation = "operation completed after cancellation was requested";
            }
        } else {
            terminal_state = ControlReceiptState::CompletedAfterRevocation;
            result.result_code = ControlResultCode::CompletedAfterRevocation;
            result.retry = ControlRetryClassification::Never;
            if (current->cancellation_requested) {
                result.cancellation_reason = current->cancellation_reason;
                if (result.explanation.empty())
                    result.explanation = "operation completed after cancellation was requested";
            } else if (result.explanation.empty()) {
                result.explanation = "operation completed after revocation";
            }
        }
    } else if (terminal_state == ControlReceiptState::Cancelled && !result.result_code) {
        result.result_code = ControlResultCode::Cancelled;
    } else if (terminal_state == ControlReceiptState::UnknownNeedsRefresh && !result.result_code) {
        result.result_code = ControlResultCode::UnknownNeedsRefresh;
        result.retry = ControlRetryClassification::AfterRefresh;
    }
    return operation_store_->transition(plan.receipt_id, ControlReceiptState::Running,
                                        terminal_state, std::move(result));
}

ControlOperationStoreResult
ControlBroker::cancel_operation(const VerifiedControlPeerIdentity& client_peer,
                                const ControlClientId& client_id, std::string_view request_id,
                                std::string_view reason) {
    if (!operation_store_ || !operation_store_->is_open() || !client_id || request_id.empty()) {
        return {
            .status = ControlOperationStoreStatus::InvalidRequest,
            .error = "cancellation request is incomplete",
        };
    }
    {
        std::lock_guard coordination_lock(coordination_mutex_);
        if (!owns_client_peer(client_id, client_peer)) {
            return {
                .status = ControlOperationStoreStatus::InvalidRequest,
                .error = "cancellation peer does not own the client identity",
            };
        }
    }
    std::optional<ControlReceiptId> match;
    for (const auto& receipt : operation_store_->receipts()) {
        if (receipt.binding.client_id != client_id || receipt.binding.request_id != request_id)
            continue;
        if (match && *match != receipt.receipt_id) {
            return {
                .status = ControlOperationStoreStatus::InvalidRequest,
                .error = "request id is ambiguous for this client",
            };
        }
        match = receipt.receipt_id;
    }
    if (!match) {
        return {
            .status = ControlOperationStoreStatus::NotFound,
            .error = "operation request was not found",
        };
    }
    return operation_store_->request_cancellation(*match, std::string(reason));
}

bool ControlBroker::operation_cancellation_requested(const VerifiedControlPeerIdentity& client_peer,
                                                     const ControlAdmissionPlan& plan) const {
    if (!operation_store_ || !operation_store_->is_open() || !plan.receipt_id)
        return false;
    const auto receipt = operation_store_->receipt(plan.receipt_id);
    return receipt && receipt->state == ControlReceiptState::Running &&
           receipt->cancellation_requested &&
           receipt->binding.client_principal == client_peer.fingerprint() &&
           receipt_matches_plan(*receipt, plan);
}

ControlExecutionCheckpoint
ControlBroker::execution_checkpoint(const VerifiedControlPeerIdentity& client_peer,
                                    const ControlAdmissionPlan& plan) {
    if (!operation_store_ || !operation_store_->is_open() || !plan.receipt_id)
        return ControlExecutionCheckpoint::AuthorityRevoked;
    const auto receipt = operation_store_->receipt(plan.receipt_id);
    if (!receipt || receipt->state != ControlReceiptState::Running ||
        !receipt_matches_plan(*receipt, plan) ||
        receipt->binding.client_principal != client_peer.fingerprint()) {
        return ControlExecutionCheckpoint::AuthorityRevoked;
    }
    if (receipt->cancellation_requested)
        return ControlExecutionCheckpoint::Cancelled;
    if (plan.deadline_unix_ms <= wall_clock_unix_ms(wall_clock_))
        return ControlExecutionCheckpoint::DeadlineExceeded;
    return revalidate_operation(client_peer, plan) ? ControlExecutionCheckpoint::Continue
                                                   : ControlExecutionCheckpoint::AuthorityRevoked;
}

std::optional<ControlOperationReceipt>
ControlBroker::operation_receipt(const ControlReceiptId& receipt_id) const {
    if (!operation_store_ || !operation_store_->is_open())
        return std::nullopt;
    return operation_store_->receipt(receipt_id);
}

ControlArtifactStoreResult ControlBroker::store_operation_artifact(
    const VerifiedControlPeerIdentity& client_peer, const ControlAdmissionPlan& plan,
    std::span<const std::uint8_t> bytes, ControlArtifactProperties properties) {
    if (!artifact_store_ || !operation_store_ || !operation_store_->is_open() || !plan.receipt_id) {
        return {.status = ControlArtifactStatus::IoError};
    }
    std::lock_guard coordination_lock(coordination_mutex_);
    const auto receipt = operation_store_->receipt(plan.receipt_id);
    if (!receipt || receipt->state != ControlReceiptState::Running ||
        receipt->cancellation_requested || !receipt_matches_plan(*receipt, plan) ||
        receipt->binding.client_principal != client_peer.fingerprint() ||
        !revalidate_operation_locked(client_peer, plan)) {
        return {.status = ControlArtifactStatus::Unauthorized};
    }
    return artifact_store_->store(bytes, artifact_lineage_from_receipt(*receipt),
                                  std::move(properties));
}

ControlArtifactStoreResult ControlBroker::store_operation_artifact(
    const VerifiedControlPeerIdentity& client_peer, const ControlAdmissionPlan& plan,
    std::span<const std::uint8_t> bytes, std::string content_type,
    ControlArtifactSensitivity sensitivity, ControlArtifactRedactionState redaction_state,
    std::chrono::milliseconds lifetime) {
    const auto now_count = std::chrono::duration_cast<std::chrono::milliseconds>(
                               wall_clock_().time_since_epoch())
                               .count();
    if (now_count <= 0 || lifetime.count() <= 0 ||
        static_cast<std::uint64_t>(lifetime.count()) >
            std::numeric_limits<std::uint64_t>::max() -
                static_cast<std::uint64_t>(now_count)) {
        return {.status = ControlArtifactStatus::InvalidRequest};
    }
    const auto created_at = static_cast<std::uint64_t>(now_count);
    return store_operation_artifact(
        client_peer, plan, bytes,
        {.content_type = std::move(content_type),
         .created_at_unix_ms = created_at,
         .expires_at_unix_ms = created_at + static_cast<std::uint64_t>(lifetime.count()),
         .sensitivity = sensitivity,
         .redaction_state = redaction_state});
}

std::size_t ControlBroker::artifact_maximum_blob_bytes() const noexcept {
    return artifact_store_ ? artifact_store_->maximum_blob_bytes() : 0;
}

ControlArtifactReadResult ControlBroker::read_artifact(
    const VerifiedControlPeerIdentity& client_peer, const ControlClientId& requesting_client_id,
    std::string_view artifact_id, std::uint64_t offset, std::size_t maximum_bytes) {
    if (!artifact_store_)
        return {.status = ControlArtifactStatus::IoError};

    // Metadata and blob reads are owner-private but may block on storage and
    // hash the complete blob. Keep both outside the broker coordination lock.
    // The immutable metadata value is the authorization snapshot shared with
    // read_authorized(), which rejects any metadata or blob substitution.
    const auto metadata = artifact_store_->metadata(artifact_id);
    if (!metadata)
        return {.status = ControlArtifactStatus::NotFound};
    {
        std::lock_guard coordination_lock(coordination_mutex_);
        if (!authorize_artifact_read_locked(client_peer, requesting_client_id, *metadata))
            return {.status = ControlArtifactStatus::Unauthorized};
    }

    auto result = artifact_store_->read_authorized(artifact_id, offset, maximum_bytes, *metadata);
    if (result.status != ControlArtifactStatus::Read || !result.metadata)
        return result;

    // The second authorization is the read's linearization point. A completed
    // revocation/identity mutation wins the lock first and clears the result;
    // otherwise this read is ordered before the later mutation. Never expose
    // bytes obtained between the two checks when authority changed.
    {
        std::lock_guard coordination_lock(coordination_mutex_);
        if (!authorize_artifact_read_locked(client_peer, requesting_client_id, *result.metadata)) {
            return {.status = ControlArtifactStatus::Unauthorized};
        }
    }
    return result;
}

bool ControlBroker::authorize_artifact_read_locked(const VerifiedControlPeerIdentity& client_peer,
                                                   const ControlClientId& requesting_client_id,
                                                   const ControlArtifactMetadata& metadata) {
    const auto& lineage = metadata.lineage;
    const auto receipt = operation_store_
                             ? operation_store_->receipt(ControlReceiptId{lineage.receipt_id})
                             : std::nullopt;
    if (!receipt)
        return false;
    const auto& authority = receipt->binding.authority_binding();
    const auto client = identities_.client(authority.client_id);
    const auto registration = identities_.registration(authority.registration_id);
    const auto grant = grants_.grant(authority.grant_id);
    const auto* operation =
        resolve_control_operation(authority.operation_id, authority.operation_version);

    const bool valid =
        artifact_lineage_matches_receipt(lineage, *receipt) &&
        authority.broker_id == identities_.broker_id() &&
        authority.client_id == requesting_client_id &&
        metadata.expires_at_unix_ms > static_cast<std::uint64_t>(wall_clock_unix_ms(wall_clock_)) &&
        (receipt->state == ControlReceiptState::Completed ||
         receipt->state == ControlReceiptState::CompletedAfterRevocation) &&
        client && registration && grant && operation && operation->artifact_binding.produced &&
        operation->capability == authority.capability &&
        client->peer_fingerprint == client_peer.fingerprint() &&
        client->peer_fingerprint == authority.client_principal &&
        registration->session_id == authority.session_id &&
        registration->instance_id == authority.instance_id &&
        registration->publication_id == authority.publication_id &&
        authority.instance_generation == registration->publication_id &&
        registration->manifest_digest == authority.manifest_digest &&
        registration->artifact_digest == authority.producer_artifact_digest &&
        grant->consent_decision_id == authority.consent_decision_id &&
        std::ranges::any_of(receipt->result.artifacts,
                            [&](const auto& artifact) {
                                return artifact.artifact_id == metadata.artifact_id &&
                                       artifact.media_type == metadata.content_type &&
                                       artifact.byte_size == metadata.byte_size &&
                                       receipt->result.artifacts.size() == 1 &&
                                       produced_artifact_result_matches(
                                           *operation, receipt->result, artifact, metadata);
                            }) &&
        grants_.is_granted(authority.grant_id, authority.client_id, authority.registration_id,
                           authority.capability);
    if (!valid) {
        audit_denial("artifact.read", client_peer, "producer-lineage-unavailable",
                     requesting_client_id, authority.registration_id);
        return false;
    }
    return true;
}

std::optional<ControlClientIdentity> ControlBroker::client(const ControlClientId& client_id) const {
    return identities_.client(client_id);
}

std::optional<ControlRegistration>
ControlBroker::registration(const ControlRegistrationId& registration_id) const {
    return identities_.registration(registration_id);
}

std::vector<ControlRegistration> ControlBroker::registrations() const {
    std::lock_guard lock(coordination_mutex_);
    return identities_.registrations();
}

std::optional<ControlGrant> ControlBroker::grant(const ControlGrantId& grant_id) {
    return grants_.grant(grant_id);
}

void ControlBroker::sweep_expired() {
    std::lock_guard coordination_lock(coordination_mutex_);
    identities_.sweep_expired();
    grants_.sweep_expired();
    if (!operation_store_ || !operation_store_->is_open())
        return;
    const auto now = wall_clock_unix_ms(wall_clock_);
    for (const auto& receipt : operation_store_->receipts()) {
        if (control_receipt_state_is_terminal(receipt.state))
            continue;
        const auto& binding = receipt.binding;
        const auto client = identities_.client(binding.client_id);
        const auto registration = identities_.registration(binding.registration_id);
        const auto* operation =
            resolve_control_operation(binding.operation_id, binding.operation_version);
        const bool authority_live =
            client && registration && operation &&
            client->peer_fingerprint == binding.client_principal &&
            registration->session_id == binding.session_id &&
            registration->instance_id == binding.instance_id &&
            registration->publication_id == binding.publication_id &&
            registration->manifest_digest == binding.manifest_digest &&
            grants_.is_granted(binding.grant_id, binding.client_id, binding.registration_id,
                               operation->capability);
        if (!authority_live || binding.deadline_unix_ms <= now) {
            (void)operation_store_->request_cancellation(
                receipt.receipt_id,
                authority_live ? "operation-deadline-expired" : "operation-authority-expired");
        }
    }
}

void ControlBroker::audit_denial(std::string_view action,
                                 const VerifiedControlPeerIdentity& observed_peer,
                                 std::string_view reason, const ControlClientId& client_id,
                                 const ControlRegistrationId& registration_id) {
    audit_log_->append(ControlSecurityAuditEntry{
        .action = std::string(action),
        .broker_id = identities_.broker_id().value,
        .peer_fingerprint = std::string(observed_peer.fingerprint()),
        .client_id = client_id.value,
        .registration_id = registration_id.value,
        .outcome = ControlSecurityOutcome::Denied,
        .reason = std::string(reason),
    });
}

bool ControlBroker::owns_client_peer(const ControlClientId& client_id,
                                     const VerifiedControlPeerIdentity& client_peer) const {
    const auto identity = identities_.client(client_id);
    return identity && identity->peer_fingerprint == client_peer.fingerprint();
}

bool ControlBroker::owns_registration_peer(const ControlRegistrationId& registration_id,
                                           const VerifiedControlPeerIdentity& host_peer) const {
    const auto identity = identities_.registration(registration_id);
    return identity && identity->peer_fingerprint == host_peer.fingerprint();
}

void ControlBroker::request_active_cancellation(const ControlClientId& client_id,
                                                const ControlRegistrationId& registration_id,
                                                const ControlGrantId& grant_id,
                                                std::string_view reason) {
    if (!operation_store_ || !operation_store_->is_open())
        return;
    for (const auto& receipt : operation_store_->receipts()) {
        if ((client_id && receipt.binding.client_id != client_id) ||
            (registration_id && receipt.binding.registration_id != registration_id) ||
            (grant_id && receipt.binding.grant_id != grant_id) ||
            control_receipt_state_is_terminal(receipt.state)) {
            continue;
        }
        (void)operation_store_->request_cancellation(receipt.receipt_id, std::string(reason));
    }
}

} // namespace pulp::inspect
