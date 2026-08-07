#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace pulp::inspect {

inline constexpr std::uint32_t kControlProtocolVersion = 1;
inline constexpr std::string_view kControlEnvelopeSchema = "dev.pulp.control/envelope@1";
inline constexpr std::size_t kControlMaximumRequestPayloadBytes = 512u * 1024u;
inline constexpr std::size_t kControlMaximumEnvelopeBytes = 2u * 1024u * 1024u;
inline constexpr std::size_t kControlMaximumResultDetailBytes = 1600u * 1024u;
/// Receipt normalization and wire encoding use these same field boundaries.
inline constexpr std::size_t kControlReceiptMaximumRequestIdBytes = 128;
inline constexpr std::size_t kControlReceiptMaximumOperationIdBytes = 256;
inline constexpr std::size_t kControlReceiptMaximumExplanationBytes = 1024;
inline constexpr std::size_t kControlReceiptMaximumCancellationReasonBytes = 1024;
inline constexpr std::size_t kControlReceiptMaximumArtifacts = 64;
inline constexpr std::size_t kControlReceiptMaximumArtifactIdBytes = 128;
inline constexpr std::size_t kControlReceiptMaximumArtifactMediaTypeBytes = 128;
inline constexpr std::size_t kControlReceiptMaximumEvidenceIds = 64;
inline constexpr std::size_t kControlReceiptMaximumEvidenceIdBytes = 128;
static_assert(kControlMaximumRequestPayloadBytes + 256u * 1024u <= kControlMaximumEnvelopeBytes);
static_assert(kControlMaximumResultDetailBytes + 256u * 1024u <= kControlMaximumEnvelopeBytes);

struct ControlProtocolRange {
    std::uint32_t minimum = kControlProtocolVersion;
    std::uint32_t maximum = kControlProtocolVersion;
    friend bool operator==(const ControlProtocolRange&, const ControlProtocolRange&) = default;
};

struct ControlNegotiationOffer {
    ControlProtocolRange versions;
    std::vector<std::string> mandatory_features;
    std::vector<std::string> optional_features;
    friend bool operator==(const ControlNegotiationOffer&,
                           const ControlNegotiationOffer&) = default;
};

enum class ControlNegotiationStatus : std::uint8_t {
    Accepted,
    InvalidOffer,
    NoCommonVersion,
    DowngradeRejected,
    UnsupportedMandatoryFeature,
};

struct ControlNegotiationResult {
    ControlNegotiationStatus status = ControlNegotiationStatus::InvalidOffer;
    std::uint32_t selected_version = 0;
    std::vector<std::string> features;
    std::string explanation;
    friend bool operator==(const ControlNegotiationResult&,
                           const ControlNegotiationResult&) = default;
};

/// Selects the highest common revision at or above local_security_floor.
ControlNegotiationResult
negotiate_control_protocol(const ControlNegotiationOffer& local,
                           const ControlNegotiationOffer& peer,
                           std::uint32_t local_security_floor = kControlProtocolVersion);

struct ControlRequestEnvelope {
    std::string request_id;
    std::string client_id;
    std::string registration_id;
    std::string grant_id;
    std::string instance_generation;
    std::string operation_id;
    std::uint32_t operation_version = 1;
    std::string idempotency_key;
    std::string request_hash;
    std::int64_t deadline_unix_ms = 0;
    std::uint64_t expected_state_generation = 0;
    std::string params_json = "{}";
    friend bool operator==(const ControlRequestEnvelope&, const ControlRequestEnvelope&) = default;
};

struct ControlCancelEnvelope {
    std::string request_id;
    /// Required on the wire; the empty default is an invalid/uninitialized sentinel.
    std::string reason;
    friend bool operator==(const ControlCancelEnvelope&, const ControlCancelEnvelope&) = default;
};

/// A bounded progress observation for one admitted operation. Sequence numbers
/// start at one; `current` may equal `total` but never exceeds it.
struct ControlProgressEnvelope {
    std::string request_id;
    std::string receipt_id;
    std::uint64_t sequence = 0;
    std::uint64_t current = 0;
    std::uint64_t total = 0;
    std::string detail_json = "{}";
    friend bool operator==(const ControlProgressEnvelope&,
                           const ControlProgressEnvelope&) = default;
};

enum class ControlReceiptState : std::uint8_t {
    Admitted,
    Running,
    Completed,
    Failed,
    Cancelled,
    CompletedAfterRevocation,
    UnknownNeedsRefresh,
};

