#include <pulp/inspect/capabilities.hpp>
#include <pulp/inspect/protocol.hpp>

#include <array>

namespace pulp::inspect {
namespace {

#define PULP_BUILD_FEATURE_Protocol "inspect-protocol"
#define PULP_BUILD_FEATURE_HostMain "inspect-host-bridge"
#define PULP_BUILD_FEATURE_Background "inspect-runtime"
#define PULP_BUILD_FEATURE_OfflineJob "inspect-offline-runtime"
#define PULP_BUILD_FEATURE_RuntimeEvaluator "inspect-runtime-eval"
#define PULP_CONTEXTS_Protocol "standalone,pulp-host,offline"
#define PULP_CONTEXTS_HostMain "standalone,pulp-host"
#define PULP_CONTEXTS_Background "standalone,pulp-host,offline"
#define PULP_CONTEXTS_OfflineJob "offline"
#define PULP_CONTEXTS_RuntimeEvaluator "standalone-research"
#define PULP_HOST_TIERS_Protocol "pulp-owned,trusted-bridge"
#define PULP_HOST_TIERS_HostMain "pulp-owned,trusted-bridge"
#define PULP_HOST_TIERS_Background "pulp-owned,trusted-bridge"
#define PULP_HOST_TIERS_OfflineJob "pulp-owned"
#define PULP_HOST_TIERS_RuntimeEvaluator "pulp-owned"
#define PULP_GRANT_SCOPE_0 "none"
#define PULP_GRANT_SCOPE_1 "instance-operation"

#define PULP_INSPECT_CAPABILITY( \
    symbol, legacy_id, contract_id, risk, side_effect, executor, evidence, \
    observe, develop, grantable, publication_bound) \
    InspectorCapabilityDescriptor{InspectorCapability::symbol, legacy_id, contract_id, \
                                  InspectorCapabilityRisk::risk, \
                                  InspectorSideEffect::side_effect, \
                                  InspectorExecutor::executor, \
                                  InspectorEvidence::evidence, \
                                  PULP_BUILD_FEATURE_##executor, \
                                  PULP_CONTEXTS_##executor, \
                                  PULP_HOST_TIERS_##executor, \
                                  "explicit", \
                                  "seven-term-permission-equation-v1", \
                                  PULP_GRANT_SCOPE_##grantable, \
                                  "cooperative-before-dispatch", \
                                  "broker-deadline-required", \
                                  "same-major-additive", "not-deprecated", \
                                  observe, develop, \
                                  grantable, publication_bound},
constexpr auto kCapabilities = std::to_array<InspectorCapabilityDescriptor>({
#include <pulp/inspect/capability_definitions.inc>
});
#undef PULP_INSPECT_CAPABILITY

#define PULP_IF_0(...)
#define PULP_IF_1(...) __VA_ARGS__
#define PULP_INSPECT_CAPABILITY( \
    symbol, legacy_id, contract_id, risk, side_effect, executor, evidence, \
    observe, develop, grantable, publication_bound) \
    PULP_IF_##observe(InspectorCapability::symbol,)
constexpr auto kObserveCapabilities = std::to_array<InspectorCapability>({
#include <pulp/inspect/capability_definitions.inc>
});
#undef PULP_INSPECT_CAPABILITY

#define PULP_INSPECT_CAPABILITY( \
    symbol, legacy_id, contract_id, risk, side_effect, executor, evidence, \
    observe, develop, grantable, publication_bound) \
    PULP_IF_##develop(InspectorCapability::symbol,)
constexpr auto kDevelopCapabilities = std::to_array<InspectorCapability>({
#include <pulp/inspect/capability_definitions.inc>
});
#undef PULP_INSPECT_CAPABILITY
#undef PULP_IF_1
#undef PULP_IF_0

#define PULP_INSPECT_METHOD(symbol, wire_name, capability, kind) \
    InspectorMethodDescriptor{methods::symbol, InspectorCapability::capability, \
                              InspectorMethodKind::kind},
