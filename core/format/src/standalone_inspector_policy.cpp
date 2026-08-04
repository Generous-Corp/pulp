#include "standalone_inspector_policy.hpp"

#include <pulp/format/detail/standalone_inspector.hpp>
#include <pulp/view/scripted_ui.hpp>

#include <algorithm>
#include <string>

namespace pulp::format::detail {

std::optional<inspect::InspectorProfile>
parse_standalone_inspector_profile(std::string_view profile) {
    if (profile.empty() || profile == "off")
        return inspect::InspectorProfile::Off;
    return inspect::profile_from_id(profile);
}

std::vector<inspect::InspectorCapability>
standalone_inspector_capabilities(bool compositor_capture,
                                  bool runtime_eval_enabled) {
    using C = inspect::InspectorCapability;
    std::vector<inspect::InspectorCapability> result{
        C::SessionDescribe, C::SessionControl, C::StateRead, C::UiRead,
        C::DiagnosticsRead, C::LogsRead, C::StateWrite, C::TestInput,
        C::AuthoringTweaks, C::TelemetryStream};
    if (compositor_capture)
        result.push_back(C::CaptureImage);
    if (runtime_eval_enabled)
        result.push_back(C::RuntimeEval);
    return result;
}

bool standalone_inspector_capability_available(
    inspect::InspectorCapability capability, bool compositor_capture,
    bool runtime_eval_enabled) {
    const auto available = standalone_inspector_capabilities(
        compositor_capture, runtime_eval_enabled);
    return std::find(available.begin(), available.end(), capability)
        != available.end();
}

std::optional<std::string>
standalone_runtime_eval_realm_denial(const view::ScriptedUiSession* scripted_ui) {
    if (!scripted_ui)
        return std::nullopt;
    const auto effectful = scripted_ui->granted_capabilities().first_effectful();
    if (effectful)
        return "Runtime.evaluate denied: live scripted-UI realm grants effectful capability '"
            + std::string(view::capability_name(*effectful)) + "'";
    if (const auto* bridge = scripted_ui->bridge()) {
        const auto reset_denial = bridge->bounded_realm_retirement_denial();
        if (!reset_denial.empty())
            return "Runtime.evaluate denied: live scripted-UI "
                + std::string(reset_denial);
    }
    return std::nullopt;
}

} // namespace pulp::format::detail
