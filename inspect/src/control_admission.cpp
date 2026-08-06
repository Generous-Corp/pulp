#include <pulp/inspect/control_admission.hpp>

namespace pulp::inspect {

std::optional<ControlAdmissionRequest>
control_admission_request(const ControlRequestEnvelope& envelope) {
    const auto expected_hash = control_request_hash(envelope);
    if (!expected_hash || envelope.request_hash != *expected_hash || envelope.client_id.empty() ||
        envelope.registration_id.empty() || envelope.grant_id.empty() ||
        envelope.instance_generation.empty() || envelope.operation_id.empty()) {
        return std::nullopt;
    }
    return ControlAdmissionRequest{
        .client_id = ControlClientId{envelope.client_id},
        .registration_id = ControlRegistrationId{envelope.registration_id},
        .grant_id = ControlGrantId{envelope.grant_id},
        .request_id = envelope.request_id,
        .instance_generation = envelope.instance_generation,
        .operation_id = envelope.operation_id,
        .operation_version = envelope.operation_version,
        .idempotency_key = envelope.idempotency_key,
        .canonical_request_hash = envelope.request_hash,
        .deadline_unix_ms = envelope.deadline_unix_ms,
        .expected_state_generation = envelope.expected_state_generation,
        .params_json = envelope.params_json,
    };
}

std::string_view control_admission_status_id(ControlAdmissionStatus status) {
    switch (status) {
    case ControlAdmissionStatus::Admitted:
        return "admitted";
    case ControlAdmissionStatus::InvalidRequest:
        return "invalid-request";
    case ControlAdmissionStatus::UnknownOperation:
        return "unknown-operation";
    case ControlAdmissionStatus::IdentityMismatch:
        return "identity-mismatch";
    case ControlAdmissionStatus::ArtifactLineageRequired:
        return "artifact-lineage-required";
    case ControlAdmissionStatus::PermissionDenied:
        return "permission-denied";
    case ControlAdmissionStatus::DurableStoreUnavailable:
        return "durable-store-unavailable";
    case ControlAdmissionStatus::IdempotencyConflict:
        return "idempotency-conflict";
    case ControlAdmissionStatus::RequestIdConflict:
        return "request-id-conflict";
    case ControlAdmissionStatus::ReplayWindowExpired:
        return "replay-window-expired";
    case ControlAdmissionStatus::ResourceExhausted:
        return "resource-exhausted";
    case ControlAdmissionStatus::DeadlineExceeded:
        return "deadline-exceeded";
    case ControlAdmissionStatus::CancelledBeforeDispatch:
        return "cancelled-before-dispatch";
    }
    return "invalid-request";
}

} // namespace pulp::inspect
