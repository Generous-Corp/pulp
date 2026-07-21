#pragma once

#include <pulp/playback/automation_program.hpp>

#include <memory>

namespace pulp::playback::detail {

inline bool automation_target_less(const AutomationProgram* lhs,
                                   const AutomationProgram* rhs) noexcept {
    const auto lhs_target = lhs->target();
    const auto rhs_target = rhs->target();
    if (lhs_target.device_placement_id != rhs_target.device_placement_id)
        return lhs_target.device_placement_id < rhs_target.device_placement_id;
    if (lhs_target.param_id != rhs_target.param_id)
        return lhs_target.param_id < rhs_target.param_id;
    return lhs->lane_id() < rhs->lane_id();
}

inline bool automation_lane_less(const std::shared_ptr<const AutomationProgram>& lhs,
                                 const std::shared_ptr<const AutomationProgram>& rhs) noexcept {
    if (lhs->lane_id() != rhs->lane_id())
        return lhs->lane_id() < rhs->lane_id();
    return automation_target_less(lhs.get(), rhs.get());
}

} // namespace pulp::playback::detail
