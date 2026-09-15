#include "transaction_modulation_internal.hpp"

#include "transaction_dispatch_internal.hpp"
#include "transaction_reduction_support.hpp"

#include <array>
#include <optional>
#include <utility>
#include <variant>

namespace pulp::timeline::detail {
namespace {

ItemLocation sequence_location(const Project& project, ItemId sequence) {
    return ItemLocation{ItemKind::Sequence,
                        immediate_parent_id(ItemKind::Sequence, project.id(), sequence, {}, {}),
                        sequence,
                        {},
                        {},
                        true};
}

ItemLocation track_location(const Project& project, ItemId sequence, ItemId track) {
    return ItemLocation{ItemKind::Track,
                        immediate_parent_id(ItemKind::Track, project.id(), sequence, track, {}),
                        sequence,
                        track,
                        {},
                        true};
}

// A modulator, a macro, and a route are all parented by their track, so one
// builder serves all three and the kind is the only thing that varies. A route
// is parented by the track rather than by the source it reads, so that removing
// a source is not an ownership question the document answers twice.
ItemLocation owned_location(const Project& project, ItemKind kind, ItemId sequence, ItemId track) {
    return ItemLocation{kind,     immediate_parent_id(kind, project.id(), sequence, track, {}),
                        sequence, track,
                        {},       true};
}

OwnedIdentity owned_identity(ItemKind kind, ItemId item, ItemId sequence, ItemId track) {
    return {
        item,
        ItemLocation{
            kind, immediate_parent_id(kind, {}, sequence, track, {}), sequence, track, {}, true}};
}

// Every arm below ends the same way: edit the track, put it back in its
// sequence, put the sequence back in the project, and report one dirty item.
// Stating it once keeps a missed identity argument from being a per-arm bug.
template <typename EditFn>
runtime::Result<ModulationCommandReduction, TransactionError>
apply_track_edit(const Project& project, ItemId sequence_id, ItemId track_id, ItemId item,
                 DirtyFlags flags, std::span<const IdentityMutation> identities,
                 std::optional<std::uint64_t> next_item_id, Command inverse,
                 const Transaction& transaction, CommandId command, EditFn&& edit) {
    const auto* sequence = project.find_sequence(sequence_id);
    const auto* track = sequence ? sequence->find_track(track_id) : nullptr;
    if (!track)
        return reject_reduction<ModulationCommandReduction>(
            ConflictCode::TargetMissing, transaction, command, track_id, sequence_id);
    auto next_track = edit(*track);
    if (!next_track)
        return runtime::Err(model_failure(transaction, command, next_track.error()));
    auto next_sequence = sequence->replace_track(std::move(next_track).value());
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    auto next_project = ProjectEditAccess::replace_sequence(
        project, std::move(next_sequence).value(), identities, next_item_id);
    if (!next_project)
        return runtime::Err(model_failure(transaction, command, next_project.error()));
    return runtime::Ok(ModulationCommandReduction{
        std::move(next_project).value(), std::move(inverse), {item, track_id, sequence_id, flags}});
}

// Insert differs between the three collections only in the kind it registers,
// the model edit it runs, and the inverse it names.
template <typename Item, typename EditFn>
runtime::Result<ModulationCommandReduction, TransactionError>
reduce_insert_item(const Project& project, ItemKind kind, ItemId sequence_id, ItemId track_id,
                   const Item& item, Command inverse, const Transaction& transaction,
                   CommandId command, bool allow_tombstone_restore, EditFn&& edit) {
    if (const auto code =
            target_error(project, sequence_id, sequence_location(project, sequence_id)))
        return reject_reduction<ModulationCommandReduction>(*code, transaction, command,
                                                            sequence_id);
    if (const auto code =
            target_error(project, track_id, track_location(project, sequence_id, track_id)))
        return reject_reduction<ModulationCommandReduction>(*code, transaction, command, track_id,
                                                            sequence_id);
    const std::array identity{owned_identity(kind, item.id, sequence_id, track_id)};
    auto identity_plan =
        plan_identity_insert(project, identity, allow_tombstone_restore, transaction, command);
    if (!identity_plan)
        return runtime::Err(identity_plan.error());
    return apply_track_edit(project, sequence_id, track_id, item.id,
                            DirtyFlags::Structure | DirtyFlags::Content | DirtyFlags::Added,
                            identity_plan->mutations, identity_plan->next_item_id,
                            std::move(inverse), transaction, command, std::forward<EditFn>(edit));
}

runtime::Result<ModulationCommandReduction, TransactionError>
reduce_insert_modulator(const Project& project, const InsertModulator& insert,
                        const Transaction& transaction, CommandId command,
                        bool allow_tombstone_restore) {
    return reduce_insert_item(
        project, ItemKind::Modulator, insert.sequence_id, insert.track_id, insert.modulator,
        RemoveModulator{insert.sequence_id, insert.track_id, insert.modulator.id}, transaction,
        command, allow_tombstone_restore,
        [&](const Track& track) { return track.insert_modulator(insert.modulator); });
}

runtime::Result<ModulationCommandReduction, TransactionError>
reduce_insert_macro(const Project& project, const InsertMacro& insert,
                    const Transaction& transaction, CommandId command,
                    bool allow_tombstone_restore) {
    return reduce_insert_item(project, ItemKind::MacroControl, insert.sequence_id, insert.track_id,
                              insert.macro,
                              RemoveMacro{insert.sequence_id, insert.track_id, insert.macro.id},
                              transaction, command, allow_tombstone_restore,
                              [&](const Track& track) { return track.insert_macro(insert.macro); });
}

runtime::Result<ModulationCommandReduction, TransactionError>
reduce_remove_modulator(const Project& project, const RemoveModulator& remove,
                        const Transaction& transaction, CommandId command) {
    if (const auto code = target_error(
            project, remove.modulator_id,
            owned_location(project, ItemKind::Modulator, remove.sequence_id, remove.track_id)))
        return reject_reduction<ModulationCommandReduction>(*code, transaction, command,
                                                            remove.modulator_id, remove.track_id);
    const auto* sequence = project.find_sequence(remove.sequence_id);
    const auto* track = sequence ? sequence->find_track(remove.track_id) : nullptr;
    const auto* modulator = track ? track->find_modulator(remove.modulator_id) : nullptr;
    if (!modulator)
        return reject_reduction<ModulationCommandReduction>(
            ConflictCode::TargetMissing, transaction, command, remove.modulator_id);
    // Captured whole before the edit: the inverse restores the declaration as
    // well as the identity, so undo is exact rather than reconstructed.
    const Modulator removed = *modulator;
    const std::array identity{owned_identity(ItemKind::Modulator, remove.modulator_id,
                                             remove.sequence_id, remove.track_id)};
    return apply_track_edit(
        project, remove.sequence_id, remove.track_id, remove.modulator_id,
        DirtyFlags::Structure | DirtyFlags::Content | DirtyFlags::Removed,
        plan_identity_deactivate(identity), std::nullopt,
        InsertModulator{remove.sequence_id, remove.track_id, removed}, transaction, command,
        [&](const Track& current) { return current.erase_modulator(remove.modulator_id); });
}

runtime::Result<ModulationCommandReduction, TransactionError>
reduce_remove_macro(const Project& project, const RemoveMacro& remove,
                    const Transaction& transaction, CommandId command) {
    if (const auto code = target_error(
            project, remove.macro_id,
            owned_location(project, ItemKind::MacroControl, remove.sequence_id, remove.track_id)))
        return reject_reduction<ModulationCommandReduction>(*code, transaction, command,
                                                            remove.macro_id, remove.track_id);
    const auto* sequence = project.find_sequence(remove.sequence_id);
    const auto* track = sequence ? sequence->find_track(remove.track_id) : nullptr;
    const auto* macro = track ? track->find_macro(remove.macro_id) : nullptr;
    if (!macro)
        return reject_reduction<ModulationCommandReduction>(ConflictCode::TargetMissing,
                                                            transaction, command, remove.macro_id);
    const MacroControl removed = *macro;
    const std::array identity{owned_identity(ItemKind::MacroControl, remove.macro_id,
                                             remove.sequence_id, remove.track_id)};
    return apply_track_edit(
        project, remove.sequence_id, remove.track_id, remove.macro_id,
        DirtyFlags::Structure | DirtyFlags::Content | DirtyFlags::Removed,
        plan_identity_deactivate(identity), std::nullopt,
        InsertMacro{remove.sequence_id, remove.track_id, removed}, transaction, command,
        [&](const Track& current) { return current.erase_macro(remove.macro_id); });
}

runtime::Result<ModulationCommandReduction, TransactionError>
reduce_set_modulator(const Project& project, const SetModulator& set,
                     const Transaction& transaction, CommandId command) {
    // Identity may not move through a Modify. The model is handed one modulator
    // and cannot see that the caller named another, so a swap would be a removal
    // and a creation wearing a signature that declares neither -- and removal is
    // the axis an untrusted writer's mask denies by default.
    if (set.expected.id != set.replacement.id || set.expected.id != set.modulator_id)
        return reject_reduction<ModulationCommandReduction>(ConflictCode::ModelInvariant,
                                                            transaction, command, set.modulator_id,
                                                            set.replacement.id);
    if (const auto code = target_error(
            project, set.modulator_id,
            owned_location(project, ItemKind::Modulator, set.sequence_id, set.track_id)))
        return reject_reduction<ModulationCommandReduction>(*code, transaction, command,
                                                            set.modulator_id, set.track_id);
    const auto* sequence = project.find_sequence(set.sequence_id);
    const auto* track = sequence ? sequence->find_track(set.track_id) : nullptr;
    const auto* modulator = track ? track->find_modulator(set.modulator_id) : nullptr;
    if (!modulator)
        return reject_reduction<ModulationCommandReduction>(ConflictCode::TargetMissing,
                                                            transaction, command, set.modulator_id);
    if (*modulator != set.expected)
        return reject_reduction<ModulationCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                            transaction, command, set.modulator_id);
    return apply_track_edit(project, set.sequence_id, set.track_id, set.modulator_id,
                            DirtyFlags::Content, {}, std::nullopt,
                            SetModulator{set.sequence_id, set.track_id, set.modulator_id,
                                         set.replacement, set.expected},
                            transaction, command, [&](const Track& current) {
                                return current.replace_modulator(set.replacement);
                            });
}

runtime::Result<ModulationCommandReduction, TransactionError>
reduce_set_macro(const Project& project, const SetMacro& set, const Transaction& transaction,
                 CommandId command) {
    if (set.expected.id != set.replacement.id || set.expected.id != set.macro_id)
        return reject_reduction<ModulationCommandReduction>(
            ConflictCode::ModelInvariant, transaction, command, set.macro_id, set.replacement.id);
    if (const auto code = target_error(
            project, set.macro_id,
            owned_location(project, ItemKind::MacroControl, set.sequence_id, set.track_id)))
        return reject_reduction<ModulationCommandReduction>(*code, transaction, command,
                                                            set.macro_id, set.track_id);
    const auto* sequence = project.find_sequence(set.sequence_id);
    const auto* track = sequence ? sequence->find_track(set.track_id) : nullptr;
    const auto* macro = track ? track->find_macro(set.macro_id) : nullptr;
    if (!macro)
        return reject_reduction<ModulationCommandReduction>(ConflictCode::TargetMissing,
                                                            transaction, command, set.macro_id);
    if (*macro != set.expected)
        return reject_reduction<ModulationCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                            transaction, command, set.macro_id);
    return apply_track_edit(
        project, set.sequence_id, set.track_id, set.macro_id, DirtyFlags::Content, {}, std::nullopt,
        SetMacro{set.sequence_id, set.track_id, set.macro_id, set.replacement, set.expected},
        transaction, command,
        [&](const Track& current) { return current.replace_macro(set.replacement); });
}