constexpr auto kMethods = std::to_array<InspectorMethodDescriptor>({
#include <pulp/inspect/protocol_methods.inc>
});
#undef PULP_INSPECT_METHOD

consteval bool method_registry_is_unique() {
    for (std::size_t i = 0; i < kMethods.size(); ++i) {
        if (kMethods[i].method.empty())
            return false;
        for (std::size_t j = i + 1; j < kMethods.size(); ++j) {
            if (kMethods[i].method == kMethods[j].method)
                return false;
        }
    }
    return true;
}

consteval bool capability_registry_is_unique() {
    for (std::size_t i = 0; i < kCapabilities.size(); ++i) {
        if (kCapabilities[i].id.empty())
            return false;
        for (std::size_t j = i + 1; j < kCapabilities.size(); ++j) {
            if (kCapabilities[i].capability == kCapabilities[j].capability ||
                kCapabilities[i].id == kCapabilities[j].id ||
                kCapabilities[i].contract_id == kCapabilities[j].contract_id)
                return false;
        }
    }
    return true;
}

static_assert(method_registry_is_unique(),
              "Every inspector method must have exactly one capability mapping");
static_assert(capability_registry_is_unique(),
              "Inspector capability symbols and IDs must be unique");

#undef PULP_GRANT_SCOPE_1
#undef PULP_GRANT_SCOPE_0
#undef PULP_HOST_TIERS_RuntimeEvaluator
#undef PULP_HOST_TIERS_OfflineJob
#undef PULP_HOST_TIERS_Background
#undef PULP_HOST_TIERS_HostMain
#undef PULP_HOST_TIERS_Protocol
#undef PULP_CONTEXTS_RuntimeEvaluator
#undef PULP_CONTEXTS_OfflineJob
#undef PULP_CONTEXTS_Background
#undef PULP_CONTEXTS_HostMain
#undef PULP_CONTEXTS_Protocol
#undef PULP_BUILD_FEATURE_RuntimeEvaluator
#undef PULP_BUILD_FEATURE_OfflineJob
#undef PULP_BUILD_FEATURE_Background
#undef PULP_BUILD_FEATURE_HostMain
#undef PULP_BUILD_FEATURE_Protocol

} // namespace

std::span<const InspectorCapabilityDescriptor> inspector_capability_registry() {
    return kCapabilities;
}

std::string_view capability_id(InspectorCapability capability) {
    for (const auto& descriptor : kCapabilities) {
        if (descriptor.capability == capability)
            return descriptor.id;
    }
    return "unavailable";
}

std::string_view capability_contract_id(InspectorCapability capability) {
    for (const auto& descriptor : kCapabilities) {
        if (descriptor.capability == capability)
            return descriptor.contract_id;
    }
    return "dev.pulp.unavailable/operation@1";
}

std::optional<InspectorCapability> capability_from_id(std::string_view id) {
    for (const auto& descriptor : kCapabilities) {
        if (descriptor.id == id)
            return descriptor.capability;
    }
    return std::nullopt;
}

std::optional<InspectorCapability> capability_from_contract_id(std::string_view id) {
    for (const auto& descriptor : kCapabilities) {
        if (descriptor.contract_id == id)
            return descriptor.capability;
    }
    return std::nullopt;
}

std::string_view capability_risk_id(InspectorCapabilityRisk risk) {
    switch (risk) {
        case InspectorCapabilityRisk::Observe: return "read-only";
        case InspectorCapabilityRisk::Sensitive: return "sensitive";
        case InspectorCapabilityRisk::StreamingSensitive:
            return "streaming-sensitive";
        case InspectorCapabilityRisk::ResourceConsuming:
            return "resource-consuming";
        case InspectorCapabilityRisk::Control: return "mutating";
        case InspectorCapabilityRisk::HighRisk: return "high-risk-mutation";
        case InspectorCapabilityRisk::Critical: return "critical";
        case InspectorCapabilityRisk::Unavailable: return "unavailable";
    }
    return "unavailable";
}

