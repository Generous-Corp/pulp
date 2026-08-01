#include "transaction_marker_internal.hpp"

#include "transaction_dispatch_internal.hpp"
#include "transaction_reduction_support.hpp"

#include <array>
#include <utility>
#include <variant>

namespace pulp::timeline::detail {
namespace {

// A marker and a region are both parented by their sequence, so one location
// builder serves both; the kind is the only thing that varies.
OwnedIdentity owned_identity(ItemKind kind, ItemId item, ItemId sequence) {
    return {item,
            ItemLocation{
                kind, immediate_parent_id(kind, {}, sequence, {}, {}), sequence, {}, {}, true}};
}

ItemLocation sequence_location(const Project& project, ItemId sequence) {
    return ItemLocation{ItemKind::Sequence,
                        immediate_parent_id(ItemKind::Sequence, project.id(), sequence, {}, {}),
                        sequence,
                        {},
                        {},
                        true};
}

ItemLocation annotation_location(const Project& project, ItemKind kind, ItemId sequence) {
    return ItemLocation{
        kind, immediate_parent_id(kind, project.id(), sequence, {}, {}), sequence, {}, {}, true};
}

// Insert and remove differ only in which model edit runs and which inverse is
// produced, so both directions share this shell: gate the target, plan the
// identity transition, swap the sequence, and report the dirty item.
template <typename EditFn>
runtime::Result<MarkerCommandReduction, TransactionError>
apply_sequence_edit(const Project& project, ItemId sequence_id, ItemId item, DirtyFlags flags,
                    std::span<const IdentityMutation> identities,
                    std::optional<std::uint64_t> next_item_id, Command inverse,
                    const Transaction& transaction, CommandId command, EditFn&& edit) {
    const auto* sequence = project.find_sequence(sequence_id);
    if (!sequence)
        return reject_reduction<MarkerCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                        command, sequence_id);
    auto next_sequence = edit(*sequence);
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    auto next_project = ProjectEditAccess::replace_sequence(
        project, std::move(next_sequence).value(), identities, next_item_id);
    if (!next_project)
        return runtime::Err(model_failure(transaction, command, next_project.error()));
    return runtime::Ok(MarkerCommandReduction{
        std::move(next_project).value(), std::move(inverse), {item, {}, sequence_id, flags}});
}

runtime::Result<MarkerCommandReduction, TransactionError>
reduce_insert_marker(const Project& project, const InsertMarker& insert,
                     const Transaction& transaction, CommandId command,
                     bool allow_tombstone_restore) {
    if (const auto code = target_error(project, insert.sequence_id,
                                       sequence_location(project, insert.sequence_id)))
        return reject_reduction<MarkerCommandReduction>(*code, transaction, command,
                                                        insert.sequence_id);
    const std::array identity{
        owned_identity(ItemKind::Marker, insert.marker.id, insert.sequence_id)};
    auto identity_plan =
        plan_identity_insert(project, identity, allow_tombstone_restore, transaction, command);
    if (!identity_plan)
        return runtime::Err(identity_plan.error());
    return apply_sequence_edit(
        project, insert.sequence_id, insert.marker.id,
        DirtyFlags::Structure | DirtyFlags::Marker | DirtyFlags::Added, identity_plan->mutations,
        identity_plan->next_item_id, RemoveMarker{insert.sequence_id, insert.marker.id},
        transaction, command,
        [&](const Sequence& sequence) { return sequence.insert_marker(insert.marker); });
}

runtime::Result<MarkerCommandReduction, TransactionError>
reduce_remove_marker(const Project& project, const RemoveMarker& remove,
                     const Transaction& transaction, CommandId command) {
    if (const auto code =
            target_error(project, remove.marker_id,
                         annotation_location(project, ItemKind::Marker, remove.sequence_id)))
        return reject_reduction<MarkerCommandReduction>(*code, transaction, command,
                                                        remove.marker_id, remove.sequence_id);
    const auto* sequence = project.find_sequence(remove.sequence_id);
    const auto* marker = sequence ? sequence->find_marker(remove.marker_id) : nullptr;
    if (!marker)
        return reject_reduction<MarkerCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                        command, remove.marker_id);
    const SequenceMarker removed = *marker;
    const std::array identity{
        owned_identity(ItemKind::Marker, remove.marker_id, remove.sequence_id)};
    const auto identity_changes = plan_identity_deactivate(identity);
    return apply_sequence_edit(
        project, remove.sequence_id, remove.marker_id,
        DirtyFlags::Structure | DirtyFlags::Marker | DirtyFlags::Removed, identity_changes,
        std::nullopt, InsertMarker{remove.sequence_id, removed}, transaction, command,
        [&](const Sequence& current) { return current.erase_marker(remove.marker_id); });
}

