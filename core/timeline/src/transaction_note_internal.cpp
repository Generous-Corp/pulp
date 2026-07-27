#include "transaction_note_internal.hpp"

#include "transaction_reduction_support.hpp"

#include <algorithm>

namespace pulp::timeline::detail {
namespace {

ItemLocation expected_location(ItemKind kind, const Project& project, ItemId sequence, ItemId track,
                               ItemId clip) {
    return {kind,     immediate_parent_id(kind, project.id(), sequence, track, clip),
            sequence, track,
            clip,     true};
}

bool equal_note(const NoteEvent& lhs, const NoteEvent& rhs) noexcept {
    return lhs.id == rhs.id && lhs.start == rhs.start && lhs.duration == rhs.duration &&
           lhs.velocity == rhs.velocity && lhs.pitch == rhs.pitch && lhs.channel == rhs.channel;
}

runtime::Result<Project, TransactionError> replace_note_content(
    const Project& project, const Sequence& sequence, const Track& track, const Clip& clip,
    NoteContent content, std::span<const IdentityMutation> identities,
    std::optional<std::uint64_t> next_item_id, const Transaction& transaction, CommandId command) {
    auto next_clip = clip.with_content(std::move(content));
    if (!next_clip)
        return runtime::Err(model_failure(transaction, command, next_clip.error()));
    auto next_track = track.replace_clip(std::move(next_clip).value());
    if (!next_track)
        return runtime::Err(model_failure(transaction, command, next_track.error()));
    auto next_sequence = sequence.replace_track(std::move(next_track).value());
    if (!next_sequence)
        return runtime::Err(model_failure(transaction, command, next_sequence.error()));
    auto next_project = ProjectEditAccess::replace_sequence(
        project, std::move(next_sequence).value(), identities, next_item_id);
    if (!next_project)
        return runtime::Err(model_failure(transaction, command, next_project.error()));
    return runtime::Ok(std::move(next_project).value());
}

runtime::Result<NoteCommandReduction, TransactionError>
reduce_set_note_velocity(const Project& project, const SetNoteVelocity& velocity,
                         const Transaction& transaction, CommandId command) {
    if (const auto code =
            target_error(project, velocity.note_id,
                         expected_location(ItemKind::Note, project, velocity.sequence_id,
                                           velocity.track_id, velocity.clip_id)))
        return reject_reduction<NoteCommandReduction>(*code, transaction, command, velocity.note_id,
                                                      velocity.clip_id);
    const auto* sequence = project.find_sequence(velocity.sequence_id);
    const auto* track = sequence->find_track(velocity.track_id);
    const auto* clip = track->find_clip(velocity.clip_id);
    const auto* notes = std::get_if<NoteContent>(&clip->content());
    if (!notes)
        return reject_reduction<NoteCommandReduction>(ConflictCode::WrongTargetKind, transaction,
                                                      command, velocity.clip_id);
    const auto found =
        std::find_if(notes->notes().begin(), notes->notes().end(),
                     [&](const NoteEvent& note) { return note.id == velocity.note_id; });
    if (found == notes->notes().end())
        return reject_reduction<NoteCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                      command, velocity.note_id);
    if (found->velocity != velocity.expected_velocity)
        return reject_reduction<NoteCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                      transaction, command, velocity.note_id);

    NoteEvent replacement = *found;
    replacement.velocity = velocity.replacement_velocity;
    auto next_notes = notes->replace_note(replacement);
    if (!next_notes)
        return runtime::Err(model_failure(transaction, command, next_notes.error()));
    auto next_project =
        replace_note_content(project, *sequence, *track, *clip, std::move(next_notes).value(), {},
                             std::nullopt, transaction, command);
    if (!next_project)
        return runtime::Err(next_project.error());
    return runtime::Ok(NoteCommandReduction{
        std::move(next_project).value(),
        SetNoteVelocity{velocity.sequence_id, velocity.track_id, velocity.clip_id, velocity.note_id,
                        velocity.replacement_velocity, velocity.expected_velocity},
        {velocity.note_id, velocity.track_id, velocity.sequence_id,
         DirtyFlags::Content | DirtyFlags::Notes}});
}

