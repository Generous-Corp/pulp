#include "transaction_scene_internal.hpp"

#include "sequence_scene_internal.hpp"
#include "transaction_dispatch_internal.hpp"
#include "transaction_reduction_support.hpp"

#include <array>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace pulp::timeline::detail {
namespace {

OwnedIdentity owned(ItemKind kind, ItemId item, ItemId sequence, ItemId parent = {}) {
    return {item, ItemLocation{kind,
                              immediate_parent_id(kind, {}, sequence, {}, {}, parent),
                              sequence, {}, {}, true}};
}

ItemLocation expected(ItemKind kind, const Project& project, ItemId sequence, ItemId parent = {}) {
    return ItemLocation{kind,
                        immediate_parent_id(kind, project.id(), sequence, {}, {}, parent),
                        sequence, {}, {}, true};
}

runtime::Result<SceneCommandReduction, TransactionError>
finish(const Project& project, const Sequence& sequence, Command inverse, ItemId dirty_item,
       DirtyFlags flags, std::span<const IdentityMutation> identities,
       std::optional<std::uint64_t> next_item_id, const Transaction& transaction,
       CommandId command) {
    auto next = ProjectEditAccess::replace_sequence(project, sequence, identities, next_item_id);
    if (!next)
        return runtime::Err(model_failure(transaction, command, next.error()));
    return runtime::Ok(SceneCommandReduction{
        std::move(next).value(), std::move(inverse), {dirty_item, {}, sequence.id(), flags}});
}

runtime::Result<SceneCommandReduction, TransactionError>
insert_scene(const Project& project, const InsertScene& insert, const Transaction& transaction,
             CommandId command, bool restore) {
    if (const auto code =
            target_error(project, insert.sequence_id,
                         expected(ItemKind::Sequence, project, insert.sequence_id)))
        return reject_reduction<SceneCommandReduction>(*code, transaction, command,
                                                       insert.sequence_id);
    if (insert.before_scene_id)
        if (const auto code =
                target_error(project, *insert.before_scene_id,
                             expected(ItemKind::Scene, project, insert.sequence_id)))
            return reject_reduction<SceneCommandReduction>(
                *code, transaction, command, *insert.before_scene_id, insert.sequence_id);
    std::vector<OwnedIdentity> items;
    items.reserve(1 + insert.scene.slots.size());
    items.push_back(owned(ItemKind::Scene, insert.scene.id, insert.sequence_id));
    for (const auto& slot : insert.scene.slots)
        items.push_back(owned(ItemKind::Slot, slot.id, insert.sequence_id, insert.scene.id));
    auto plan = plan_identity_insert(project, items, restore, transaction, command);
    if (!plan)
        return runtime::Err(plan.error());
    auto next = project.find_sequence(insert.sequence_id)
                    ->insert_scene(insert.scene, insert.before_scene_id);
    if (!next)
        return runtime::Err(model_failure(transaction, command, next.error()));
    return finish(project, next.value(), RemoveScene{insert.sequence_id, insert.scene.id},
                  insert.scene.id, DirtyFlags::Structure | DirtyFlags::Added, plan->mutations,
                  plan->next_item_id, transaction, command);
}

runtime::Result<SceneCommandReduction, TransactionError>
remove_scene(const Project& project, const RemoveScene& remove, const Transaction& transaction,
             CommandId command) {
    if (const auto code =
            target_error(project, remove.scene_id,
                         expected(ItemKind::Scene, project, remove.sequence_id)))
        return reject_reduction<SceneCommandReduction>(*code, transaction, command, remove.scene_id);
    const auto* sequence = project.find_sequence(remove.sequence_id);
    if (!sequence)
        return reject_reduction<SceneCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                       command, remove.scene_id);
    auto erased = SequenceEditAccess::erase_scene(*sequence, remove.scene_id);
    if (!erased)
        return runtime::Err(model_failure(transaction, command, erased.error()));
    auto result = std::move(erased).value();
    std::vector<OwnedIdentity> items;
    items.reserve(1 + result.removed.slots.size());
    items.push_back(owned(ItemKind::Scene, result.removed.id, remove.sequence_id));
    for (const auto& slot : result.removed.slots)
        items.push_back(owned(ItemKind::Slot, slot.id, remove.sequence_id, result.removed.id));
    const auto identities = plan_identity_deactivate(items);
    return finish(project, result.sequence,
                  InsertScene{remove.sequence_id, result.removed, result.following}, remove.scene_id,
                  DirtyFlags::Structure | DirtyFlags::Removed, identities, std::nullopt,
                  transaction, command);
}

