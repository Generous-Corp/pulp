#pragma once

#include <pulp/playback/automation_program.hpp>

#include <memory>
#include <variant>

namespace pulp::playback::detail {

// A total order over every target kind. The leading discriminator keeps a mixer
// target from sorting into the middle of a device's lanes, so grouping by device
// stays a contiguous scan and two kinds can never compare equal.
struct AutomationTargetOrderKey {
    std::size_t kind = 0;
    timeline::ItemId referenced_item;
    std::uint32_t parameter = 0;
    constexpr auto operator<=>(const AutomationTargetOrderKey&) const = default;
};

inline AutomationTargetOrderKey
automation_target_order_key(const timeline::AutomationTarget& target) noexcept {
    return std::visit(
        timeline::AutomationTargetCases{
            [](const timeline::DeviceParameterTarget& device) {
                return AutomationTargetOrderKey{0, device.device_placement_id, device.param_id};
            },
            [](const timeline::TrackMixerTarget& mixer) {
                return AutomationTargetOrderKey{1, {},
                                                static_cast<std::uint32_t>(mixer.parameter)};
            },
        },
        target);
}

inline bool automation_target_less(const AutomationProgram* lhs,
                                   const AutomationProgram* rhs) noexcept {
    const auto lhs_key = automation_target_order_key(lhs->target());
    const auto rhs_key = automation_target_order_key(rhs->target());
    if (lhs_key != rhs_key)
        return lhs_key < rhs_key;
    return lhs->lane_id() < rhs->lane_id();
}

inline bool automation_lane_less(const std::shared_ptr<const AutomationProgram>& lhs,
                                 const std::shared_ptr<const AutomationProgram>& rhs) noexcept {
    if (lhs->lane_id() != rhs->lane_id())
        return lhs->lane_id() < rhs->lane_id();
    return automation_target_less(lhs.get(), rhs.get());
}

} // namespace pulp::playback::detail
