#include "transaction_scene_internal.hpp"

#include "transaction_reduction_support.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include <utility>
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
    const auto* scene = sequence ? sequence->find_scene(remove.scene_id) : nullptr;
    if (!scene)
        return reject_reduction<SceneCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                       command, remove.scene_id);
    const Scene removed = *scene;
    const auto scene_position =
        std::find_if(sequence->scenes().begin(), sequence->scenes().end(),
                     [&](const Scene& candidate) { return candidate.id == remove.scene_id; });
    const auto following_scene = std::next(scene_position);
    const auto before_scene_id =
        following_scene == sequence->scenes().end()
            ? std::optional<ItemId>{}
            : std::optional<ItemId>{following_scene->id};
    std::vector<OwnedIdentity> items;
    items.reserve(1 + removed.slots.size());
    items.push_back(owned(ItemKind::Scene, removed.id, remove.sequence_id));
    for (const auto& slot : removed.slots)
        items.push_back(owned(ItemKind::Slot, slot.id, remove.sequence_id, removed.id));
    auto next = sequence->erase_scene(remove.scene_id);
    if (!next)
        return runtime::Err(model_failure(transaction, command, next.error()));
    const auto identities = plan_identity_deactivate(items);
    return finish(project, next.value(),
                  InsertScene{remove.sequence_id, removed, before_scene_id}, remove.scene_id,
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
    const auto* scene = sequence ? sequence->find_scene(remove.scene_id) : nullptr;
    const Slot* slot = nullptr;
    if (scene) {
        const auto found = std::find_if(scene->slots.begin(), scene->slots.end(),
                                       [&](const Slot& value) {
                                           return value.id == remove.slot_id;
                                       });
        if (found != scene->slots.end())
            slot = &*found;
    }
    if (!slot)
        return reject_reduction<SceneCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                       command, remove.slot_id);
    const Slot removed = *slot;
    const auto slot_position =
        std::find_if(scene->slots.begin(), scene->slots.end(),
                     [&](const Slot& candidate) { return candidate.id == remove.slot_id; });
    const auto following_slot = std::next(slot_position);
    const auto before_slot_id =
        following_slot == scene->slots.end() ? std::optional<ItemId>{}
                                             : std::optional<ItemId>{following_slot->id};
    auto next = sequence->erase_slot(remove.scene_id, remove.slot_id);
    if (!next)
        return runtime::Err(model_failure(transaction, command, next.error()));
    const std::array item{owned(ItemKind::Slot, remove.slot_id, remove.sequence_id,
                                remove.scene_id)};
    const auto identities = plan_identity_deactivate(item);
    return finish(project, next.value(),
                  InsertSlot{remove.sequence_id, remove.scene_id, removed, before_slot_id},
                  remove.slot_id,
                  DirtyFlags::Structure | DirtyFlags::Removed, identities, std::nullopt,
                  transaction, command);
}

} // namespace

bool is_scene_command(const Command& command) noexcept {
    return std::holds_alternative<InsertScene>(command) ||
           std::holds_alternative<RemoveScene>(command) ||
           std::holds_alternative<InsertSlot>(command) ||
           std::holds_alternative<RemoveSlot>(command);
}

runtime::Result<SceneCommandReduction, TransactionError>
reduce_scene_command(const Project& project, const Command& command,
                     const Transaction& transaction, CommandId command_id,
                     bool allow_tombstone_restore) {
    if (const auto* value = std::get_if<InsertScene>(&command))
        return insert_scene(project, *value, transaction, command_id, allow_tombstone_restore);
    if (const auto* value = std::get_if<RemoveScene>(&command))
        return remove_scene(project, *value, transaction, command_id);
    if (const auto* value = std::get_if<InsertSlot>(&command))
        return insert_slot(project, *value, transaction, command_id, allow_tombstone_restore);
    if (const auto* value = std::get_if<RemoveSlot>(&command))
        return remove_slot(project, *value, transaction, command_id);
    return reject_reduction<SceneCommandReduction>(ConflictCode::ModelInvariant, transaction,
                                                   command_id);
}

} // namespace pulp::timeline::detail