runtime::Result<SceneCommandReduction, TransactionError>
insert_slot(const Project& project, const InsertSlot& insert, const Transaction& transaction,
            CommandId command, bool restore) {
    if (const auto code = target_error(
            project, insert.scene_id,
            expected(ItemKind::Scene, project, insert.sequence_id)))
        return reject_reduction<SceneCommandReduction>(*code, transaction, command, insert.scene_id);
    if (insert.before_slot_id)
        if (const auto code =
                target_error(project, *insert.before_slot_id,
                             expected(ItemKind::Slot, project, insert.sequence_id, insert.scene_id)))
            return reject_reduction<SceneCommandReduction>(
                *code, transaction, command, *insert.before_slot_id, insert.scene_id);
    const std::array item{
        owned(ItemKind::Slot, insert.slot.id, insert.sequence_id, insert.scene_id)};
    auto plan = plan_identity_insert(project, item, restore, transaction, command);
    if (!plan)
        return runtime::Err(plan.error());
    auto next = project.find_sequence(insert.sequence_id)
                    ->insert_slot(insert.scene_id, insert.slot, insert.before_slot_id);
    if (!next)
        return runtime::Err(model_failure(transaction, command, next.error()));
    return finish(project, next.value(),
                  RemoveSlot{insert.sequence_id, insert.scene_id, insert.slot.id}, insert.slot.id,
                  DirtyFlags::Structure | DirtyFlags::Added, plan->mutations, plan->next_item_id,
                  transaction, command);
}

runtime::Result<SceneCommandReduction, TransactionError>
remove_slot(const Project& project, const RemoveSlot& remove, const Transaction& transaction,
            CommandId command) {
    if (const auto code =
            target_error(project, remove.slot_id,
                         expected(ItemKind::Slot, project, remove.sequence_id, remove.scene_id)))
        return reject_reduction<SceneCommandReduction>(*code, transaction, command, remove.slot_id);
    const auto* sequence = project.find_sequence(remove.sequence_id);
    if (!sequence)
        return reject_reduction<SceneCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                       command, remove.slot_id);
    auto erased = SequenceEditAccess::erase_slot(*sequence, remove.scene_id, remove.slot_id);
    if (!erased)
        return runtime::Err(model_failure(transaction, command, erased.error()));
    auto result = std::move(erased).value();
    const std::array item{owned(ItemKind::Slot, remove.slot_id, remove.sequence_id,
                                remove.scene_id)};
    const auto identities = plan_identity_deactivate(item);
    return finish(project, result.sequence,
                  InsertSlot{remove.sequence_id, remove.scene_id, result.removed, result.following},
                  remove.slot_id,
                  DirtyFlags::Structure | DirtyFlags::Removed, identities, std::nullopt,
                  transaction, command);
}

} // namespace

bool is_scene_command(const Command& command) noexcept {
    return std::visit([]<typename T>(const T&) { return is_scene_command_type<T>; }, command);
}

runtime::Result<SceneCommandReduction, TransactionError>
reduce_scene_command(const Project& project, const Command& command,
                     const Transaction& transaction, CommandId command_id,
                     bool allow_tombstone_restore) {
    // The family predicate above this call and the arms below it are two
    // statements of the same list, and a chain of get_if proves nothing about
    // its own coverage. Visiting puts a claimed-but-unhandled command in front
    // of the compiler, which is where the outer dispatch already resolves it.
    return std::visit(
        [&]<typename T>(
            const T& value) -> runtime::Result<SceneCommandReduction, TransactionError> {
            if constexpr (std::is_same_v<T, InsertScene>)
                return insert_scene(project, value, transaction, command_id,
                                    allow_tombstone_restore);
            else if constexpr (std::is_same_v<T, RemoveScene>)
                return remove_scene(project, value, transaction, command_id);
            else if constexpr (std::is_same_v<T, InsertSlot>)
                return insert_slot(project, value, transaction, command_id,
                                   allow_tombstone_restore);
            else if constexpr (std::is_same_v<T, RemoveSlot>)
                return remove_slot(project, value, transaction, command_id);
            else {
                static_assert(!is_scene_command_type<T>,
                              "a command claimed by is_scene_command_type in "
                              "transaction_dispatch_internal.hpp has no arm here; add "
                              "one, or drop it from the claim list");
                // Reached only by an alternative no family claims, which the
                // caller's predicate already excludes. Kept as a rejection rather
                // than std::unreachable(): this TU is -fno-exceptions, so being
                // wrong here would abort the process, not fail one transaction.
                return reject_reduction<SceneCommandReduction>(ConflictCode::ModelInvariant,
                                                               transaction, command_id);
            }
        },
        command);
}

} // namespace pulp::timeline::detail
