#include "transaction_automation_internal.hpp"

#include "transaction_reduction_support.hpp"

#include <utility>
#include <vector>

namespace pulp::timeline::detail {
namespace {

const DeviceParameterTarget& device_target(const AutomationLane& lane) noexcept {
    return std::get<DeviceParameterTarget>(lane.target());
}

std::vector<OwnedIdentity> owned_identities(const AutomationLane& lane, ItemId sequence,
                                            ItemId track) {
    // Identity ownership rides parent_id: a lane's parent is its track, a point's
    // parent is its lane. immediate_parent_id stays the single parent path.
    const auto lane_parent = immediate_parent_id(ItemKind::AutomationLane, {}, sequence, track, {});
    const auto point_parent =
        immediate_parent_id(ItemKind::AutomationPoint, {}, sequence, track, {}, lane.id());
    std::vector<OwnedIdentity> result{
        {lane.id(), ItemLocation{ItemKind::AutomationLane, lane_parent, sequence, track, {}, true}}};
    result.reserve(1 + lane.curve().points().size());
    for (const auto& point : lane.curve().points())
        result.push_back({point.id, ItemLocation{ItemKind::AutomationPoint, point_parent, sequence,
                                                 track, {}, true}});
    return result;
}

runtime::Result<AutomationCommandReduction, TransactionError>
reduce_insert(const Project& project, const InsertAutomationLane& insert,
              const Transaction& transaction, CommandId command, bool allow_tombstone_restore) {
    if (const auto code = target_error(
            project, insert.sequence_id,
            ItemLocation{ItemKind::Sequence,
                         immediate_parent_id(ItemKind::Sequence, project.id(), insert.sequence_id,
                                             {}, {}),
                         insert.sequence_id, {}, {}, true}))
        return reject_reduction<AutomationCommandReduction>(
            *code, transaction, command, insert.sequence_id);
    if (const auto code = target_error(
            project, insert.track_id,
            ItemLocation{ItemKind::Track,
                         immediate_parent_id(ItemKind::Track, project.id(), insert.sequence_id,
                                             insert.track_id, {}),
                         insert.sequence_id, insert.track_id, {}, true}))
        return reject_reduction<AutomationCommandReduction>(
            *code, transaction, command, insert.track_id, insert.sequence_id);
    const auto placement_id = device_target(insert.lane).device_placement_id;
    if (const auto code = target_error(
            project, placement_id,
            ItemLocation{ItemKind::DevicePlacement,
                         immediate_parent_id(ItemKind::DevicePlacement, project.id(),
                                             insert.sequence_id, insert.track_id, {}),
                         insert.sequence_id, insert.track_id, {}, true}))
        return reject_reduction<AutomationCommandReduction>(
            *code, transaction, command, placement_id, insert.track_id);

    const auto identities = owned_identities(insert.lane, insert.sequence_id, insert.track_id);
    if (const auto duplicate = duplicate_owned_identity(identities))
        return reject_reduction<AutomationCommandReduction>(
            ConflictCode::IdentityNotAvailable, transaction, command, *duplicate);
    auto identity_plan = plan_identity_insert(project, identities, allow_tombstone_restore,
                                              transaction, command);
    if (!identity_plan)
        return runtime::Err(identity_plan.error());

    const auto* sequence = project.find_sequence(insert.sequence_id);
    const auto* track = sequence->find_track(insert.track_id);
    auto next_track = track->insert_automation_lane(insert.lane);
    if (!next_track)
        return runtime::Err(model_failure(transaction, command, next_track.error()));
    auto next_sequence = sequence->replace_track(std::move(next_track).value());
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    auto next_project = ProjectEditAccess::replace_sequence(
        project, std::move(next_sequence).value(), identity_plan->mutations,
        identity_plan->next_item_id);
    if (!next_project)
        return runtime::Err(model_failure(transaction, command, next_project.error()));

    return runtime::Ok(AutomationCommandReduction{
        std::move(next_project).value(),
        RemoveAutomationLane{insert.sequence_id, insert.track_id, insert.lane.id()},
        {insert.lane.id(), insert.track_id, insert.sequence_id,
         DirtyFlags::Structure | DirtyFlags::Automation | DirtyFlags::Added}});
}

runtime::Result<AutomationCommandReduction, TransactionError>
reduce_remove(const Project& project, const RemoveAutomationLane& remove,
              const Transaction& transaction, CommandId command) {
    if (const auto code = target_error(
            project, remove.lane_id,
            ItemLocation{ItemKind::AutomationLane,
                         immediate_parent_id(ItemKind::AutomationLane, project.id(),
                                             remove.sequence_id, remove.track_id, {}),
                         remove.sequence_id, remove.track_id, {}, true}))
        return reject_reduction<AutomationCommandReduction>(
            *code, transaction, command, remove.lane_id, remove.track_id);
    const auto* sequence = project.find_sequence(remove.sequence_id);
    const auto* track = sequence ? sequence->find_track(remove.track_id) : nullptr;
    const auto* lane = track ? track->find_automation_lane(remove.lane_id) : nullptr;
    if (!lane)
        return reject_reduction<AutomationCommandReduction>(
            ConflictCode::TargetMissing, transaction, command, remove.lane_id);
    const AutomationLane removed = *lane;

    const auto identities = owned_identities(removed, remove.sequence_id, remove.track_id);
    const auto identity_changes = plan_identity_deactivate(identities);

    auto next_track = track->erase_automation_lane(remove.lane_id);
    if (!next_track)
        return runtime::Err(model_failure(transaction, command, next_track.error()));
    auto next_sequence = sequence->replace_track(std::move(next_track).value());
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    auto next_project = ProjectEditAccess::replace_sequence(
        project, std::move(next_sequence).value(), identity_changes);
    if (!next_project)
        return runtime::Err(model_failure(transaction, command, next_project.error()));

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
    return reject_reduction<AutomationCommandReduction>(ConflictCode::ModelInvariant, transaction,
                                                        command_id);
}

} // namespace pulp::timeline::detail
