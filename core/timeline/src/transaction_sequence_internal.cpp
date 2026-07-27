#include "transaction_sequence_internal.hpp"

#include "media_reference_validation.hpp"
#include "owned_identity_traversal.hpp"
#include "sequence_graph_validation.hpp"
#include "transaction_reduction_support.hpp"

#include <utility>

namespace pulp::timeline::detail {
namespace {

ItemLocation expected_location(ItemKind kind, const Project& project, ItemId sequence,
                               ItemId track = {}, ItemId clip = {}) {
    return ItemLocation{kind,     immediate_parent_id(kind, project.id(), sequence, track, clip),
                        sequence, track,
                        clip,     true};
}

std::vector<OwnedIdentity> owned_identities(const Sequence& sequence, const Project& project) {
    std::vector<OwnedIdentity> result;
    visit_sequence_owned_identities(sequence, [&](const ModelOwnedIdentity& identity) {
        result.push_back(
            {identity.id,
             ItemLocation{identity.kind,
                          immediate_parent_id(identity.kind, project.id(), sequence.id(),
                                              identity.track, identity.clip, identity.lane),
                          sequence.id(), identity.track, identity.clip, true}});
    });
    return result;
}

runtime::Result<SequenceCommandReduction, TransactionError>
reduce_insert(const Project& project, const InsertSequence& insert, const Transaction& transaction,
              CommandId command, bool allow_tombstone_restore) {
    if (const auto error = validate_sequence_media(project, insert.sequence))
        return runtime::Err(model_failure(transaction, command, *error));
    const auto identities = owned_identities(insert.sequence, project);
    auto identity_plan =
        plan_identity_insert(project, identities, allow_tombstone_restore, transaction, command);
    if (!identity_plan)
        return runtime::Err(identity_plan.error());
    auto next_project = ProjectEditAccess::append_sequence(
        project, insert.sequence, identity_plan->mutations, identity_plan->next_item_id);
    if (!next_project)
        return runtime::Err(model_failure(transaction, command, next_project.error()));
    const auto inserted_id = insert.sequence.id();
    return runtime::Ok(SequenceCommandReduction{
        std::move(next_project).value(),
        RemoveSequence{inserted_id},
        {inserted_id, {}, inserted_id, DirtyFlags::Structure | DirtyFlags::Added}});
}

runtime::Result<SequenceCommandReduction, TransactionError>
reduce_clone(const Project& project, const CloneSequence& clone, const Transaction& transaction,
             CommandId command, bool allow_tombstone_restore) {
    const auto* source = project.find_sequence(clone.source_sequence_id);
    if (!source)
        return reject_reduction<SequenceCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                          command, clone.source_sequence_id);
    auto remapped = remap_ids(*source, clone.id_remap);
    if (!remapped)
        return runtime::Err(model_failure(transaction, command, remapped.error()));
    if (remapped->sequence.id() != clone.cloned_sequence_id)
        return reject_reduction<SequenceCommandReduction>(
            ConflictCode::IdentityNotAvailable, transaction, command, clone.cloned_sequence_id,
            remapped->sequence.id());
    const auto identities = owned_identities(remapped->sequence, project);
    auto identity_plan =
        plan_identity_insert(project, identities, allow_tombstone_restore, transaction, command);
    if (!identity_plan)
        return runtime::Err(identity_plan.error());
    auto next_project =
        ProjectEditAccess::append_sequence(project, std::move(remapped).value().sequence,
                                           identity_plan->mutations, identity_plan->next_item_id);
    if (!next_project)
        return runtime::Err(model_failure(transaction, command, next_project.error()));
    return runtime::Ok(SequenceCommandReduction{std::move(next_project).value(),
                                                RemoveSequence{clone.cloned_sequence_id},
                                                {clone.cloned_sequence_id,
                                                 {},
                                                 clone.cloned_sequence_id,
                                                 DirtyFlags::Structure | DirtyFlags::Added}});
}

runtime::Result<SequenceCommandReduction, TransactionError>
reduce_remove(const Project& project, const RemoveSequence& remove, const Transaction& transaction,
              CommandId command) {
    if (const auto code =
            target_error(project, remove.sequence_id,
                         expected_location(ItemKind::Sequence, project, remove.sequence_id)))
        return reject_reduction<SequenceCommandReduction>(*code, transaction, command,
                                                          remove.sequence_id);
    const auto* found = project.find_sequence(remove.sequence_id);
    if (!found)
        return reject_reduction<SequenceCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                          command, remove.sequence_id);
    const Sequence removed = *found;
    const auto identities = owned_identities(removed, project);
    const auto identity_changes = plan_identity_deactivate(identities);
    auto next_project =
        ProjectEditAccess::remove_sequence(project, remove.sequence_id, identity_changes);
    if (!next_project)
        return runtime::Err(model_failure(transaction, command, next_project.error()));
    return runtime::Ok(SequenceCommandReduction{
        std::move(next_project).value(),
        InsertSequence{removed},
        {remove.sequence_id, {}, remove.sequence_id, DirtyFlags::Structure | DirtyFlags::Removed}});
}

