#include "transaction_annotation_internal.hpp"

#include "transaction_reduction_support.hpp"

namespace pulp::timeline::detail {
namespace {

template <typename T>
runtime::Result<T, TransactionError> reject(ConflictCode code, const Transaction& transaction,
                                            CommandId command, ItemId item,
                                            ItemId related = {}) {
    return reject_reduction<T>(code, transaction, command, item, related);
}

template <typename Value>
runtime::Result<AnnotationCommandReduction, TransactionError>
insert_value(const Project& project, ItemId sequence_id, const Value& value,
             ItemKind kind, const Transaction& transaction, CommandId command,
             bool allow_tombstone_restore) {
    if (const auto code = target_error(project, sequence_id,
                                       {.kind = ItemKind::Sequence,
                                        .sequence_id = sequence_id,
                                        .active = true}))
        return reject<AnnotationCommandReduction>(*code, transaction, command, sequence_id);
    const OwnedIdentity identity{value.id, {.kind = kind,
                                            .sequence_id = sequence_id,
                                            .active = true}};
    auto plan = plan_identity_insert(project, std::span(&identity, 1), allow_tombstone_restore,
                                     transaction, command);
    if (!plan)
        return runtime::Err(plan.error());
    const auto* sequence = project.find_sequence(sequence_id);
    runtime::Result<Sequence, ModelError> next = [&] {
        if constexpr (std::is_same_v<Value, SequenceMarker>)
            return sequence->insert_marker(value);
        else
            return sequence->insert_region(value);
    }();
    if (!next)
        return runtime::Err(model_failure(transaction, command, next.error()));
    auto replaced = ProjectEditAccess::replace_sequence(project, std::move(next).value(),
                                                        plan->mutations, plan->next_item_id);
    if (!replaced)
        return runtime::Err(model_failure(transaction, command, replaced.error()));
    Command inverse = [&]() -> Command {
        if constexpr (std::is_same_v<Value, SequenceMarker>)
            return RemoveSequenceMarker{sequence_id, value.id};
        else
            return RemoveSequenceRegion{sequence_id, value.id};
    }();
    return runtime::Ok(AnnotationCommandReduction{
        std::move(replaced).value(), std::move(inverse),
        {value.id, {}, sequence_id,
         DirtyFlags::Structure | DirtyFlags::Annotation | DirtyFlags::Added}});
}

template <typename Value>
runtime::Result<AnnotationCommandReduction, TransactionError>
remove_value(const Project& project, ItemId sequence_id, ItemId item_id, ItemKind kind,
             const Transaction& transaction, CommandId command) {
    if (const auto code = target_error(project, item_id,
                                       {.kind = kind,
                                        .sequence_id = sequence_id,
                                        .active = true}))
        return reject<AnnotationCommandReduction>(*code, transaction, command, item_id,
                                                  sequence_id);
    const auto* sequence = project.find_sequence(sequence_id);
    const Value* value = [&] {
        if constexpr (std::is_same_v<Value, SequenceMarker>)
            return sequence ? sequence->find_marker(item_id) : nullptr;
        else
            return sequence ? sequence->find_region(item_id) : nullptr;
    }();
    if (!value)
        return reject<AnnotationCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                  command, item_id);
    const Value removed = *value;
    const OwnedIdentity identity{item_id, {.kind = kind,
                                           .sequence_id = sequence_id,
                                           .active = true}};
    const auto mutations = plan_identity_deactivate(std::span(&identity, 1));
    runtime::Result<Sequence, ModelError> next = [&] {
        if constexpr (std::is_same_v<Value, SequenceMarker>)
            return sequence->erase_marker(item_id);
        else
            return sequence->erase_region(item_id);
    }();
    if (!next)
        return runtime::Err(model_failure(transaction, command, next.error()));
    auto replaced = ProjectEditAccess::replace_sequence(project, std::move(next).value(), mutations);
    if (!replaced)
        return runtime::Err(model_failure(transaction, command, replaced.error()));
    Command inverse = [&]() -> Command {
        if constexpr (std::is_same_v<Value, SequenceMarker>)
            return InsertSequenceMarker{sequence_id, removed};
        else
            return InsertSequenceRegion{sequence_id, removed};
    }();
    return runtime::Ok(AnnotationCommandReduction{
        std::move(replaced).value(), std::move(inverse),
        {item_id, {}, sequence_id,
         DirtyFlags::Structure | DirtyFlags::Annotation | DirtyFlags::Removed}});
}

template <typename Value>
runtime::Result<AnnotationCommandReduction, TransactionError>
set_value(const Project& project, ItemId sequence_id, ItemId item_id, const Value& expected,
          const Value& replacement, ItemKind kind, const Transaction& transaction,
          CommandId command) {
    if (expected.id != item_id || replacement.id != item_id)
        return reject<AnnotationCommandReduction>(ConflictCode::InvalidIdentifier, transaction,
                                                  command, item_id);
    if (const auto code = target_error(project, item_id,
                                       {.kind = kind,
                                        .sequence_id = sequence_id,
                                        .active = true}))
        return reject<AnnotationCommandReduction>(*code, transaction, command, item_id,
                                                  sequence_id);
    const auto* sequence = project.find_sequence(sequence_id);
    const Value* current = [&] {
        if constexpr (std::is_same_v<Value, SequenceMarker>)
            return sequence ? sequence->find_marker(item_id) : nullptr;
        else
            return sequence ? sequence->find_region(item_id) : nullptr;
    }();
    if (!current || *current != expected)
        return reject<AnnotationCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                  transaction, command, item_id);
    runtime::Result<Sequence, ModelError> next = [&] {
        if constexpr (std::is_same_v<Value, SequenceMarker>)
            return sequence->replace_marker(replacement);
        else
            return sequence->replace_region(replacement);
    }();
    if (!next)
        return runtime::Err(model_failure(transaction, command, next.error()));
    auto replaced = ProjectEditAccess::replace_sequence(project, std::move(next).value());
    if (!replaced)
        return runtime::Err(model_failure(transaction, command, replaced.error()));
    Command inverse = [&]() -> Command {
        if constexpr (std::is_same_v<Value, SequenceMarker>)
            return SetSequenceMarker{sequence_id, item_id, replacement, expected};
        else
            return SetSequenceRegion{sequence_id, item_id, replacement, expected};
    }();
    return runtime::Ok(AnnotationCommandReduction{
        std::move(replaced).value(), std::move(inverse),
        {item_id, {}, sequence_id,
         DirtyFlags::Timing | DirtyFlags::Content | DirtyFlags::Annotation}});
}

} // namespace

