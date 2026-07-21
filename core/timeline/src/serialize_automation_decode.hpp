#pragma once

#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_json.hpp>

namespace pulp::timeline::detail {

runtime::Result<std::vector<AutomationLane>, PersistenceError>
decode_automation_lanes(const JsonValue& value, const DecodeLimits& limits, std::size_t& lane_count,
                        std::size_t& point_count, std::string path);

} // namespace pulp::timeline::detail
