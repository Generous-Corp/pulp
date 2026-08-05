#pragma once

#include <pulp/inspect/capabilities.hpp>

#include <optional>
#include <string_view>
#include <vector>

namespace pulp::format::detail {

std::optional<inspect::InspectorProfile>
parse_standalone_inspector_profile(std::string_view profile);

std::vector<inspect::InspectorCapability>
standalone_inspector_capabilities(bool compositor_capture,
                                  bool runtime_eval_enabled);

bool standalone_inspector_capability_available(
    inspect::InspectorCapability capability, bool compositor_capture,
    bool runtime_eval_enabled);

} // namespace pulp::format::detail
