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
inline constexpr std::string_view kControlLegacyInspectorJsonEncoding = "legacy-inspector-json-v1";
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
inline constexpr std::size_t kControlMaximumArtifactReadBytes = 1024u * 1024u;
inline constexpr std::size_t kControlMaximumArtifactChunkBase64Bytes =
    ((kControlMaximumArtifactReadBytes + 2u) / 3u) * 4u;
inline constexpr std::size_t kControlMaximumStatusIdBytes = 64;
inline constexpr std::size_t kControlMaximumErrorCodeBytes = 128;
inline constexpr std::size_t kControlMaximumSdkVersionBytes = 128;
inline constexpr std::size_t kControlMaximumArtifactMetadataFieldBytes = 512;
inline constexpr std::size_t kControlMaximumArtifactContentTypeBytes = 256;
inline constexpr std::size_t kControlHostPreflightMaximumBootstrapBase64Bytes = 24u * 1024u;
static_assert(kControlMaximumRequestPayloadBytes + 256u * 1024u <= kControlMaximumEnvelopeBytes);
static_assert(kControlMaximumResultDetailBytes + 256u * 1024u <= kControlMaximumEnvelopeBytes);
static_assert(kControlMaximumArtifactChunkBase64Bytes + 256u * 1024u <=
              kControlMaximumEnvelopeBytes);

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

struct ControlSessionOpenEnvelope {
    std::string request_id;
    std::string admission_id;
    friend bool operator==(const ControlSessionOpenEnvelope&,
                           const ControlSessionOpenEnvelope&) = default;
};

struct ControlSessionOpenResult {
    std::string request_id;
    bool accepted = false;
    std::string client_id;
    std::string error_code;
    std::string explanation;
    friend bool operator==(const ControlSessionOpenResult&,
                           const ControlSessionOpenResult&) = default;
};

/// Finite broker-management family used by installed control adapters. The
/// command is one of enroll, instances, grant-request, revoke, host-prepare,
/// host-prepare-installed, or host-launch; params and data are bounded canonical JSON objects whose
/// per-command shape is checked by the endpoint/client API.
struct ControlManagementEnvelope {
    std::string request_id;
    std::string command;
    std::string params_json = "{}";
    friend bool operator==(const ControlManagementEnvelope&,
                           const ControlManagementEnvelope&) = default;
};

struct ControlManagementResult {
    std::string request_id;
    std::string status_id;
    std::string data_json = "{}";
    std::string explanation;
    friend bool operator==(const ControlManagementResult&,
                           const ControlManagementResult&) = default;
};

struct ControlHostOpenEnvelope {
    std::string request_id;
    std::string admission_id;
    std::string enrollment_id;
    friend bool operator==(const ControlHostOpenEnvelope&,
                           const ControlHostOpenEnvelope&) = default;
};

struct ControlHostOpenResult {
    std::string request_id;
    bool accepted = false;
    std::string registration_id;
    std::string error_code;
    std::string explanation;
    friend bool operator==(const ControlHostOpenResult&, const ControlHostOpenResult&) = default;
};

struct ControlHostPreflightChallengeEnvelope {
    std::string nonce;
    friend bool operator==(const ControlHostPreflightChallengeEnvelope&,
                           const ControlHostPreflightChallengeEnvelope&) = default;
};

struct ControlHostPreflightResponseEnvelope {
    std::string nonce;
    friend bool operator==(const ControlHostPreflightResponseEnvelope&,
                           const ControlHostPreflightResponseEnvelope&) = default;
};

struct ControlHostPreflightBootstrapEnvelope {
    std::string nonce;
    std::string bootstrap_base64;
    friend bool operator==(const ControlHostPreflightBootstrapEnvelope&,
                           const ControlHostPreflightBootstrapEnvelope&) = default;
};

/// Broker-to-host execution request. Client and grant identities deliberately
/// remain broker-private; the host receives only the fields needed to run the
/// already-admitted operation.
struct ControlHostExecuteEnvelope {
    std::string route_id;
    std::string receipt_id;
    std::string operation_id;
    std::uint32_t operation_version = 1;
    std::int64_t deadline_unix_ms = 0;
    std::uint64_t expected_state_generation = 0;
    std::string params_json = "{}";
    friend bool operator==(const ControlHostExecuteEnvelope&,
                           const ControlHostExecuteEnvelope&) = default;
};

struct ControlHostProgressEnvelope {
    std::string route_id;
    std::uint64_t current = 0;
    std::uint64_t total = 0;
    std::string detail_json = "{}";
    friend bool operator==(const ControlHostProgressEnvelope&,
                           const ControlHostProgressEnvelope&) = default;
};

struct ControlHostCancelEnvelope {
    std::string route_id;
    std::string reason;
    friend bool operator==(const ControlHostCancelEnvelope&,
                           const ControlHostCancelEnvelope&) = default;
};

struct ControlArtifactReadEnvelope {
    std::string request_id;
    std::string artifact_id;
    std::uint64_t offset = 0;
    std::size_t maximum_bytes = 0;
    friend bool operator==(const ControlArtifactReadEnvelope&,
                           const ControlArtifactReadEnvelope&) = default;
};

