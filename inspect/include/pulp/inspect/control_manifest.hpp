#pragma once

#include <pulp/inspect/capabilities.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::inspect {

inline constexpr std::uint32_t kControlManifestSchemaVersion = 1;
inline constexpr std::string_view kControlManifestSchemaId = "dev.pulp.control/artifact-manifest@1";
// Updated deliberately whenever the canonical v1 registry bytes change.
#include <pulp/inspect/control_registry_digest.inc>
inline constexpr std::string_view kControlRegistryDigest = PULP_CONTROL_REGISTRY_DIGEST_V1;
#undef PULP_CONTROL_REGISTRY_DIGEST_V1

enum class ControlBuildProfile : std::uint8_t {
    ProductionStripped,
    DeveloperLocal,
    TestDeterministic,
    SupportDiagnostics,
    ResearchUnsafe,
};

enum class ControlManifestError : std::uint8_t {
    None,
    Parse,
    RootType,
    UnknownField,
    MissingOrInvalidField,
    UnsupportedSchema,
    VersionDowngrade,
    VersionTooNew,
    UnknownProfile,
    PermissionTermsMismatch,
    CapabilityLimit,
    UnknownCapability,
    InvalidCapabilitySet,
    MissingCapabilityDependency,
    ProfileViolation,
    InvalidProfile,
    InvalidIdentity,
    RegistryMismatch,
    EndpointMismatch,
    UnsafeAcknowledgementMismatch,
};

enum class ControlDenialReason : std::uint8_t {
    UnknownCapability,
    NotImplemented,
    NotBuilt,
    HostUnavailable,
    NotActivated,
    PolicyIneligible,
    ClientNotGranted,
    SessionNotLive,
    ProfileForbidden,
    UnsupportedExecutor,
    PublicationMismatch,
};

struct ControlManifest {
    std::uint32_t schema_version = kControlManifestSchemaVersion;
    ControlBuildProfile profile = ControlBuildProfile::ProductionStripped;
    std::string target;
    std::string product_name;
    std::string bundle_id;
    std::string build_id;
    std::string registry_digest = std::string(kControlRegistryDigest);
    bool endpoint_included = false;
    bool unsafe_runtime_eval_acknowledged = false;
    std::vector<InspectorCapability> capabilities;
};

struct ControlManifestDiagnostics {
    ControlManifestError code = ControlManifestError::None;
    std::string error;
    std::vector<std::string> unknown_fields;
};

struct ControlManifestValidation {
    bool valid = false;
    ControlManifestError code = ControlManifestError::None;
    std::string error;
};

struct ControlArtifactExpectation {
    std::string profile_id;
    std::string manifest_digest;
    bool endpoint_included = false;
    bool runtime_eval_included = false;
    std::vector<std::string> capability_ids;
};

struct ControlArtifactValidation {
    bool valid = false;
    std::string error;
};

/// A frozen Product A operation contract. Schema bodies are canonical JSON
/// Schema, not projections of the legacy Inspector transport.
struct ControlArtifactResultBinding {
    bool produced = false;
    std::string_view artifact_id_field;
    std::string_view sha256_field;
    std::string_view byte_count_field;
    std::string_view media_type_field;
};

/// Binds a typed success-result field to the broker-minted durable receipt.
struct ControlReceiptResultBinding {
    bool bound = false;
    std::string_view receipt_id_field;
};

struct ControlOperationDescriptor {
    std::string_view id;
    std::uint32_t version = 1;
    InspectorCapability capability;
    std::string_view input_schema_id;
    std::string_view input_schema_json;
    std::string_view output_schema_id;
    std::string_view output_schema_json;
    std::string_view result_kind;
    ControlArtifactResultBinding artifact_binding;
    ControlReceiptResultBinding receipt_binding;
};

/// Inputs to the canonical permission equation. Every term defaults false so
/// an omitted or newly introduced authority can never become an implicit grant.
struct ControlPermissionInputs {
    bool implemented = false;
    bool built = false;
    bool host_available = false;
    bool activated = false;
    bool policy_eligible = false;
    bool client_granted = false;
    bool session_live = false;
};

struct ControlPermissionDecision {
    bool allowed = false;
    std::optional<ControlDenialReason> denial = ControlDenialReason::NotImplemented;
};

std::string_view control_profile_id(ControlBuildProfile profile);
std::optional<ControlBuildProfile> control_profile_from_id(std::string_view id);
std::string_view control_manifest_error_id(ControlManifestError error);
std::string_view control_denial_reason_id(ControlDenialReason reason);

std::span<const std::string_view> control_permission_terms();
ControlPermissionDecision evaluate_control_permission(const ControlPermissionInputs& inputs);

bool validate_control_manifest(const ControlManifest& manifest, std::string& error);
ControlManifestValidation validate_control_manifest_detailed(const ControlManifest& manifest);
std::optional<ControlManifest>
parse_control_manifest(std::string_view json, ControlManifestDiagnostics* diagnostics = nullptr);

/// Emits the unique stable byte representation used for artifact digests.
std::string serialize_control_manifest(const ControlManifest& manifest);
std::string control_manifest_digest(const ControlManifest& manifest);
std::string control_consent_identity(std::string_view manifest_digest,
                                     std::string_view artifact_digest);

/// Validates the immutable markers embedded in one exact executable snapshot.
/// Callers remain responsible for obtaining bytes through a race-safe snapshot.
ControlArtifactValidation
validate_control_artifact_bytes(std::string_view bytes,
                                const ControlArtifactExpectation& expectation);

std::span<const ControlOperationDescriptor> control_operation_registry();
const ControlOperationDescriptor* resolve_control_operation(std::string_view id,
                                                            std::uint32_t version);

/// Canonical projection of capability metadata and adapter operations.
std::string serialize_control_registry();

} // namespace pulp::inspect
