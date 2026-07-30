#include <pulp/inspect/capabilities.hpp>
#include <pulp/inspect/protocol.hpp>

#include <array>

namespace pulp::inspect {
namespace {

#define PULP_INSPECT_CAPABILITY(symbol, id, risk, observe, develop, grantable) \
    InspectorCapabilityDescriptor{InspectorCapability::symbol, id, \
                                  InspectorCapabilityRisk::risk, observe, develop, \
                                  grantable},
constexpr auto kCapabilities = std::to_array<InspectorCapabilityDescriptor>({
#include <pulp/inspect/capability_definitions.inc>
});
#undef PULP_INSPECT_CAPABILITY

#define PULP_IF_0(...)
#define PULP_IF_1(...) __VA_ARGS__
#define PULP_INSPECT_CAPABILITY(symbol, id, risk, observe, develop, grantable) \
    PULP_IF_##observe(InspectorCapability::symbol,)
constexpr auto kObserveCapabilities = std::to_array<InspectorCapability>({
#include <pulp/inspect/capability_definitions.inc>
});
#undef PULP_INSPECT_CAPABILITY

#define PULP_INSPECT_CAPABILITY(symbol, id, risk, observe, develop, grantable) \
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
                kCapabilities[i].id == kCapabilities[j].id)
                return false;
        }
    }
    return true;
}

static_assert(method_registry_is_unique(),
              "Every inspector method must have exactly one capability mapping");
static_assert(capability_registry_is_unique(),
              "Inspector capability symbols and IDs must be unique");

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

std::optional<InspectorCapability> capability_from_id(std::string_view id) {
    for (const auto& descriptor : kCapabilities) {
        if (descriptor.id == id)
            return descriptor.capability;
    }
    return std::nullopt;
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
           risk == InspectorCapabilityRisk::HighRisk;
}

} // namespace pulp::inspect
