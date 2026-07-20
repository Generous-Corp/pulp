#include "transaction_automation_internal.hpp"

#include "transaction_internal.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace pulp::timeline::detail {
namespace {

runtime::Result<AutomationCommandReduction, TransactionError>
reject(ConflictCode code, const Transaction& transaction, CommandId command, ItemId item = {},
       ItemId related = {}) {
    return runtime::Err(TransactionError{code, transaction.id, command, item, related,
                                         transaction.expected_revision, {}, {}});
}

runtime::Result<AutomationCommandReduction, TransactionError>
model_failure(const Transaction& transaction, CommandId command, const ModelError& error) {
    return runtime::Err(TransactionError{ConflictCode::ModelInvariant,
                                         transaction.id,
                                         command,
                                         error.item,
                                         error.related_item,
                                         transaction.expected_revision,
                                         {},
                                         error});
}

std::optional<ConflictCode> target_error(const Project& project, ItemId id, ItemKind kind,
                                         ItemId sequence, ItemId track = {},
                                         ItemId automation_lane = {}) {
    const auto location = project.locate(id);
    if (!location)
        return ConflictCode::TargetMissing;
    if (!location->active)
        return ConflictCode::InactiveTarget;
    if (location->kind != kind)
        return ConflictCode::WrongTargetKind;
    if (location->sequence_id != sequence || location->track_id != track ||
        location->automation_lane_id != automation_lane)
        return ConflictCode::ParentMismatch;
    return std::nullopt;
}

const DeviceParameterTarget& device_target(const AutomationLane& lane) noexcept {
    return std::get<DeviceParameterTarget>(lane.target());
}

std::vector<std::pair<ItemId, ItemKind>> owned_identities(const AutomationLane& lane) {
    std::vector<std::pair<ItemId, ItemKind>> result{{lane.id(), ItemKind::AutomationLane}};
    result.reserve(1 + lane.curve().points().size());
    for (const auto& point : lane.curve().points())
        result.emplace_back(point.id, ItemKind::AutomationPoint);
    return result;
}

runtime::Result<AutomationCommandReduction, TransactionError>
reduce_insert(const Project& project, const InsertAutomationLane& insert,
              const Transaction& transaction, CommandId command, bool allow_tombstone_restore) {
    if (const auto code =
            target_error(project, insert.sequence_id, ItemKind::Sequence, insert.sequence_id))
        return reject(*code, transaction, command, insert.sequence_id);
    if (const auto code = target_error(project, insert.track_id, ItemKind::Track,
                                       insert.sequence_id, insert.track_id))
        return reject(*code, transaction, command, insert.track_id, insert.sequence_id);
    const auto placement_id = device_target(insert.lane).device_placement_id();
    if (const auto code = target_error(project, placement_id, ItemKind::DevicePlacement,
                                       insert.sequence_id, insert.track_id))
        return reject(*code, transaction, command, placement_id, insert.track_id);

    auto identities = owned_identities(insert.lane);
    auto ordered = identities;
    std::sort(ordered.begin(), ordered.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });
    if (const auto duplicate =
            std::adjacent_find(ordered.begin(), ordered.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.first == rhs.first;
            });
        duplicate != ordered.end())
        return reject(ConflictCode::IdentityNotAvailable, transaction, command, duplicate->first);

    std::vector<IdentityMutation> identity_changes;
    identity_changes.reserve(identities.size());
    std::uint64_t next = project.next_item_id();
    for (const auto [id, kind] : identities) {
        const ItemLocation wanted{kind, insert.sequence_id, insert.track_id, {}, true,
                                  insert.lane.id()};
        const auto existing = project.locate(id);
        if (allow_tombstone_restore && existing) {
            if (existing->active || existing->kind != wanted.kind ||
                existing->sequence_id != wanted.sequence_id ||
                existing->track_id != wanted.track_id ||
                existing->clip_id != wanted.clip_id ||
                existing->automation_lane_id != wanted.automation_lane_id)
                return reject(ConflictCode::IdentityNotAvailable, transaction, command, id);
            identity_changes.push_back({IdentityMutationKind::Reactivate, id, wanted});
        } else {
            if (existing || id.value < project.next_item_id())
                return reject(ConflictCode::IdentityNotAvailable, transaction, command, id);
            identity_changes.push_back({IdentityMutationKind::Insert, id, wanted});
            next = std::max(next, id.value == std::numeric_limits<std::uint64_t>::max() - 1
                                      ? std::numeric_limits<std::uint64_t>::max()
                                      : id.value + 1);
        }
    }

    const auto* sequence = project.find_sequence(insert.sequence_id);
    const auto* track = sequence->find_track(insert.track_id);
    auto next_track = track->insert_automation_lane(insert.lane);
    if (!next_track)
        return model_failure(transaction, command, next_track.error());
    auto next_sequence = sequence->replace_track(std::move(next_track).value());
    if (!next_sequence)
        return model_failure(transaction, command, next_sequence.error());
    auto next_project = ProjectEditAccess::replace_sequence(
        project, std::move(next_sequence).value(), identity_changes, next);
    if (!next_project)
        return model_failure(transaction, command, next_project.error());

    return runtime::Ok(AutomationCommandReduction{
        std::move(next_project).value(),
        RemoveAutomationLane{insert.sequence_id, insert.track_id, insert.lane.id()},
        {insert.lane.id(), insert.track_id, insert.sequence_id,
         DirtyFlags::Structure | DirtyFlags::Automation | DirtyFlags::Added}});
}