std::string_view side_effect_id(InspectorSideEffect side_effect) {
    switch (side_effect) {
        case InspectorSideEffect::None: return "none";
        case InspectorSideEffect::Session: return "session";
        case InspectorSideEffect::State: return "state";
        case InspectorSideEffect::Capture: return "capture";
        case InspectorSideEffect::Trace: return "trace";
        case InspectorSideEffect::TestInput: return "test-input";
        case InspectorSideEffect::Authoring: return "authoring";
        case InspectorSideEffect::Telemetry: return "telemetry";
        case InspectorSideEffect::Evaluation: return "evaluation";
        case InspectorSideEffect::Render: return "render";
        case InspectorSideEffect::Input: return "input";
        case InspectorSideEffect::Reload: return "reload";
        case InspectorSideEffect::ArtifactRead: return "artifact-read";
    }
    return "none";
}

std::string_view executor_id(InspectorExecutor executor) {
    switch (executor) {
        case InspectorExecutor::Protocol: return "protocol";
        case InspectorExecutor::HostMain: return "host-main";
        case InspectorExecutor::Background: return "background";
        case InspectorExecutor::OfflineJob: return "offline-job";
        case InspectorExecutor::RuntimeEvaluator: return "runtime-evaluator";
    }
    return "protocol";
}

std::string_view evidence_id(InspectorEvidence evidence) {
    switch (evidence) {
        case InspectorEvidence::Response: return "response";
        case InspectorEvidence::Receipt: return "receipt";
        case InspectorEvidence::Artifact: return "artifact";
        case InspectorEvidence::Stream: return "stream";
    }
    return "response";
}

std::string_view profile_id(InspectorProfile profile) {
    switch (profile) {
        case InspectorProfile::Off: return "off";
        case InspectorProfile::Observe: return "observe";
        case InspectorProfile::Develop: return "develop";
        case InspectorProfile::Custom: return "custom";
    }
    return "off";
}

std::optional<InspectorProfile> profile_from_id(std::string_view id) {
    if (id == "off") return InspectorProfile::Off;
    if (id == "observe") return InspectorProfile::Observe;
    if (id == "develop") return InspectorProfile::Develop;
    if (id == "custom") return InspectorProfile::Custom;
    return std::nullopt;
}

std::span<const InspectorCapability> profile_capabilities(InspectorProfile profile) {
    switch (profile) {
        case InspectorProfile::Observe: return kObserveCapabilities;
        case InspectorProfile::Develop: return kDevelopCapabilities;
        case InspectorProfile::Off:
        case InspectorProfile::Custom:
            return {};
    }
    return {};
}

std::span<const InspectorMethodDescriptor> inspector_method_registry() {
    return kMethods;
}

const InspectorMethodDescriptor* find_inspector_method(std::string_view method) {
    for (const auto& descriptor : kMethods) {
        if (descriptor.method == method)
            return &descriptor;
    }
    return nullptr;
}

bool capability_is_grantable(InspectorCapability capability) {
    for (const auto& descriptor : kCapabilities) {
        if (descriptor.capability == capability)
            return descriptor.grantable;
    }
    return false;
}

InspectorCapabilityRisk capability_risk(InspectorCapability capability) {
    for (const auto& descriptor : kCapabilities) {
        if (descriptor.capability == capability)
            return descriptor.risk;
    }
    return InspectorCapabilityRisk::Unavailable;
}

bool capability_requires_controller_lease(InspectorCapability capability) {
    const auto risk = capability_risk(capability);
    return risk == InspectorCapabilityRisk::Control ||
           risk == InspectorCapabilityRisk::HighRisk ||
           risk == InspectorCapabilityRisk::Critical;
}

bool capability_requires_publication_binding(
    InspectorCapability capability) {
    for (const auto& descriptor : kCapabilities) {
        if (descriptor.capability == capability)
            return descriptor.publication_bound;
    }
    return false;
}

bool inspector_event_is_lossy(std::string_view method) {
    const auto* descriptor = find_inspector_method(method);
    return descriptor &&
           descriptor->kind == InspectorMethodKind::Event &&
           descriptor->capability == InspectorCapability::TelemetryStream;
}

} // namespace pulp::inspect