runtime::Result<ModulationCommandReduction, TransactionError>
reduce_set_macro_value(const Project& project, const SetMacroValue& set,
                       const Transaction& transaction, CommandId command) {
    if (const auto code = target_error(
            project, set.macro_id,
            owned_location(project, ItemKind::MacroControl, set.sequence_id, set.track_id)))
        return reject_reduction<ModulationCommandReduction>(*code, transaction, command,
                                                            set.macro_id, set.track_id);
    const auto* sequence = project.find_sequence(set.sequence_id);
    const auto* track = sequence ? sequence->find_track(set.track_id) : nullptr;
    const auto* macro = track ? track->find_macro(set.macro_id) : nullptr;
    if (!macro)
        return reject_reduction<ModulationCommandReduction>(ConflictCode::TargetMissing,
                                                            transaction, command, set.macro_id);
    // An exact comparison, deliberately: the wire spells this float as its
    // IEEE-754 bit pattern, so a decoded value is bit-identical to the one that
    // was encoded and a tolerance would only widen the gate for a hazard that
    // does not exist here. A NaN in the document fails this test against any
    // expectation, which refuses rather than silently overwrites.
    if (!(macro->value == set.expected))
        return reject_reduction<ModulationCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                            transaction, command, set.macro_id);
    // Range refusal lives in the model, so an out-of-range replacement surfaces
    // as a model failure rather than a silently clamped macro.
    MacroControl replacement = *macro;
    replacement.value = set.replacement;
    return apply_track_edit(
        project, set.sequence_id, set.track_id, set.macro_id, DirtyFlags::Content, {}, std::nullopt,
        SetMacroValue{set.sequence_id, set.track_id, set.macro_id, set.replacement, set.expected},
        transaction, command,
        [&](const Track& current) { return current.replace_macro(replacement); });
}

