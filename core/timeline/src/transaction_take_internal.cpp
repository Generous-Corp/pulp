#include "transaction_take_internal.hpp"

#include "media_reference_validation.hpp"
#include "transaction_dispatch_internal.hpp"
#include "transaction_reduction_support.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>
#include <variant>
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
        if (const auto media_error =
                validate_media_reference(project, take.media(), take.id()))
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
    if (const auto media_error = validate_media_reference(
            project, insert.take.media(), insert.take.id()))
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

runtime::Result<TakeCommandReduction, TransactionError>
reduce_set_take_comp(const Project& project, const SetTakeComp& set,
                     const Transaction& transaction, CommandId command) {
    if (const auto code =
            target_error(project, set.lane_id,
                         ItemLocation{ItemKind::TakeLane,
                                      immediate_parent_id(ItemKind::TakeLane, project.id(),
                                                          set.sequence_id, set.track_id, {}),
                                      set.sequence_id,
                                      set.track_id,
                                      {},
                                      true}))
        return reject_reduction<TakeCommandReduction>(*code, transaction, command, set.lane_id,
                                                      set.track_id);
    const auto* sequence = project.find_sequence(set.sequence_id);
    const auto* track = sequence ? sequence->find_track(set.track_id) : nullptr;
    const auto* lane = track ? track->find_take_lane(set.lane_id) : nullptr;
    if (!lane)
        return reject_reduction<TakeCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                      command, set.lane_id, set.track_id);
    const auto current = lane->comp_segments();
    if (current.size() != set.expected.size() ||
        !std::equal(current.begin(), current.end(), set.expected.begin()))
        return reject_reduction<TakeCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                      transaction, command, set.lane_id);

    auto next_track = track->with_take_comp(set.lane_id, set.replacement);
    if (!next_track)
        return runtime::Err(model_failure(transaction, command, next_track.error()));
    const auto canonical_comp = next_track.value().find_take_lane(set.lane_id)->comp_segments();
    std::vector<TakeCompSegment> inverse_expected(canonical_comp.begin(), canonical_comp.end());
    auto next_sequence = sequence->replace_track(std::move(next_track).value());
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    auto next_project =
        ProjectEditAccess::replace_sequence(project, std::move(next_sequence).value());
    if (!next_project)
        return runtime::Err(model_failure(transaction, command, next_project.error()));
    return runtime::Ok(TakeCommandReduction{
        std::move(next_project).value(),
        SetTakeComp{set.sequence_id, set.track_id, set.lane_id, std::move(inverse_expected),
                    set.expected},
        {set.lane_id, set.track_id, set.sequence_id, DirtyFlags::Take}});
}

} // namespace

bool is_take_command(const Command& command) noexcept {
    return std::visit([]<typename T>(const T&) { return is_take_command_type<T>; }, command);
}

runtime::Result<TakeCommandReduction, TransactionError>
reduce_take_command(const Project& project, const Command& command, const Transaction& transaction,
                    CommandId command_id, bool allow_tombstone_restore) {
    // The family predicate above this call and the arms below it are two
    // statements of the same list, and a chain of get_if proves nothing about
    // its own coverage. Visiting puts a claimed-but-unhandled command in front
    // of the compiler, which is where the outer dispatch already resolves it.
    return std::visit(
        [&]<typename T>(const T& value) -> runtime::Result<TakeCommandReduction, TransactionError> {
            if constexpr (std::is_same_v<T, InsertTakeLane>)
                return reduce_insert(project, value, transaction, command_id,
                                     allow_tombstone_restore);
            else if constexpr (std::is_same_v<T, RemoveTakeLane>)
                return reduce_remove(project, value, transaction, command_id);
            else if constexpr (std::is_same_v<T, SetRecordArm>)
                return reduce_set_record_arm(project, value, transaction, command_id);
            else if constexpr (std::is_same_v<T, InsertTake>)
                return reduce_insert_take(project, value, transaction, command_id,
                                          allow_tombstone_restore);
            else if constexpr (std::is_same_v<T, RemoveTake>)
                return reduce_remove_take(project, value, transaction, command_id);
            else if constexpr (std::is_same_v<T, SetActiveTakeLane>)
                return reduce_set_active_take_lane(project, value, transaction, command_id);
            else if constexpr (std::is_same_v<T, SetTakeComp>)
                return reduce_set_take_comp(project, value, transaction, command_id);
            else {
                static_assert(!is_take_command_type<T>,
                              "a command claimed by is_take_command_type in "
                              "transaction_dispatch_internal.hpp has no arm here; add "
                              "one, or drop it from the claim list");
                // Reached only by an alternative no family claims, which the
                // caller's predicate already excludes. Kept as a rejection rather
                // than std::unreachable(): this TU is -fno-exceptions, so being
                // wrong here would abort the process, not fail one transaction.
                return reject_reduction<TakeCommandReduction>(ConflictCode::ModelInvariant,
                                                              transaction, command_id);
            }
        },
        command);
}

} // namespace pulp::timeline::detail
