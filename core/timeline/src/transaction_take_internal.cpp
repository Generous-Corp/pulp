#include "transaction_take_internal.hpp"

#include "transaction_reduction_support.hpp"

#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace pulp::timeline::detail {
namespace {

std::vector<OwnedIdentity> owned_identities(const TakeLane& lane, ItemId sequence, ItemId track) {
    // Identity ownership rides parent_id: a take lane's parent is its track, a
    // take's parent is its lane — the same lane-owned exception the model applies
    // to automation points. immediate_parent_id stays the single parent path.
    const auto lane_parent = immediate_parent_id(ItemKind::TakeLane, {}, sequence, track, {});
    const auto take_parent =
        immediate_parent_id(ItemKind::Take, {}, sequence, track, {}, lane.id());
    std::vector<OwnedIdentity> result{
        {lane.id(), ItemLocation{ItemKind::TakeLane, lane_parent, sequence, track, {}, true}}};
    result.reserve(1 + lane.takes().size());
    for (const auto& take : lane.takes())
        result.push_back(
            {take.id(), ItemLocation{ItemKind::Take, take_parent, sequence, track, {}, true}});
    return result;
}

OwnedIdentity owned_identity(const Take& take, ItemId sequence, ItemId track, ItemId lane) {
    return {take.id(),
            ItemLocation{ItemKind::Take,
                         immediate_parent_id(ItemKind::Take, {}, sequence, track, {}, lane),
                         sequence,
                         track,
                         {},
                         true}};
}

// A take references a sealed asset the same way a clip MediaRef does; the recorder
// emits CreateAsset first, so by the time this command reduces the asset exists.
// A missing or out-of-range reference is rejected fail-closed.
std::optional<ModelError> validate_take_media(const Project& project, const Take& take) noexcept {
    const auto& media = take.media();
    const auto* asset = project.find_asset(media.asset_id);
    if (!asset)
        return ModelError{ModelErrorCode::MissingAsset, take.id(), media.asset_id};
    if (media.source_start.value < 0)
        return ModelError{ModelErrorCode::InvalidMediaRange, take.id(), media.asset_id};
    const auto start = static_cast<std::uint64_t>(media.source_start.value);
    if (start > asset->frame_count || media.frame_count > asset->frame_count - start)
        return ModelError{ModelErrorCode::InvalidMediaRange, take.id(), media.asset_id};
    return std::nullopt;
}

runtime::Result<TakeCommandReduction, TransactionError>
reduce_insert(const Project& project, const InsertTakeLane& insert, const Transaction& transaction,
              CommandId command, bool allow_tombstone_restore) {
    if (const auto code =
            target_error(project, insert.sequence_id,
                         ItemLocation{ItemKind::Sequence,
                                      immediate_parent_id(ItemKind::Sequence, project.id(),
                                                          insert.sequence_id, {}, {}),
                                      insert.sequence_id,
                                      {},
                                      {},
                                      true}))
        return reject_reduction<TakeCommandReduction>(*code, transaction, command,
                                                      insert.sequence_id);
    if (const auto code =
            target_error(project, insert.track_id,
                         ItemLocation{ItemKind::Track,
                                      immediate_parent_id(ItemKind::Track, project.id(),
                                                          insert.sequence_id, insert.track_id, {}),
                                      insert.sequence_id,
                                      insert.track_id,
                                      {},
                                      true}))
        return reject_reduction<TakeCommandReduction>(*code, transaction, command, insert.track_id,
                                                      insert.sequence_id);
    for (const auto& take : insert.lane.takes())
        if (const auto media_error = validate_take_media(project, take))
            return runtime::Err(model_failure(transaction, command, *media_error));

    const auto identities = owned_identities(insert.lane, insert.sequence_id, insert.track_id);
    if (const auto duplicate = duplicate_owned_identity(identities))
        return reject_reduction<TakeCommandReduction>(ConflictCode::IdentityNotAvailable,
                                                      transaction, command, *duplicate);
    auto identity_plan =
        plan_identity_insert(project, identities, allow_tombstone_restore, transaction, command);
    if (!identity_plan)
        return runtime::Err(identity_plan.error());

    const auto* sequence = project.find_sequence(insert.sequence_id);
    const auto* track = sequence->find_track(insert.track_id);
    auto next_track = track->insert_take_lane(insert.lane);
    if (!next_track)
        return runtime::Err(model_failure(transaction, command, next_track.error()));
    auto next_sequence = sequence->replace_track(std::move(next_track).value());
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    auto next_project =
        ProjectEditAccess::replace_sequence(project, std::move(next_sequence).value(),
                                            identity_plan->mutations, identity_plan->next_item_id);
    if (!next_project)
        return runtime::Err(model_failure(transaction, command, next_project.error()));

    return runtime::Ok(
        TakeCommandReduction{std::move(next_project).value(),
                             RemoveTakeLane{insert.sequence_id, insert.track_id, insert.lane.id()},
                             {insert.lane.id(), insert.track_id, insert.sequence_id,
                              DirtyFlags::Structure | DirtyFlags::Take | DirtyFlags::Added}});
}

runtime::Result<TakeCommandReduction, TransactionError>
reduce_remove(const Project& project, const RemoveTakeLane& remove, const Transaction& transaction,
              CommandId command) {
    if (const auto code =
            target_error(project, remove.lane_id,
                         ItemLocation{ItemKind::TakeLane,
                                      immediate_parent_id(ItemKind::TakeLane, project.id(),
                                                          remove.sequence_id, remove.track_id, {}),
                                      remove.sequence_id,
                                      remove.track_id,
                                      {},
                                      true}))
        return reject_reduction<TakeCommandReduction>(*code, transaction, command, remove.lane_id,
                                                      remove.track_id);
    const auto* sequence = project.find_sequence(remove.sequence_id);
    const auto* track = sequence ? sequence->find_track(remove.track_id) : nullptr;
    const auto* lane = track ? track->find_take_lane(remove.lane_id) : nullptr;
    if (!lane)
        return reject_reduction<TakeCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                      command, remove.lane_id);
    if (track->active_take_lane_id() == remove.lane_id)
        return reject_reduction<TakeCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                      transaction, command, remove.lane_id,
                                                      remove.track_id);
    const TakeLane removed = *lane;

    const auto identities = owned_identities(removed, remove.sequence_id, remove.track_id);
    const auto identity_changes = plan_identity_deactivate(identities);

    auto next_track = track->erase_take_lane(remove.lane_id);
    if (!next_track)
        return runtime::Err(model_failure(transaction, command, next_track.error()));
    auto next_sequence = sequence->replace_track(std::move(next_track).value());
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    auto next_project = ProjectEditAccess::replace_sequence(
        project, std::move(next_sequence).value(), identity_changes);
    if (!next_project)
        return runtime::Err(model_failure(transaction, command, next_project.error()));

    return runtime::Ok(
        TakeCommandReduction{std::move(next_project).value(),
                             InsertTakeLane{remove.sequence_id, remove.track_id, removed},
                             {remove.lane_id, remove.track_id, remove.sequence_id,
                              DirtyFlags::Structure | DirtyFlags::Take | DirtyFlags::Removed}});
}

