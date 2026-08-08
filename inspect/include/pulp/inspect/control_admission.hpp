#pragma once

#include <pulp/inspect/control_grants.hpp>
#include <pulp/inspect/control_operations.hpp>
#include <pulp/inspect/control_protocol.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::inspect {

enum class ControlAdmissionStatus : std::uint8_t {
    Admitted,
    InvalidRequest,
    UnknownOperation,
    IdentityMismatch,
    ArtifactLineageRequired,
    PermissionDenied,
    DurableStoreUnavailable,
    IdempotencyConflict,
    RequestIdConflict,
    ReplayWindowExpired,
    ResourceExhausted,
    DeadlineExceeded,
    CancelledBeforeDispatch,
};

enum class ControlExecutionCheckpoint : std::uint8_t {
    Continue,
    Cancelled,
    DeadlineExceeded,
    AuthorityRevoked,
};

std::string_view control_admission_status_id(ControlAdmissionStatus status);

struct ControlAdmissionRequest {
    ControlClientId client_id;
    ControlRegistrationId registration_id;
    ControlGrantId grant_id;
    std::string request_id;
    std::string instance_generation;
    std::string operation_id;
    std::uint32_t operation_version = 1;
    std::string idempotency_key;
    std::string canonical_request_hash;
    std::int64_t deadline_unix_ms = 0;
    std::uint64_t expected_state_generation = 0;
    std::string params_json = "{}";
};

/// Immutable authority snapshot minted by ControlBroker after all seven
/// permission terms pass. Executors must present it for revalidation directly
/// before side effects; callers cannot construct a valid plan themselves.
struct ControlAdmissionPlan : ControlAuthorityBinding {
    ControlReceiptId receipt_id;

    const ControlAuthorityBinding& authority_binding() const {
        return *this;
    }
};

struct ControlAdmissionResult {
    ControlAdmissionStatus status = ControlAdmissionStatus::InvalidRequest;
    ControlPermissionDecision permission;
    std::optional<ControlAdmissionPlan> plan;
    std::optional<ControlOperationReceipt> receipt;
    bool should_dispatch = false;
};

/// Trusted, bounded, non-blocking broker-side evaluators for the three dynamic
/// permission terms not owned by identity, manifest, or grant state. They run
/// inside the broker coordination boundary and must not re-enter ControlBroker.
/// Defaults are fail-closed.
struct ControlAdmissionPolicy {
    using Predicate =
        std::function<bool(const ControlRegistration&, const ControlOperationDescriptor&)>;

    Predicate host_available;
    Predicate activated;
    Predicate policy_eligible;
};

/// Converts a strictly decoded wire request into the broker-owned admission
/// request. It verifies the canonical request hash before any authority lookup.
std::optional<ControlAdmissionRequest>
control_admission_request(const ControlRequestEnvelope& envelope);

} // namespace pulp::inspect
