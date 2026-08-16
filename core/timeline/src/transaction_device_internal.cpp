#include "transaction_device_internal.hpp"

#include "project_state_access.hpp"
#include "transaction_dispatch_internal.hpp"
#include "transaction_reduction_support.hpp"

#include <algorithm>
#include <iterator>
#include <variant>

namespace pulp::timeline::detail {
namespace {

ItemLocation expected_location(const Project& project, ItemId sequence, ItemId track,
                               ItemId device) {
    return ItemLocation{ItemKind::DevicePlacement, track, sequence, track, {}, true};
}

runtime::Result<DeviceCommandReduction, TransactionError>
finish(const Project& project, const Sequence& sequence, Command inverse, ItemId device_id,
       ItemId track_id, DirtyFlags flags, std::span<const IdentityMutation> identities,
       std::optional<std::uint64_t> next_item_id, const Transaction& transaction,
       CommandId command) {
    auto next = ProjectEditAccess::replace_sequence(project, sequence, identities, next_item_id);
    if (!next)
        return runtime::Err(model_failure(transaction, command, next.error()));
    return runtime::Ok(DeviceCommandReduction{
        std::move(next).value(), std::move(inverse),
        {device_id, track_id, sequence.id(), flags}});
}

runtime::Result<const Track*, TransactionError>
target_track(const Project& project, ItemId sequence_id, ItemId track_id,
             const Transaction& transaction, CommandId command) {
    const ItemLocation expected{ItemKind::Track, sequence_id, sequence_id, track_id, {}, true};
    if (const auto code = target_error(project, track_id, expected))
        return reject_reduction<const Track*>(*code, transaction, command, track_id, sequence_id);
    const auto* sequence = project.find_sequence(sequence_id);
    const auto* track = sequence ? sequence->find_track(track_id) : nullptr;
    if (!track)
        return reject_reduction<const Track*>(ConflictCode::TargetMissing, transaction, command,
                                              track_id, sequence_id);
    return runtime::Ok(track);
}

std::optional<ItemId> authored_position(const Track& track, ItemId device_id) {
    const auto devices = track.device_chain();
    const auto placed = std::find_if(devices.begin(), devices.end(),
                                     [device_id](const auto& value) {
                                         return value.id == device_id;
                                     });
    if (placed == devices.end() || std::next(placed) == devices.end())
        return std::nullopt;
    return std::next(placed)->id;
}

runtime::Result<DeviceCommandReduction, TransactionError>
insert_device(const Project& project, const InsertDevice& insert, const Transaction& transaction,
              CommandId command, bool restore) {
    auto track_result = target_track(project, insert.sequence_id, insert.track_id, transaction,
                                     command);
    if (!track_result)
        return runtime::Err(track_result.error());
    if (insert.before_device_id)
        if (const auto code = target_error(
                project, *insert.before_device_id,
                expected_location(project, insert.sequence_id, insert.track_id,
                                  *insert.before_device_id)))
            return reject_reduction<DeviceCommandReduction>(
                *code, transaction, command, *insert.before_device_id, insert.track_id);
    const OwnedIdentity identity{
        insert.placement.id,
        expected_location(project, insert.sequence_id, insert.track_id, insert.placement.id)};
    auto plan = plan_identity_insert(project, std::span<const OwnedIdentity>(&identity, 1), restore,
                                     transaction, command);
    if (!plan)
        return runtime::Err(plan.error());
    auto next_track = track_result.value()->insert_device(insert.placement, insert.before_device_id);
    if (!next_track)
        return runtime::Err(model_failure(transaction, command, next_track.error()));
    const auto* sequence = project.find_sequence(insert.sequence_id);
    auto next_sequence = sequence->replace_track(std::move(next_track).value());
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    return finish(project, next_sequence.value(),
                  RemoveDevice{insert.sequence_id, insert.track_id, insert.placement.id},
                  insert.placement.id, insert.track_id,
                  DirtyFlags::Structure | DirtyFlags::Added, plan->mutations,
                  plan->next_item_id, transaction, command);
}

runtime::Result<DeviceCommandReduction, TransactionError>
remove_device(const Project& project, const RemoveDevice& remove, const Transaction& transaction,
              CommandId command) {
    if (const auto code = target_error(
            project, remove.device_id,
            expected_location(project, remove.sequence_id, remove.track_id, remove.device_id)))
        return reject_reduction<DeviceCommandReduction>(*code, transaction, command,
                                                        remove.device_id, remove.track_id);
    const auto* sequence = project.find_sequence(remove.sequence_id);
    const auto* track = sequence ? sequence->find_track(remove.track_id) : nullptr;
    const auto* placement = track ? track->find_device_placement(remove.device_id) : nullptr;
    if (!placement)
        return reject_reduction<DeviceCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                        command, remove.device_id,
                                                        remove.track_id);
    const DevicePlacement removed = *placement;
    const auto following = authored_position(*track, remove.device_id);
    auto next_track = track->erase_device(remove.device_id);
    if (!next_track)
        return runtime::Err(model_failure(transaction, command, next_track.error()));
    auto next_sequence = sequence->replace_track(std::move(next_track).value());
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    const OwnedIdentity identity{
        remove.device_id,
        expected_location(project, remove.sequence_id, remove.track_id, remove.device_id)};
    const auto mutations =
        plan_identity_deactivate(std::span<const OwnedIdentity>(&identity, 1));
    return finish(project, next_sequence.value(),
                  InsertDevice{remove.sequence_id, remove.track_id, removed, following},
                  remove.device_id, remove.track_id,
                  DirtyFlags::Structure | DirtyFlags::Removed, mutations, std::nullopt,
                  transaction, command);
}

runtime::Result<DeviceCommandReduction, TransactionError>
move_device(const Project& project, const MoveDevice& move, const Transaction& transaction,
            CommandId command) {
    if (const auto code = target_error(
            project, move.device_id,
            expected_location(project, move.sequence_id, move.track_id, move.device_id)))
        return reject_reduction<DeviceCommandReduction>(*code, transaction, command,
                                                        move.device_id, move.track_id);
    if (move.replacement_before_device_id)
        if (const auto code = target_error(
                project, *move.replacement_before_device_id,
                expected_location(project, move.sequence_id, move.track_id,
                                  *move.replacement_before_device_id)))
            return reject_reduction<DeviceCommandReduction>(
                *code, transaction, command, *move.replacement_before_device_id, move.track_id);
    const auto* sequence = project.find_sequence(move.sequence_id);
    const auto* track = sequence ? sequence->find_track(move.track_id) : nullptr;
    if (!track)
        return reject_reduction<DeviceCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                        command, move.device_id, move.track_id);
    if (authored_position(*track, move.device_id) != move.expected_before_device_id)
        return reject_reduction<DeviceCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                        transaction, command, move.device_id);
    auto next_track = track->move_device(move.device_id, move.replacement_before_device_id);
    if (!next_track)
        return runtime::Err(model_failure(transaction, command, next_track.error()));
    auto next_sequence = sequence->replace_track(std::move(next_track).value());
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    return finish(project, next_sequence.value(),
                  MoveDevice{move.sequence_id, move.track_id, move.device_id,
                             move.replacement_before_device_id,
                             move.expected_before_device_id},
                  move.device_id, move.track_id, DirtyFlags::Structure, {}, std::nullopt,
                  transaction, command);
}