runtime::Result<TakeCommandReduction, TransactionError>
reduce_insert_take(const Project& project, const InsertTake& insert, const Transaction& transaction,
                   CommandId command, bool allow_tombstone_restore) {
    if (const auto code =
            target_error(project, insert.lane_id,
                         ItemLocation{ItemKind::TakeLane,
                                      immediate_parent_id(ItemKind::TakeLane, project.id(),
                                                          insert.sequence_id, insert.track_id, {}),
                                      insert.sequence_id,
                                      insert.track_id,
                                      {},
                                      true}))
        return reject_reduction<TakeCommandReduction>(*code, transaction, command, insert.lane_id,
                                                      insert.track_id);
    if (const auto media_error = validate_take_media(project, insert.take))
        return runtime::Err(model_failure(transaction, command, *media_error));

    const std::array identity{
        owned_identity(insert.take, insert.sequence_id, insert.track_id, insert.lane_id)};
    auto identity_plan =
        plan_identity_insert(project, identity, allow_tombstone_restore, transaction, command);
    if (!identity_plan)
        return runtime::Err(identity_plan.error());

    const auto* sequence = project.find_sequence(insert.sequence_id);
    const auto* track = sequence->find_track(insert.track_id);
    auto next_track = track->insert_take(insert.lane_id, insert.take);
    if (!next_track)
        return runtime::Err(model_failure(transaction, command, next_track.error()));
    auto next_sequence = sequence->replace_track(std::move(next_track).value());
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    auto next_project =
        ProjectEditAccess::replace_sequence(project, std::move(next_sequence).value(),
                                            identity_plan->mutations, identity_plan->next_item_id);
    if (!next_project)
        return runtime::Err(model_failure(transaction, command, next_project.error()));

    return runtime::Ok(TakeCommandReduction{
        std::move(next_project).value(),
        RemoveTake{insert.sequence_id, insert.track_id, insert.lane_id, insert.take.id()},
        {insert.take.id(), insert.track_id, insert.sequence_id,
         DirtyFlags::Structure | DirtyFlags::Take | DirtyFlags::Added}});
}