runtime::Result<ModulationCommandReduction, TransactionError>
reduce_insert_modulation_route(const Project& project, const InsertModulationRoute& insert,
                               const Transaction& transaction, CommandId command,
                               bool allow_tombstone_restore) {
    return reduce_insert_item(
        project, ItemKind::ModulationRoute, insert.sequence_id, insert.track_id, insert.route,
        RemoveModulationRoute{insert.sequence_id, insert.track_id, insert.route.id}, transaction,
        command, allow_tombstone_restore,
        [&](const Track& track) { return track.insert_modulation_route(insert.route); });
}

runtime::Result<ModulationCommandReduction, TransactionError>
reduce_remove_modulation_route(const Project& project, const RemoveModulationRoute& remove,
                               const Transaction& transaction, CommandId command) {
    if (const auto code = target_error(project, remove.route_id,
                                       owned_location(project, ItemKind::ModulationRoute,
                                                      remove.sequence_id, remove.track_id)))
        return reject_reduction<ModulationCommandReduction>(*code, transaction, command,
                                                            remove.route_id, remove.track_id);
    const auto* sequence = project.find_sequence(remove.sequence_id);
    const auto* track = sequence ? sequence->find_track(remove.track_id) : nullptr;
    const auto* route = track ? track->find_modulation_route(remove.route_id) : nullptr;
    if (!route)
        return reject_reduction<ModulationCommandReduction>(ConflictCode::TargetMissing,
                                                            transaction, command, remove.route_id);
    // Captured whole before the edit, bypass included: a disabled route keeps
    // its identity, depth, and target so that re-enabling restores what was
    // there, and an inverse that reconstructed a default would enable a route
    // the author had silenced.
    const ModulationRoute removed = *route;
    const std::array identity{owned_identity(ItemKind::ModulationRoute, remove.route_id,
                                             remove.sequence_id, remove.track_id)};
    return apply_track_edit(
        project, remove.sequence_id, remove.track_id, remove.route_id,
        DirtyFlags::Structure | DirtyFlags::Content | DirtyFlags::Removed,
        plan_identity_deactivate(identity), std::nullopt,
        InsertModulationRoute{remove.sequence_id, remove.track_id, removed}, transaction, command,
        [&](const Track& current) { return current.erase_modulation_route(remove.route_id); });
}