enum class ControlResultCode : std::uint8_t {
    NotImplemented,
    NotBuilt,
    HostUnavailable,
    Inactive,
    PolicyDenied,
    GrantRequired,
    GrantExpired,
    ConsentRequired,
    LeaseConflict,
    StateConflict,
    SessionStale,
    InvalidRequest,
    DeadlineExceeded,
    Cancelled,
    CompletedAfterRevocation,
    UnknownNeedsRefresh,
    ResourceExhausted,
    InternalError,
};

enum class ControlRetryClassification : std::uint8_t {
    Never,
    AfterRefresh,
    AfterGrant,
    AfterBackoff,
};

struct ControlArtifactHandle {
    std::string artifact_id;
    std::string media_type;
    std::uint64_t byte_size = 0;
    friend bool operator==(const ControlArtifactHandle&, const ControlArtifactHandle&) = default;
};

struct ControlReceiptEnvelope {
    std::string request_id;
    std::string receipt_id;
    std::string operation_id;
    std::uint32_t operation_version = 1;
    ControlReceiptState state = ControlReceiptState::Admitted;
    std::optional<ControlResultCode> result_code;
    ControlRetryClassification retry = ControlRetryClassification::Never;
    std::string explanation;
    std::string detail_json = "{}";
    std::vector<ControlArtifactHandle> artifacts;
    friend bool operator==(const ControlReceiptEnvelope&, const ControlReceiptEnvelope&) = default;
};

using ControlEnvelopePayload =
    std::variant<ControlNegotiationOffer, ControlNegotiationResult, ControlRequestEnvelope,
                 ControlCancelEnvelope, ControlProgressEnvelope, ControlReceiptEnvelope>;

struct ControlEnvelope {
    std::uint32_t schema_version = kControlProtocolVersion;
    ControlEnvelopePayload payload;
    friend bool operator==(const ControlEnvelope&, const ControlEnvelope&) = default;
};

enum class ControlProtocolError : std::uint8_t {
    None,
    EnvelopeTooLarge,
    Parse,
    RootType,
    UnknownField,
    MissingField,
    InvalidType,
    UnsupportedSchema,
    UnsupportedVersion,
    InvalidValue,
    LimitExceeded,
    HashMismatch,
};

struct ControlProtocolDiagnostics {
    ControlProtocolError code = ControlProtocolError::None;
    std::string explanation;
};

enum class ControlJsonSchemaError : std::uint8_t {
    None,
    InvalidDocument,
    InvalidSchema,
    UnsupportedKeyword,
    LimitExceeded,
    ValidationFailed,
};

struct ControlJsonSchemaDiagnostics {
    ControlJsonSchemaError code = ControlJsonSchemaError::None;
    std::string path;
    std::string explanation;
};

/// Validates an untrusted JSON instance against the bounded JSON Schema subset
/// used by Product A operation descriptors. The schema is treated as trusted
/// registry data, but unsupported or malformed schema keywords still fail
/// closed so adding a contract keyword cannot silently weaken enforcement.
bool validate_control_json_schema(std::string_view instance_json, std::string_view schema_json,
                                  ControlJsonSchemaDiagnostics* diagnostics = nullptr);

/// Validates an operation result using the larger, still-bounded result budget.
/// Request validation deliberately remains on the smaller control payload budget.
bool validate_control_output_json_schema(std::string_view instance_json,
                                         std::string_view schema_json,
                                         ControlJsonSchemaDiagnostics* diagnostics = nullptr);

std::string_view control_negotiation_status_id(ControlNegotiationStatus status);
std::string_view control_receipt_state_id(ControlReceiptState state);
std::string_view control_result_code_id(ControlResultCode code);
std::string_view control_retry_classification_id(ControlRetryClassification retry);

bool control_receipt_state_is_terminal(ControlReceiptState state);
bool valid_control_receipt_transition(ControlReceiptState from, ControlReceiptState to);

/// Checks that a later observation belongs to the same admitted operation and
/// advances both sequence and completed work without changing the total.
bool valid_control_progress_transition(const ControlProgressEnvelope& previous,
                                       const ControlProgressEnvelope& next);

/// Canonicalizes an arbitrary JSON value by recursively sorting object keys.
/// Returns nullopt for malformed JSON or inputs exceeding the payload bound.
std::optional<std::string> canonicalize_control_json(std::string_view json);

/// Hashes the authority and operation binding plus canonical request parameters.
/// The transport request_id and deadline are deliberately not part of replay identity.
std::optional<std::string> control_request_hash(const ControlRequestEnvelope& request);

/// Emits one deterministic JSON representation. Returns an empty string if the
/// in-memory envelope violates the same bounds enforced by the decoder.
std::string encode_control_envelope(const ControlEnvelope& envelope);

std::optional<ControlEnvelope>
decode_control_envelope(std::string_view json, ControlProtocolDiagnostics* diagnostics = nullptr);

} // namespace pulp::inspect
