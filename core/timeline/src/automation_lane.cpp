#include <pulp/timeline/automation_lane.hpp>

#include <utility>

namespace pulp::timeline {
namespace {

struct TargetValidation {
    bool valid;
    AutomationLaneErrorCode code;
    ItemId related_item;
};

} // namespace

AutomationLane::AutomationLane(ItemId id, AutomationTarget target, AutomationCurve curve) noexcept
    : id_(id), target_(std::move(target)), curve_(std::move(curve)) {}

runtime::Result<AutomationLane, AutomationLaneError>
AutomationLane::create(ItemId id, AutomationTarget target, AutomationCurve curve) {
    if (!id.valid()) {
        return runtime::Result<AutomationLane, AutomationLaneError>(
            runtime::Err(AutomationLaneError{AutomationLaneErrorCode::InvalidLaneId, id, {}}));
    }
    const auto validation = std::visit(
        AutomationTargetCases{
            [](const DeviceParameterTarget& candidate) -> TargetValidation {
                return {candidate.valid(), AutomationLaneErrorCode::InvalidDevicePlacementId,
                        candidate.device_placement_id};
            },
            // A mixer target names a control the owning track always has, so
            // there is no referenced identity that could be missing.
            [](const TrackMixerTarget& candidate) -> TargetValidation {
                return {candidate.valid(), AutomationLaneErrorCode::InvalidLaneId, {}};
            },
        },
        target);
    if (!validation.valid) {
        return runtime::Result<AutomationLane, AutomationLaneError>(
            runtime::Err(AutomationLaneError{validation.code, id, validation.related_item}));
    }
    return runtime::Result<AutomationLane, AutomationLaneError>(
        runtime::Ok(AutomationLane(id, std::move(target), std::move(curve))));
}

AutomationLane AutomationLane::with_curve(AutomationCurve replacement) const noexcept {
    return AutomationLane(id_, target_, std::move(replacement));
}

} // namespace pulp::timeline
