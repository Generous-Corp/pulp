#pragma once

#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_json.hpp>

namespace pulp::timeline::detail {

/// Decodes one parameter-target envelope.
///
/// Shared with modulation routes: a route and a lane address the same
/// parameters and use one spelling on the wire, so a second decoder would be a
/// second chance to disagree about it.
runtime::Result<ParameterTarget, PersistenceError>
decode_parameter_target(const JsonValue& value, const std::string& path);

runtime::Result<std::vector<AutomationLane>, PersistenceError>
decode_automation_lanes(const JsonValue& value, const DecodeLimits& limits, std::size_t& lane_count,
                        std::size_t& point_count, std::string path);

} // namespace pulp::timeline::detail