runtime::Result<NoteCommandReduction, TransactionError>
reduce_replace_note_content(const Project& project, const ReplaceNoteContent& replace,
                            const Transaction& transaction, CommandId command,
                            bool allow_tombstone_restore) {
    if (const auto code =
            target_error(project, replace.clip_id,
                         expected_location(ItemKind::Clip, project, replace.sequence_id,
                                           replace.track_id, replace.clip_id)))
        return reject_reduction<NoteCommandReduction>(*code, transaction, command, replace.clip_id,
                                                      replace.track_id);
    const auto* sequence = project.find_sequence(replace.sequence_id);
    const auto* track = sequence->find_track(replace.track_id);
    const auto* clip = track->find_clip(replace.clip_id);
    const auto* notes = std::get_if<NoteContent>(&clip->content());
    if (!notes)
        return reject_reduction<NoteCommandReduction>(ConflictCode::WrongTargetKind, transaction,
                                                      command, replace.clip_id);

    auto expected_notes = NoteContent::create(replace.expected);
    if (!expected_notes)
        return runtime::Err(model_failure(transaction, command, expected_notes.error()));
    if (notes->notes().size() != expected_notes->notes().size() ||
        !std::equal(notes->notes().begin(), notes->notes().end(), expected_notes->notes().begin(),
                    equal_note))
        return reject_reduction<NoteCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                      transaction, command, replace.clip_id);

    auto next_notes = NoteContent::create(replace.replacement);
    if (!next_notes)
        return runtime::Err(model_failure(transaction, command, next_notes.error()));

    std::vector<ItemId> expected_ids;
    std::vector<ItemId> replacement_ids;
    expected_ids.reserve(expected_notes->notes().size());
    replacement_ids.reserve(next_notes->notes().size());
    for (const auto& note : expected_notes->notes())
        expected_ids.push_back(note.id);
    for (const auto& note : next_notes->notes())
        replacement_ids.push_back(note.id);
    std::sort(expected_ids.begin(), expected_ids.end());
    std::sort(replacement_ids.begin(), replacement_ids.end());

    const auto location = expected_location(ItemKind::Note, project, replace.sequence_id,
                                            replace.track_id, replace.clip_id);
    std::vector<OwnedIdentity> removed;
    std::vector<OwnedIdentity> inserted;
    for (const auto id : expected_ids)
        if (!std::binary_search(replacement_ids.begin(), replacement_ids.end(), id))
            removed.push_back({id, location});
    for (const auto id : replacement_ids)
        if (!std::binary_search(expected_ids.begin(), expected_ids.end(), id))
            inserted.push_back({id, location});

    auto identity_plan =
        plan_identity_insert(project, inserted, allow_tombstone_restore, transaction, command);
    if (!identity_plan)
        return runtime::Err(identity_plan.error());
    auto identity_changes = plan_identity_deactivate(removed);
    identity_changes.insert(identity_changes.end(), identity_plan->mutations.begin(),
                            identity_plan->mutations.end());

    const std::vector<NoteEvent> canonical_expected(expected_notes->notes().begin(),
                                                    expected_notes->notes().end());
    const std::vector<NoteEvent> canonical_replacement(next_notes->notes().begin(),
                                                       next_notes->notes().end());
    auto next_project =
        replace_note_content(project, *sequence, *track, *clip, std::move(next_notes).value(),
                             identity_changes, identity_plan->next_item_id, transaction, command);
    if (!next_project)
        return runtime::Err(next_project.error());
    return runtime::Ok(NoteCommandReduction{
        std::move(next_project).value(),
        ReplaceNoteContent{replace.sequence_id, replace.track_id, replace.clip_id,
                           canonical_replacement, canonical_expected},
        {replace.clip_id, replace.track_id, replace.sequence_id,
         DirtyFlags::Content | DirtyFlags::Notes}});
}

} // namespace

bool is_note_command(const Command& command) noexcept {
    return std::holds_alternative<SetNoteVelocity>(command) ||
           std::holds_alternative<ReplaceNoteContent>(command);
}

runtime::Result<NoteCommandReduction, TransactionError>
reduce_note_command(const Project& project, const Command& command, const Transaction& transaction,
                    CommandId command_id, bool allow_tombstone_restore) {
    if (const auto* velocity = std::get_if<SetNoteVelocity>(&command))
        return reduce_set_note_velocity(project, *velocity, transaction, command_id);
    if (const auto* replace = std::get_if<ReplaceNoteContent>(&command))
        return reduce_replace_note_content(project, *replace, transaction, command_id,
                                           allow_tombstone_restore);
    return reject_reduction<NoteCommandReduction>(ConflictCode::ModelInvariant, transaction,
                                                  command_id);
}

} // namespace pulp::timeline::detail