runtime::Result<TakeCommandReduction, TransactionError>
reduce_remove_take(const Project& project, const RemoveTake& remove, const Transaction& transaction,
                   CommandId command) {
    if (const auto code = target_error(
            project, remove.take_id,
            ItemLocation{ItemKind::Take,
                         immediate_parent_id(ItemKind::Take, project.id(), remove.sequence_id,
                                             remove.track_id, {}, remove.lane_id),
                         remove.sequence_id,
                         remove.track_id,
                         {},
                         true}))
        return reject_reduction<TakeCommandReduction>(*code, transaction, command, remove.take_id,
                                                      remove.lane_id);
    const auto* sequence = project.find_sequence(remove.sequence_id);
    const auto* track = sequence ? sequence->find_track(remove.track_id) : nullptr;
    const auto* lane = track ? track->find_take_lane(remove.lane_id) : nullptr;
    const auto* take = lane ? lane->find_take(remove.take_id) : nullptr;
    if (!take)
        return reject_reduction<TakeCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                      command, remove.take_id, remove.lane_id);
    const Take removed = *take;

    const std::array identity{
        owned_identity(removed, remove.sequence_id, remove.track_id, remove.lane_id)};
    const auto identity_changes = plan_identity_deactivate(identity);
    auto next_track = track->erase_take(remove.lane_id, remove.take_id);
    if (!next_track)
        return runtime::Err(model_failure(transaction, command, next_track.error()));
    auto next_sequence = sequence->replace_track(std::move(next_track).value());
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    auto next_project = ProjectEditAccess::replace_sequence(
        project, std::move(next_sequence).value(), identity_changes);
    if (!next_project)
        return runtime::Err(model_failure(transaction, command, next_project.error()));

    return runtime::Ok(TakeCommandReduction{
        std::move(next_project).value(),
        InsertTake{remove.sequence_id, remove.track_id, remove.lane_id, removed},
        {remove.take_id, remove.track_id, remove.sequence_id,
         DirtyFlags::Structure | DirtyFlags::Take | DirtyFlags::Removed}});
}

runtime::Result<TakeCommandReduction, TransactionError>
reduce_set_record_arm(const Project& project, const SetRecordArm& set,
                      const Transaction& transaction, CommandId command) {
    if (const auto code =
            target_error(project, set.track_id,
                         ItemLocation{ItemKind::Track,
                                      immediate_parent_id(ItemKind::Track, project.id(),
                                                          set.sequence_id, set.track_id, {}),
                                      set.sequence_id,
                                      set.track_id,
                                      {},
                                      true}))
        return reject_reduction<TakeCommandReduction>(*code, transaction, command, set.track_id,
                                                      set.sequence_id);
    const auto* sequence = project.find_sequence(set.sequence_id);
    const auto* track = sequence->find_track(set.track_id);
    // Optimistic gate: the caller's expected value must match the current arming
    // so a concurrent writer cannot silently clobber it.
    if (track->record_armed() != set.expected)
        return reject_reduction<TakeCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                      transaction, command, set.track_id);

    auto next_sequence = sequence->replace_track(track->with_record_armed(set.replacement));
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    auto next_project =
        ProjectEditAccess::replace_sequence(project, std::move(next_sequence).value());
    if (!next_project)
        return runtime::Err(model_failure(transaction, command, next_project.error()));

    return runtime::Ok(TakeCommandReduction{
        std::move(next_project).value(),
        SetRecordArm{set.sequence_id, set.track_id, set.replacement, set.expected},
        {set.track_id, set.track_id, set.sequence_id, DirtyFlags::Take}});
}