runtime::Result<SequenceCommandReduction, TransactionError>
reduce_set_reference(const Project& project, const SetClipSequenceRef& set_reference,
                     const Transaction& transaction, CommandId command) {
    if (const auto code =
            target_error(project, set_reference.clip_id,
                         expected_location(ItemKind::Clip, project, set_reference.sequence_id,
                                           set_reference.track_id, set_reference.clip_id)))
        return reject_reduction<SequenceCommandReduction>(
            *code, transaction, command, set_reference.clip_id, set_reference.track_id);
    const auto* sequence = project.find_sequence(set_reference.sequence_id);
    const auto* track = sequence ? sequence->find_track(set_reference.track_id) : nullptr;
    const auto* clip = track ? track->find_clip(set_reference.clip_id) : nullptr;
    const auto* current = clip ? std::get_if<SequenceRef>(&clip->content()) : nullptr;
    if (!current)
        return reject_reduction<SequenceCommandReduction>(
            ConflictCode::WrongTargetKind, transaction, command, set_reference.clip_id);
    if (*current != set_reference.expected)
        return reject_reduction<SequenceCommandReduction>(
            ConflictCode::ExpectedValueMismatch, transaction, command, set_reference.clip_id);
    if (!project.find_sequence(set_reference.replacement.sequence_id))
        return runtime::Err(model_failure(transaction, command,
                                          ModelError{ModelErrorCode::MissingSequenceReference,
                                                     set_reference.sequence_id,
                                                     set_reference.replacement.sequence_id}));
    if (current->sequence_id != set_reference.replacement.sequence_id)
        if (const auto graph_error =
                validate_sequence_edge(project.sequences(), set_reference.sequence_id,
                                       set_reference.replacement.sequence_id))
            return runtime::Err(model_failure(transaction, command, *graph_error));
    auto next_clip = clip->with_content(set_reference.replacement);
    if (!next_clip)
        return runtime::Err(model_failure(transaction, command, next_clip.error()));
    auto next_track = track->replace_clip(std::move(next_clip).value());
    if (!next_track)
        return runtime::Err(model_failure(transaction, command, next_track.error()));
    auto next_sequence = sequence->replace_track(std::move(next_track).value());
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    auto next_project =
        ProjectEditAccess::replace_sequence(project, std::move(next_sequence).value());
    if (!next_project)
        return runtime::Err(model_failure(transaction, command, next_project.error()));
    return runtime::Ok(SequenceCommandReduction{
        std::move(next_project).value(),
        SetClipSequenceRef{set_reference.sequence_id, set_reference.track_id, set_reference.clip_id,
                           set_reference.replacement, set_reference.expected},
        {set_reference.clip_id, set_reference.track_id, set_reference.sequence_id,
         DirtyFlags::Content | DirtyFlags::Structure}});
}

} // namespace

bool is_sequence_command(const Command& command) noexcept {
    return std::holds_alternative<InsertSequence>(command) ||
           std::holds_alternative<CloneSequence>(command) ||
           std::holds_alternative<RemoveSequence>(command) ||
           std::holds_alternative<SetClipSequenceRef>(command);
}

runtime::Result<SequenceCommandReduction, TransactionError>
reduce_sequence_command(const Project& project, const Command& command,
                        const Transaction& transaction, CommandId command_id,
                        bool allow_tombstone_restore) {
    if (const auto* insert = std::get_if<InsertSequence>(&command))
        return reduce_insert(project, *insert, transaction, command_id, allow_tombstone_restore);
    if (const auto* clone = std::get_if<CloneSequence>(&command))
        return reduce_clone(project, *clone, transaction, command_id, allow_tombstone_restore);
    if (const auto* remove = std::get_if<RemoveSequence>(&command))
        return reduce_remove(project, *remove, transaction, command_id);
    if (const auto* set_reference = std::get_if<SetClipSequenceRef>(&command))
        return reduce_set_reference(project, *set_reference, transaction, command_id);
    return reject_reduction<SequenceCommandReduction>(ConflictCode::ModelInvariant, transaction,
                                                      command_id);
}

} // namespace pulp::timeline::detail