runtime::Result<DeviceCommandReduction, TransactionError>
retarget_device(const Project& project, const RetargetDevice& retarget,
                const Transaction& transaction, CommandId command) {
    if (const auto code = target_error(
            project, retarget.device_id,
            expected_location(project, retarget.sequence_id, retarget.track_id,
                              retarget.device_id)))
        return reject_reduction<DeviceCommandReduction>(*code, transaction, command,
                                                        retarget.device_id, retarget.track_id);
    const auto* sequence = project.find_sequence(retarget.sequence_id);
    const auto* track = sequence ? sequence->find_track(retarget.track_id) : nullptr;
    const auto* current = track ? track->find_device_placement(retarget.device_id) : nullptr;
    if (!current)
        return reject_reduction<DeviceCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                        command, retarget.device_id);
    if (current->configuration != retarget.expected)
        return reject_reduction<DeviceCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                        transaction, command,
                                                        retarget.device_id);
    auto replacement = *current;
    replacement.configuration = retarget.replacement;
    auto next_track = track->replace_device(std::move(replacement));
    if (!next_track)
        return runtime::Err(model_failure(transaction, command, next_track.error()));
    auto next_sequence = sequence->replace_track(std::move(next_track).value());
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    return finish(project, next_sequence.value(),
                  RetargetDevice{retarget.sequence_id, retarget.track_id, retarget.device_id,
                                 retarget.replacement, retarget.expected},
                  retarget.device_id, retarget.track_id, DirtyFlags::Structure, {}, std::nullopt,
                  transaction, command);
}