runtime::Result<AutomationCommandReduction, TransactionError>
reduce_remove(const Project& project, const RemoveAutomationLane& remove,
              const Transaction& transaction, CommandId command) {
    if (const auto code = target_error(project, remove.lane_id, ItemKind::AutomationLane,
                                       remove.sequence_id, remove.track_id, remove.lane_id))
        return reject(*code, transaction, command, remove.lane_id, remove.track_id);
    const auto* sequence = project.find_sequence(remove.sequence_id);
    const auto* track = sequence ? sequence->find_track(remove.track_id) : nullptr;
    const auto* lane = track ? track->find_automation_lane(remove.lane_id) : nullptr;
    if (!lane)
        return reject(ConflictCode::TargetMissing, transaction, command, remove.lane_id);
    const AutomationLane removed = *lane;

    std::vector<IdentityMutation> identity_changes;
    const auto identities = owned_identities(removed);
    identity_changes.reserve(identities.size());
    for (const auto [id, kind] : identities)
        identity_changes.push_back(
            {IdentityMutationKind::Deactivate,
             id,
             {kind, remove.sequence_id, remove.track_id, {}, false, remove.lane_id}});

    auto next_track = track->erase_automation_lane(remove.lane_id);
    if (!next_track)
        return model_failure(transaction, command, next_track.error());
    auto next_sequence = sequence->replace_track(std::move(next_track).value());
    if (!next_sequence)
        return model_failure(transaction, command, next_sequence.error());
    auto next_project = ProjectEditAccess::replace_sequence(
        project, std::move(next_sequence).value(), identity_changes);
    if (!next_project)
        return model_failure(transaction, command, next_project.error());

    return runtime::Ok(AutomationCommandReduction{
        std::move(next_project).value(),
        InsertAutomationLane{remove.sequence_id, remove.track_id, removed},
        {remove.lane_id, remove.track_id, remove.sequence_id,
         DirtyFlags::Structure | DirtyFlags::Automation | DirtyFlags::Removed}});
}

} // namespace

bool is_automation_command(const Command& command) noexcept {
    return std::holds_alternative<InsertAutomationLane>(command) ||
           std::holds_alternative<RemoveAutomationLane>(command);
}

runtime::Result<AutomationCommandReduction, TransactionError>
reduce_automation_command(const Project& project, const Command& command,
                          const Transaction& transaction, CommandId command_id,
                          bool allow_tombstone_restore) {
    if (const auto* insert = std::get_if<InsertAutomationLane>(&command))
        return reduce_insert(project, *insert, transaction, command_id, allow_tombstone_restore);
    if (const auto* remove = std::get_if<RemoveAutomationLane>(&command))
        return reduce_remove(project, *remove, transaction, command_id);
    return reject(ConflictCode::ModelInvariant, transaction, command_id);
}

} // namespace pulp::timeline::detail