runtime::Result<TakeCommandReduction, TransactionError>
reduce_set_active_take_lane(const Project& project, const SetActiveTakeLane& set,
                            const Transaction& transaction, CommandId command) {
    if ((set.expected_lane_id.value != 0 && !set.expected_lane_id.valid()) ||
        (set.replacement_lane_id.value != 0 && !set.replacement_lane_id.valid()))
        return reject_reduction<TakeCommandReduction>(
            ConflictCode::InvalidIdentifier, transaction, command,
            set.replacement_lane_id.value != 0 && !set.replacement_lane_id.valid()
                ? set.replacement_lane_id
                : set.expected_lane_id,
            set.track_id);
    if (const auto code =
            target_error(project, set.track_id,
                         ItemLocation{ItemKind::Track,
                                      immediate_parent_id(ItemKind::Track, project.id(),
                                                          set.sequence_id, set.track_id, {}),
                                      set.sequence_id,
                                      set.track_id,
                                      {},
                                      true}))
        return reject_reduction<TakeCommandReduction>(*code, transaction, command, set.track_id,
                                                      set.sequence_id);
    const auto* sequence = project.find_sequence(set.sequence_id);
    const auto* track = sequence->find_track(set.track_id);
    if (track->active_take_lane_id() != set.expected_lane_id)
        return reject_reduction<TakeCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                      transaction, command, set.track_id,
                                                      set.expected_lane_id);
    if (set.replacement_lane_id.valid()) {
        if (const auto code =
                target_error(project, set.replacement_lane_id,
                             ItemLocation{ItemKind::TakeLane,
                                          immediate_parent_id(ItemKind::TakeLane, project.id(),
                                                              set.sequence_id, set.track_id, {}),
                                          set.sequence_id,
                                          set.track_id,
                                          {},
                                          true}))
            return reject_reduction<TakeCommandReduction>(*code, transaction, command,
                                                          set.replacement_lane_id, set.track_id);
    }

    auto next_track = track->with_active_take_lane(set.replacement_lane_id);
    if (!next_track)
        return runtime::Err(model_failure(transaction, command, next_track.error()));
    auto next_sequence = sequence->replace_track(std::move(next_track).value());
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    auto next_project =
        ProjectEditAccess::replace_sequence(project, std::move(next_sequence).value());
    if (!next_project)
        return runtime::Err(model_failure(transaction, command, next_project.error()));

    return runtime::Ok(
        TakeCommandReduction{std::move(next_project).value(),
                             SetActiveTakeLane{set.sequence_id, set.track_id,
                                               set.replacement_lane_id, set.expected_lane_id},
                             {set.track_id, set.track_id, set.sequence_id, DirtyFlags::Take}});
}

} // namespace

bool is_take_command(const Command& command) noexcept {
    return std::holds_alternative<InsertTakeLane>(command) ||
           std::holds_alternative<RemoveTakeLane>(command) ||
           std::holds_alternative<SetRecordArm>(command) ||
           std::holds_alternative<InsertTake>(command) ||
           std::holds_alternative<RemoveTake>(command) ||
           std::holds_alternative<SetActiveTakeLane>(command);
}

runtime::Result<TakeCommandReduction, TransactionError>
reduce_take_command(const Project& project, const Command& command, const Transaction& transaction,
                    CommandId command_id, bool allow_tombstone_restore) {
    if (const auto* insert = std::get_if<InsertTakeLane>(&command))
        return reduce_insert(project, *insert, transaction, command_id, allow_tombstone_restore);
    if (const auto* remove = std::get_if<RemoveTakeLane>(&command))
        return reduce_remove(project, *remove, transaction, command_id);
    if (const auto* arm = std::get_if<SetRecordArm>(&command))
        return reduce_set_record_arm(project, *arm, transaction, command_id);
    if (const auto* insert = std::get_if<InsertTake>(&command))
        return reduce_insert_take(project, *insert, transaction, command_id,
                                  allow_tombstone_restore);
    if (const auto* remove = std::get_if<RemoveTake>(&command))
        return reduce_remove_take(project, *remove, transaction, command_id);
    if (const auto* selection = std::get_if<SetActiveTakeLane>(&command))
        return reduce_set_active_take_lane(project, *selection, transaction, command_id);
    return reject_reduction<TakeCommandReduction>(ConflictCode::ModelInvariant, transaction,
                                                  command_id);
}

} // namespace pulp::timeline::detail
