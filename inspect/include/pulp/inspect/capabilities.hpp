#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace pulp::inspect {

enum class InspectorCapability : std::uint8_t {
#define PULP_INSPECT_CAPABILITY( \
    symbol, legacy_id, contract_id, risk, side_effect, executor, evidence, \
    observe, develop, grantable, publication_bound) symbol,
#include <pulp/inspect/capability_definitions.inc>
#undef PULP_INSPECT_CAPABILITY
};

enum class InspectorMethodKind : std::uint8_t {
    Request,
    Event,
};

enum class InspectorCapabilityRisk : std::uint8_t {
    Observe,
    Sensitive,
    StreamingSensitive,
    ResourceConsuming,
    Control,
    HighRisk,
    Critical,
    Unavailable,
};

enum class InspectorProfile : std::uint8_t {
    Off,
    Observe,
    Develop,
    Custom,
};

enum class InspectorSideEffect : std::uint8_t {
    None,
    Session,
    State,
    Capture,
    Trace,
    TestInput,
    Authoring,
    Telemetry,
    Evaluation,
    Render,
    Input,
    Reload,
    ArtifactRead,
};

enum class InspectorExecutor : std::uint8_t {
    Protocol,
    HostMain,
    Background,
    RuntimeEvaluator,
};

enum class InspectorEvidence : std::uint8_t {
    Response,
    Receipt,
    Artifact,
    Stream,
};

struct InspectorMethodDescriptor {
    std::string_view method;
    InspectorCapability capability;
    InspectorMethodKind kind;
};

struct InspectorCapabilityDescriptor {
    InspectorCapability capability;
    std::string_view id;
    std::string_view contract_id;
    InspectorCapabilityRisk risk;
    InspectorSideEffect side_effect;
    InspectorExecutor executor;
    InspectorEvidence evidence;
    std::string_view required_build_feature;
    std::string_view runtime_contexts;
    std::string_view host_tiers;
    std::string_view activation;
    std::string_view policy_predicates;
    std::string_view grant_scope;
    std::string_view cancellation;
    std::string_view timeout;
    std::string_view compatibility;
    std::string_view deprecation;
    bool in_observe_profile;
    bool in_develop_profile;
    bool grantable;
    bool publication_bound;
};

std::span<const InspectorCapabilityDescriptor> inspector_capability_registry();
std::string_view capability_id(InspectorCapability capability);
std::string_view capability_contract_id(InspectorCapability capability);
std::optional<InspectorCapability> capability_from_id(std::string_view id);
std::optional<InspectorCapability> capability_from_contract_id(std::string_view id);

std::string_view capability_risk_id(InspectorCapabilityRisk risk);
std::string_view side_effect_id(InspectorSideEffect side_effect);
std::string_view executor_id(InspectorExecutor executor);
std::string_view evidence_id(InspectorEvidence evidence);

std::string_view profile_id(InspectorProfile profile);
std::optional<InspectorProfile> profile_from_id(std::string_view id);
std::span<const InspectorCapability> profile_capabilities(InspectorProfile profile);

std::span<const InspectorMethodDescriptor> inspector_method_registry();
const InspectorMethodDescriptor* find_inspector_method(std::string_view method);
bool capability_is_grantable(InspectorCapability capability);
InspectorCapabilityRisk capability_risk(InspectorCapability capability);
bool capability_requires_controller_lease(InspectorCapability capability);
bool capability_requires_publication_binding(
    InspectorCapability capability);
bool inspector_event_is_lossy(std::string_view method);

} // namespace pulp::inspect
