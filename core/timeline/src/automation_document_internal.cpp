#include "automation_document_internal.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <utility>

namespace pulp::timeline::detail {
namespace {

template <typename T>
runtime::Result<T, ModelError> fail(ModelErrorCode code, ItemId item = {}, ItemId related = {}) {
    return runtime::Err(ModelError{code, item, related});
}

// A target's identity for duplicate detection, ordered so two lanes that drive
// the same control always compare equal regardless of which kind they are. The
// leading discriminator keeps a mixer target from colliding with a device target
// that happens to share its zeroed placement ID.
struct TargetKey {
    std::uint8_t kind = 0;
    ItemId referenced_item;
    std::uint32_t parameter = 0;
    constexpr auto operator<=>(const TargetKey&) const = default;
};

TargetKey target_key(const AutomationLane& lane) noexcept {
    return std::visit(
        AutomationTargetCases{
            [](const DeviceParameterTarget& target) {
                return TargetKey{0, target.device_placement_id, target.param_id};
            },
            [](const TrackMixerTarget& target) {
                return TargetKey{1, {}, static_cast<std::uint32_t>(target.parameter)};
            },
        },
        lane.target());
}

// The document identity a target references, or none when the target names
// something the owning track always has. Only a referenced identity can be
// missing from the track.
std::optional<ItemId> referenced_device_placement(const AutomationLane& lane) noexcept {
    return std::visit(
        AutomationTargetCases{
            [](const DeviceParameterTarget& target) -> std::optional<ItemId> {
                return target.device_placement_id;
            },
            [](const TrackMixerTarget&) -> std::optional<ItemId> { return std::nullopt; },
        },
        lane.target());
}

// Rebuilds a target against a remap table, reporting the identity that had no
// replacement. A target that references nothing survives unchanged; one that
// does must resolve or the lane would silently retarget.
runtime::Result<AutomationTarget, ItemId> remap_target(const AutomationTarget& target,
                                                       const IdRemapTable& table) {
    using TargetResult = runtime::Result<AutomationTarget, ItemId>;
    return std::visit(
        AutomationTargetCases{
            [&](const DeviceParameterTarget& device) -> TargetResult {
                const auto mapped = table.find(device.device_placement_id);
                if (!mapped)
                    return runtime::Err(device.device_placement_id);
                return runtime::Ok(
                    AutomationTarget(DeviceParameterTarget{*mapped, device.param_id}));
            },
            [](const TrackMixerTarget& mixer) -> TargetResult {
                return runtime::Ok(AutomationTarget(mixer));
            },
        },
        target);
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

    std::vector<std::pair<TargetKey, ItemId>> targets;
    targets.reserve(lanes.size());
    for (const auto& lane : lanes) {
        if (const auto placement_id = referenced_device_placement(lane)) {
            const auto placement =
                std::find_if(device_chain.begin(), device_chain.end(),
                             [&](const DevicePlacement& candidate) {
                                 return candidate.id == *placement_id;
                             });
            if (placement == device_chain.end())
                return ModelError{ModelErrorCode::MissingAutomationTarget, lane.id(),
                                  *placement_id};
        }
        targets.emplace_back(target_key(lane), lane.id());
    }
    std::sort(targets.begin(), targets.end());
    const auto duplicate_target =
        std::adjacent_find(targets.begin(), targets.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.first == rhs.first;
        });
    if (duplicate_target != targets.end())
        return ModelError{ModelErrorCode::DuplicateAutomationTarget,
                          std::next(duplicate_target)->second, duplicate_target->second};
    return std::nullopt;
}

runtime::Result<AutomationLane, ModelError>
remap_attached_automation_lane(const AutomationLane& lane, const IdRemapTable& table) {
    const auto lane_id = table.find(lane.id());
    if (!lane_id)
        return fail<AutomationLane>(ModelErrorCode::InvalidIdentityTransition, lane.id(),
                                    lane.id());
    auto target = remap_target(lane.target(), table);
    if (!target)
        return fail<AutomationLane>(ModelErrorCode::InvalidIdentityTransition, lane.id(),
                                    target.error());

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
    auto remapped =
        AutomationLane::create(*lane_id, std::move(target).value(), std::move(curve).value());
    if (!remapped)
        return fail<AutomationLane>(ModelErrorCode::InvalidIdentityTransition,
                                    remapped.error().lane, remapped.error().related_item);
    return runtime::Ok(std::move(remapped).value());
}

} // namespace pulp::timeline::detail
