#pragma once

#include <pulp/timeline/schema_json.hpp>

#include <string_view>

namespace pulp::timeline::detail {

struct ProjectSummaryScan {
    std::string_view id;
    std::string_view name;
    std::string_view next_item_id;
    std::string_view root_sequence_id;
    std::uint32_t project_schema_version = 0;
    std::size_t asset_count = 0;
    std::size_t sequence_count = 0;
    std::size_t track_count = 0;
    std::size_t clip_count = 0;
    std::size_t note_count = 0;
    std::size_t device_placement_count = 0;
    std::size_t automation_lane_count = 0;
    std::size_t automation_point_count = 0;
    std::size_t take_lane_count = 0;
    std::size_t take_count = 0;
    std::size_t take_comp_segment_count = 0;
};

runtime::Result<ProjectSummaryScan, PersistenceError>
scan_project_summary(std::string_view json, const DecodeLimits& limits);

} // namespace pulp::timeline::detail
