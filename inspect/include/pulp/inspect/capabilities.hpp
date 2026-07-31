#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace pulp::inspect {

enum class InspectorCapability : std::uint8_t {
#define PULP_INSPECT_CAPABILITY( \
    symbol, id, risk, observe, develop, grantable, publication_bound) symbol,
#include <pulp/inspect/capability_definitions.inc>
#undef PULP_INSPECT_CAPABILITY
};

enum class InspectorMethodKind : std::uint8_t {
    Request,
    Event,
};

enum class InspectorCapabilityRisk : std::uint8_t {
    Observe,
    Control,
    HighRisk,
    Unavailable,
};

enum class InspectorProfile : std::uint8_t {
    Off,
    Observe,
    Develop,
    Custom,
};

struct InspectorMethodDescriptor {
    std::string_view method;
    InspectorCapability capability;
    InspectorMethodKind kind;
};

struct InspectorCapabilityDescriptor {
    InspectorCapability capability;
    std::string_view id;
    InspectorCapabilityRisk risk;
    bool in_observe_profile;
    bool in_develop_profile;
    bool grantable;
    bool publication_bound;
};

std::span<const InspectorCapabilityDescriptor> inspector_capability_registry();
std::string_view capability_id(InspectorCapability capability);
std::optional<InspectorCapability> capability_from_id(std::string_view id);

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
