#include "automation_document_internal.hpp"

#include <algorithm>
#include <iterator>
#include <tuple>

namespace pulp::timeline::detail {
namespace {

template <typename T>
runtime::Result<T, ModelError> fail(ModelErrorCode code, ItemId item = {}, ItemId related = {}) {
    return runtime::Err(ModelError{code, item, related});
}

const DeviceParameterTarget& device_target(const AutomationLane& lane) noexcept {
    return std::get<DeviceParameterTarget>(lane.target());
}

} // namespace

void append_automation_owned_ids(std::span<const AutomationLane> lanes, std::vector<ItemId>& ids) {
    for (const auto& lane : lanes) {
        ids.push_back(lane.id());
        for (const auto& point : lane.curve().points())
            ids.push_back(point.id);
    }
}

std::optional<ModelError>
validate_attached_automation(std::span<const AutomationLane> lanes,
                             std::span<const DevicePlacement> device_chain,
                             std::span<const ItemId> other_owned_ids) {
    std::vector<ItemId> ids(other_owned_ids.begin(), other_owned_ids.end());
    append_automation_owned_ids(lanes, ids);
    for (const auto id : ids)
        if (!id.valid())
            return ModelError{ModelErrorCode::InvalidItemId, id, {}};
    std::sort(ids.begin(), ids.end());
    if (const auto duplicate = std::adjacent_find(ids.begin(), ids.end()); duplicate != ids.end())
        return ModelError{ModelErrorCode::DuplicateItemId, *duplicate, {}};

    std::vector<std::tuple<ItemId, std::uint32_t, ItemId>> targets;
    targets.reserve(lanes.size());
    for (const auto& lane : lanes) {
        const auto& target = device_target(lane);
        const auto placement = std::find_if(device_chain.begin(), device_chain.end(),
                                            [&](const DevicePlacement& candidate) {
                                                return candidate.id == target.device_placement_id;
                                            });
        if (placement == device_chain.end())
            return ModelError{ModelErrorCode::MissingAutomationTarget, lane.id(),
                              target.device_placement_id};
        targets.emplace_back(target.device_placement_id, target.param_id, lane.id());
    }
    std::sort(targets.begin(), targets.end());
    const auto duplicate_target =
        std::adjacent_find(targets.begin(), targets.end(), [](const auto& lhs, const auto& rhs) {
            return std::get<0>(lhs) == std::get<0>(rhs) && std::get<1>(lhs) == std::get<1>(rhs);
        });
    if (duplicate_target != targets.end())
        return ModelError{ModelErrorCode::DuplicateAutomationTarget,
                          std::get<2>(*std::next(duplicate_target)),
                          std::get<2>(*duplicate_target)};
    return std::nullopt;
}

runtime::Result<AutomationLane, ModelError>
remap_attached_automation_lane(const AutomationLane& lane, const IdRemapTable& table) {
    const auto lane_id = table.find(lane.id());
    const auto target_id = table.find(device_target(lane).device_placement_id);
    if (!lane_id || !target_id)
        return fail<AutomationLane>(ModelErrorCode::InvalidIdentityTransition, lane.id(),
                                    device_target(lane).device_placement_id);

    std::vector<AutomationPoint> points(lane.curve().points().begin(), lane.curve().points().end());
    for (auto& point : points) {
        const auto mapped = table.find(point.id);
        if (!mapped)
            return fail<AutomationLane>(ModelErrorCode::InvalidIdentityTransition, point.id,
                                        lane.id());
        point.id = *mapped;
    }
    auto curve = AutomationCurve::create(std::move(points));
    if (!curve)
        return fail<AutomationLane>(ModelErrorCode::InvalidIdentityTransition, curve.error().point,
                                    curve.error().related_point);
    auto remapped = AutomationLane::create(
        *lane_id, DeviceParameterTarget{*target_id, device_target(lane).param_id},
        std::move(curve).value());
    if (!remapped)
        return fail<AutomationLane>(ModelErrorCode::InvalidIdentityTransition,
                                    remapped.error().lane, remapped.error().related_item);
    return runtime::Ok(std::move(remapped).value());
}

} // namespace pulp::timeline::detail