runtime::Result<ModulationCommandReduction, TransactionError>
reduce_set_modulation_route(const Project& project, const SetModulationRoute& set,
                            const Transaction& transaction, CommandId command) {
    if (set.expected.id != set.replacement.id || set.expected.id != set.route_id)
        return reject_reduction<ModulationCommandReduction>(
            ConflictCode::ModelInvariant, transaction, command, set.route_id, set.replacement.id);
    if (const auto code = target_error(
            project, set.route_id,
            owned_location(project, ItemKind::ModulationRoute, set.sequence_id, set.track_id)))
        return reject_reduction<ModulationCommandReduction>(*code, transaction, command,
                                                            set.route_id, set.track_id);
    const auto* sequence = project.find_sequence(set.sequence_id);
    const auto* track = sequence ? sequence->find_track(set.track_id) : nullptr;
    const auto* route = track ? track->find_modulation_route(set.route_id) : nullptr;
    if (!route)
        return reject_reduction<ModulationCommandReduction>(ConflictCode::TargetMissing,
                                                            transaction, command, set.route_id);
    // The whole route including its bypass, so an edit that believed a route
    // was live cannot land on one an author had disabled.
    if (*route != set.expected)
        return reject_reduction<ModulationCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                            transaction, command, set.route_id);
    // Whether the replacement names a source the track holds, of the kind it
    // claims, and a target the device chain carries, is decided by the model's
    // own revalidation rather than restated here.
    return apply_track_edit(
        project, set.sequence_id, set.track_id, set.route_id, DirtyFlags::Content, {}, std::nullopt,
        SetModulationRoute{set.sequence_id, set.track_id, set.route_id, set.replacement,
                           set.expected},
        transaction, command,
        [&](const Track& current) { return current.replace_modulation_route(set.replacement); });
}

} // namespace