bool is_annotation_command(const Command& command) noexcept {
    return std::holds_alternative<InsertSequenceMarker>(command) ||
           std::holds_alternative<RemoveSequenceMarker>(command) ||
           std::holds_alternative<SetSequenceMarker>(command) ||
           std::holds_alternative<InsertSequenceRegion>(command) ||
           std::holds_alternative<RemoveSequenceRegion>(command) ||
           std::holds_alternative<SetSequenceRegion>(command);
}

runtime::Result<AnnotationCommandReduction, TransactionError>
reduce_annotation_command(const Project& project, const Command& command,
                          const Transaction& transaction, CommandId command_id,
                          bool allow_tombstone_restore) {
    if (const auto* value = std::get_if<InsertSequenceMarker>(&command))
        return insert_value(project, value->sequence_id, value->marker, ItemKind::SequenceMarker,
                            transaction, command_id, allow_tombstone_restore);
    if (const auto* value = std::get_if<RemoveSequenceMarker>(&command))
        return remove_value<SequenceMarker>(project, value->sequence_id, value->marker_id,
                                            ItemKind::SequenceMarker, transaction, command_id);
    if (const auto* value = std::get_if<SetSequenceMarker>(&command))
        return set_value(project, value->sequence_id, value->marker_id, value->expected,
                         value->replacement, ItemKind::SequenceMarker, transaction, command_id);
    if (const auto* value = std::get_if<InsertSequenceRegion>(&command))
        return insert_value(project, value->sequence_id, value->region, ItemKind::SequenceRegion,
                            transaction, command_id, allow_tombstone_restore);
    if (const auto* value = std::get_if<RemoveSequenceRegion>(&command))
        return remove_value<SequenceRegion>(project, value->sequence_id, value->region_id,
                                            ItemKind::SequenceRegion, transaction, command_id);
    if (const auto* value = std::get_if<SetSequenceRegion>(&command))
        return set_value(project, value->sequence_id, value->region_id, value->expected,
                         value->replacement, ItemKind::SequenceRegion, transaction, command_id);
    return reject<AnnotationCommandReduction>(ConflictCode::ModelInvariant, transaction,
                                              command_id, {});
}

} // namespace pulp::timeline::detail