/// Flattened, bounded wire representation of artifact metadata. Carrier code
/// explicitly maps this DTO to the broker-owned artifact model.
struct ControlArtifactWireMetadata {
    std::string artifact_id;
    std::string broker_id;
    std::string receipt_id;
    std::string producer_client_id;
    std::string producer_registration_id;
    std::string session_id;
    std::string instance_id;
    std::string publication_id;
    std::string producer_capability_id;
    std::string producer_operation_id;
    std::uint32_t producer_operation_version = 0;
    std::string original_grant_id;
    std::string consent_decision_id;
    std::string manifest_digest;
    std::string producer_artifact_digest;
    std::string sha256;
    std::uint64_t byte_size = 0;
    std::string content_type;
    std::uint64_t created_at_unix_ms = 0;
    std::uint64_t expires_at_unix_ms = 0;
    std::string sensitivity_id;
    std::string deletion_state_id;
    std::string redaction_state_id;
    friend bool operator==(const ControlArtifactWireMetadata&,
                           const ControlArtifactWireMetadata&) = default;
};

struct ControlArtifactReadResponseEnvelope {
    std::string request_id;
    std::string status_id;
    std::optional<ControlArtifactWireMetadata> metadata;
    std::string bytes_base64;
    bool eof = false;
    std::string explanation;
    friend bool operator==(const ControlArtifactReadResponseEnvelope&,
                           const ControlArtifactReadResponseEnvelope&) = default;
};

struct ControlHealthEnvelope {
    std::string request_id;
    friend bool operator==(const ControlHealthEnvelope&, const ControlHealthEnvelope&) = default;
};

struct ControlHealthResult {
    std::string request_id;
    std::string sdk_version;
    ControlProtocolRange protocol_versions;
    std::string broker_id;
    std::uint64_t process_generation = 0;
    friend bool operator==(const ControlHealthResult&, const ControlHealthResult&) = default;
};

struct ControlErrorEnvelope {
    std::string request_id;
    std::string error_code;
    std::string explanation;
    friend bool operator==(const ControlErrorEnvelope&, const ControlErrorEnvelope&) = default;
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

/// Host-authored execution outcome. CompletedAfterRevocation is intentionally
/// unavailable here; only the broker may derive that durable receipt state.
struct ControlHostCompleteEnvelope {
    std::string route_id;
    ControlReceiptState terminal_state = ControlReceiptState::Failed;
    std::optional<ControlResultCode> result_code;
    ControlRetryClassification retry = ControlRetryClassification::Never;
    std::string explanation;
    std::string detail_json = "{}";
    std::string cancellation_reason;
    friend bool operator==(const ControlHostCompleteEnvelope&,
                           const ControlHostCompleteEnvelope&) = default;
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

/// Lossless compatibility detail for a failed operation declared with the
/// legacy-inspector-json-v1 adapter encoding. The typed receipt result remains
/// authoritative for policy and retry behavior; this tuple preserves the
/// existing client-facing error contract without deriving codes from prose.
struct ControlLegacyInspectorError {
    std::string error_code;
    std::string error_message;
    std::string error_data_json;
    friend bool operator==(const ControlLegacyInspectorError&,
                           const ControlLegacyInspectorError&) = default;
};

using ControlEnvelopePayload =
    std::variant<ControlNegotiationOffer, ControlNegotiationResult, ControlRequestEnvelope,
                 ControlCancelEnvelope, ControlProgressEnvelope, ControlReceiptEnvelope,
                 ControlSessionOpenEnvelope, ControlSessionOpenResult, ControlManagementEnvelope,
                 ControlManagementResult, ControlArtifactReadEnvelope,
                 ControlArtifactReadResponseEnvelope, ControlHealthEnvelope, ControlHealthResult,
                 ControlErrorEnvelope, ControlHostOpenEnvelope, ControlHostOpenResult,
                 ControlHostPreflightChallengeEnvelope, ControlHostPreflightResponseEnvelope,
                 ControlHostPreflightBootstrapEnvelope, ControlHostExecuteEnvelope,
                 ControlHostProgressEnvelope, ControlHostCancelEnvelope,
                 ControlHostCompleteEnvelope>;

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

enum class ControlEnvelopeDirection : std::uint8_t {
    ClientToBroker,
    BrokerToClient,
    HostToBroker,
    BrokerToHost,
    HostToLauncher,
    LauncherToHost,
};

/// Direction is transport state, not payload authority. Carriers call this
/// after decoding and close on a role-incompatible frame.
bool control_envelope_allowed(const ControlEnvelope& envelope, ControlEnvelopeDirection direction);

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

/// Encodes/decodes the strict compatibility object carried in a failed
/// adapter receipt's detail_json. Unknown structured error data is retained as
/// its original JSON spelling. Malformed or out-of-bounds input fails closed.
std::optional<std::string>
encode_control_legacy_inspector_error(const ControlLegacyInspectorError& error);
std::optional<ControlLegacyInspectorError>
decode_control_legacy_inspector_error(std::string_view detail_json);

/// Emits one deterministic JSON representation. Returns an empty string if the
/// in-memory envelope violates the same bounds enforced by the decoder.
std::string encode_control_envelope(const ControlEnvelope& envelope);

std::optional<ControlEnvelope>
decode_control_envelope(std::string_view json, ControlProtocolDiagnostics* diagnostics = nullptr);

} // namespace pulp::inspect