runtime::Result<DeviceCommandReduction, TransactionError>
set_device_state(const Project& project, const SetDeviceState& set,
                 const Transaction& transaction, CommandId command) {
    if (const auto code = target_error(
            project, set.device_id,
            expected_location(project, set.sequence_id, set.track_id, set.device_id)))
        return reject_reduction<DeviceCommandReduction>(*code, transaction, command,
                                                        set.device_id, set.track_id);
    const auto* sequence = project.find_sequence(set.sequence_id);
    const auto* track = sequence ? sequence->find_track(set.track_id) : nullptr;
    const auto* current = track ? track->find_device_placement(set.device_id) : nullptr;
    if (!current)
        return reject_reduction<DeviceCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                        command, set.device_id);
    if (current->state_ref != set.expected)
        return reject_reduction<DeviceCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                        transaction, command, set.device_id);
    auto replacement = *current;
    replacement.state_ref = set.replacement;
    auto next_track = track->replace_device(std::move(replacement));
    if (!next_track)
        return runtime::Err(model_failure(transaction, command, next_track.error()));
    auto next_sequence = sequence->replace_track(std::move(next_track).value());
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    return finish(project, next_sequence.value(),
                  SetDeviceState{set.sequence_id, set.track_id, set.device_id, set.replacement,
                                 set.expected},
                  set.device_id, set.track_id, DirtyFlags::Content, {}, std::nullopt,
                  transaction, command);
}

} // namespace

bool is_device_command(const Command& command) noexcept {
    return std::visit([]<typename T>(const T&) { return is_device_command_type<T>; }, command);
}

runtime::Result<DeviceCommandReduction, TransactionError>
reduce_device_command(const Project& project, const Command& command,
                      const Transaction& transaction, CommandId command_id,
                      bool allow_tombstone_restore) {
    return std::visit(
        [&]<typename T>(const T& value)
            -> runtime::Result<DeviceCommandReduction, TransactionError> {
            if constexpr (std::is_same_v<T, InsertDevice>)
                return insert_device(project, value, transaction, command_id,
                                     allow_tombstone_restore);
            else if constexpr (std::is_same_v<T, RemoveDevice>)
                return remove_device(project, value, transaction, command_id);
            else if constexpr (std::is_same_v<T, MoveDevice>)
                return move_device(project, value, transaction, command_id);
            else if constexpr (std::is_same_v<T, RetargetDevice>)
                return retarget_device(project, value, transaction, command_id);
            else if constexpr (std::is_same_v<T, SetDeviceState>)
                return set_device_state(project, value, transaction, command_id);
            else {
                static_assert(!is_device_command_type<T>,
                              "a command claimed by is_device_command_type has no reducer arm");
                return reject_reduction<DeviceCommandReduction>(ConflictCode::ModelInvariant,
                                                                transaction, command_id);
            }
        },
        command);
}

} // namespace pulp::timeline::detail