bool is_modulation_command(const Command& command) noexcept {
    return std::visit([]<typename T>(const T&) { return is_modulation_command_type<T>; }, command);
}

runtime::Result<ModulationCommandReduction, TransactionError>
reduce_modulation_command(const Project& project, const Command& command,
                          const Transaction& transaction, CommandId command_id,
                          bool allow_tombstone_restore) {
    // The family predicate above this call and the arms below it are two
    // statements of the same list, and a chain of get_if proves nothing about
    // its own coverage. Visiting puts a claimed-but-unhandled command in front
    // of the compiler, which is where the outer dispatch already resolves it.
    return std::visit(
        [&]<typename T>(
            const T& value) -> runtime::Result<ModulationCommandReduction, TransactionError> {
            if constexpr (std::is_same_v<T, InsertModulator>)
                return reduce_insert_modulator(project, value, transaction, command_id,
                                               allow_tombstone_restore);
            else if constexpr (std::is_same_v<T, RemoveModulator>)
                return reduce_remove_modulator(project, value, transaction, command_id);
            else if constexpr (std::is_same_v<T, SetModulator>)
                return reduce_set_modulator(project, value, transaction, command_id);
            else if constexpr (std::is_same_v<T, InsertMacro>)
                return reduce_insert_macro(project, value, transaction, command_id,
                                           allow_tombstone_restore);
            else if constexpr (std::is_same_v<T, RemoveMacro>)
                return reduce_remove_macro(project, value, transaction, command_id);
            else if constexpr (std::is_same_v<T, SetMacro>)
                return reduce_set_macro(project, value, transaction, command_id);
            else if constexpr (std::is_same_v<T, SetMacroValue>)
                return reduce_set_macro_value(project, value, transaction, command_id);
            else if constexpr (std::is_same_v<T, InsertModulationRoute>)
                return reduce_insert_modulation_route(project, value, transaction, command_id,
                                                      allow_tombstone_restore);
            else if constexpr (std::is_same_v<T, RemoveModulationRoute>)
                return reduce_remove_modulation_route(project, value, transaction, command_id);
            else if constexpr (std::is_same_v<T, SetModulationRoute>)
                return reduce_set_modulation_route(project, value, transaction, command_id);
            else {
                static_assert(!is_modulation_command_type<T>,
                              "a command claimed by is_modulation_command_type in "
                              "transaction_dispatch_internal.hpp has no arm here; add "
                              "one, or drop it from the claim list");
                // Reached only by an alternative no family claims, which the
                // caller's predicate already excludes. Kept as a rejection rather
                // than std::unreachable(): this TU is -fno-exceptions, so being
                // wrong here would abort the process, not fail one transaction.
                return reject_reduction<ModulationCommandReduction>(ConflictCode::ModelInvariant,
                                                                    transaction, command_id);
            }
        },
        command);
}

} // namespace pulp::timeline::detail