runtime::Result<MarkerCommandReduction, TransactionError>
reduce_insert_region(const Project& project, const InsertRegion& insert,
                     const Transaction& transaction, CommandId command,
                     bool allow_tombstone_restore) {
    if (const auto code = target_error(project, insert.sequence_id,
                                       sequence_location(project, insert.sequence_id)))
        return reject_reduction<MarkerCommandReduction>(*code, transaction, command,
                                                        insert.sequence_id);
    const std::array identity{
        owned_identity(ItemKind::Region, insert.region.id, insert.sequence_id)};
    auto identity_plan =
        plan_identity_insert(project, identity, allow_tombstone_restore, transaction, command);
    if (!identity_plan)
        return runtime::Err(identity_plan.error());
    return apply_sequence_edit(
        project, insert.sequence_id, insert.region.id,
        DirtyFlags::Structure | DirtyFlags::Marker | DirtyFlags::Added, identity_plan->mutations,
        identity_plan->next_item_id, RemoveRegion{insert.sequence_id, insert.region.id},
        transaction, command,
        [&](const Sequence& sequence) { return sequence.insert_region(insert.region); });
}

runtime::Result<MarkerCommandReduction, TransactionError>
reduce_remove_region(const Project& project, const RemoveRegion& remove,
                     const Transaction& transaction, CommandId command) {
    if (const auto code =
            target_error(project, remove.region_id,
                         annotation_location(project, ItemKind::Region, remove.sequence_id)))
        return reject_reduction<MarkerCommandReduction>(*code, transaction, command,
                                                        remove.region_id, remove.sequence_id);
    const auto* sequence = project.find_sequence(remove.sequence_id);
    const auto* region = sequence ? sequence->find_region(remove.region_id) : nullptr;
    if (!region)
        return reject_reduction<MarkerCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                        command, remove.region_id);
    const SequenceRegion removed = *region;
    const std::array identity{
        owned_identity(ItemKind::Region, remove.region_id, remove.sequence_id)};
    const auto identity_changes = plan_identity_deactivate(identity);
    return apply_sequence_edit(
        project, remove.sequence_id, remove.region_id,
        DirtyFlags::Structure | DirtyFlags::Marker | DirtyFlags::Removed, identity_changes,
        std::nullopt, InsertRegion{remove.sequence_id, removed}, transaction, command,
        [&](const Sequence& current) { return current.erase_region(remove.region_id); });
}

} // namespace

bool is_marker_command(const Command& command) noexcept {
    return std::visit([]<typename T>(const T&) { return is_marker_command_type<T>; }, command);
}

runtime::Result<MarkerCommandReduction, TransactionError>
reduce_marker_command(const Project& project, const Command& command,
                      const Transaction& transaction, CommandId command_id,
                      bool allow_tombstone_restore) {
    if (const auto* insert = std::get_if<InsertMarker>(&command))
        return reduce_insert_marker(project, *insert, transaction, command_id,
                                    allow_tombstone_restore);
    if (const auto* remove = std::get_if<RemoveMarker>(&command))
        return reduce_remove_marker(project, *remove, transaction, command_id);
    if (const auto* insert = std::get_if<InsertRegion>(&command))
        return reduce_insert_region(project, *insert, transaction, command_id,
                                    allow_tombstone_restore);
    if (const auto* remove = std::get_if<RemoveRegion>(&command))
        return reduce_remove_region(project, *remove, transaction, command_id);
    return reject_reduction<MarkerCommandReduction>(ConflictCode::ModelInvariant, transaction,
                                                    command_id);
}

} // namespace pulp::timeline::detail
