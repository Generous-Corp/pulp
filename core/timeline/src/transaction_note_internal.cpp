#include "transaction_note_internal.hpp"

#include "transaction_dispatch_internal.hpp"
#include "transaction_reduction_support.hpp"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <utility>
#include <variant>
#include <vector>

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
    MidiContent content, std::span<const IdentityMutation> identities,
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
    const auto* notes = std::get_if<MidiContent>(&clip->content());
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
reduce_set_note_events(const Project& project, const SetNoteEvents& set,
                       const Transaction& transaction, CommandId command) {
    if (const auto code = target_error(project, set.clip_id,
                                       expected_location(ItemKind::Clip, project, set.sequence_id,
                                                         set.track_id, set.clip_id)))
        return reject_reduction<NoteCommandReduction>(*code, transaction, command, set.clip_id,
                                                      set.track_id);
    const auto* sequence = project.find_sequence(set.sequence_id);
    const auto* track = sequence->find_track(set.track_id);
    const auto* clip = track->find_clip(set.clip_id);
    const auto* notes = std::get_if<MidiContent>(&clip->content());
    if (!notes)
        return reject_reduction<NoteCommandReduction>(ConflictCode::WrongTargetKind, transaction,
                                                      command, set.clip_id);

    // The two arrays are one edit read twice: entry i says what note i is now
    // and what it becomes. A payload naming no note is not an edit; arrays of
    // unequal length cannot be paired at all; and a pair whose halves name
    // different notes would give the inverse a note the forward edit never
    // touched.
    if (set.expected.empty() || set.expected.size() != set.replacement.size())
        return reject_reduction<NoteCommandReduction>(ConflictCode::ModelInvariant, transaction,
                                                      command, set.clip_id);
    for (std::size_t i = 0; i < set.expected.size(); ++i)
        if (set.expected[i].id != set.replacement[i].id)
            return reject_reduction<NoteCommandReduction>(ConflictCode::ModelInvariant, transaction,
                                                          command, set.expected[i].id, set.clip_id);

    // Naming one note twice would apply both entries in payload order and leave
    // the inverse unable to say which of the two values to restore.
    std::vector<ItemId> named;
    named.reserve(set.expected.size());
    for (const auto& note : set.expected)
        named.push_back(note.id);
    std::sort(named.begin(), named.end());
    if (const auto duplicate = std::adjacent_find(named.begin(), named.end());
        duplicate != named.end())
        return reject_reduction<NoteCommandReduction>(ConflictCode::ModelInvariant, transaction,
                                                      command, *duplicate, set.clip_id);

    // Notes are stored in (start, id) order, so finding one by identity needs
    // its own index. Building that index once costs one sort of the clip and
    // then a binary search per named note, rather than a scan of the clip per
    // named note — the quadratic term this command exists to avoid on a drag
    // over a large clip.
    std::vector<std::pair<ItemId, std::size_t>> by_id;
    by_id.reserve(notes->notes().size());
    for (std::size_t i = 0; i < notes->notes().size(); ++i)
        by_id.emplace_back(notes->notes()[i].id, i);
    std::sort(by_id.begin(), by_id.end());

    const auto note_location =
        expected_location(ItemKind::Note, project, set.sequence_id, set.track_id, set.clip_id);
    std::vector<NoteEvent> next(notes->notes().begin(), notes->notes().end());
    for (std::size_t i = 0; i < set.expected.size(); ++i) {
        const auto& gate = set.expected[i];
        if (const auto code = target_error(project, gate.id, note_location))
            return reject_reduction<NoteCommandReduction>(*code, transaction, command, gate.id,
                                                          set.clip_id);
        const auto found = std::lower_bound(
            by_id.begin(), by_id.end(), gate.id,
            [](const std::pair<ItemId, std::size_t>& entry, ItemId id) { return entry.first < id; });
        if (found == by_id.end() || found->first != gate.id)
            return reject_reduction<NoteCommandReduction>(ConflictCode::TargetMissing, transaction,
                                                          command, gate.id, set.clip_id);
        // The gate reads the clip as it was. Identities are unique and each is
        // named once, so no two iterations write the same slot and an earlier
        // replacement can never satisfy a later note's gate.
        if (!equal_note(notes->notes()[found->second], gate))
            return reject_reduction<NoteCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                          transaction, command, gate.id,
                                                          set.clip_id);
        next[found->second] = set.replacement[i];
    }

    // Modifiers, the seed their probability draws derive from, and the
    // controller and expression lanes all carry across whole. Neither is
    // filtered here, and neither should be: this command's identity set is
    // invariant, so no modifier can be left keying a note that is gone. The
    // surviving-note filter in reduce_replace_note_content answers a question
    // this command cannot ask, and copying it here would only ever find notes
    // to keep — while the same edit applied to lanes, which key on a
    // MidiLaneAddress naming no note, deletes a clip's controller streams.
    auto next_notes = MidiContent::create(
        std::move(next),
        std::vector<NoteModifier>(notes->modifiers().begin(), notes->modifiers().end()),
        notes->modifier_seed(),
        std::vector<MidiExpressionLane>(notes->lanes().begin(), notes->lanes().end()));
    if (!next_notes)
        return runtime::Err(model_failure(transaction, command, next_notes.error()));

    // The inverse is this edit read backwards, so both its arrays need one
    // agreed order. Ordering by note id makes a payload's inverse independent of
    // the order its author happened to list the notes in.
    std::vector<std::size_t> order(set.expected.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
        return set.expected[lhs].id < set.expected[rhs].id;
    });
    std::vector<NoteEvent> inverse_expected;
    std::vector<NoteEvent> inverse_replacement;
    inverse_expected.reserve(order.size());
    inverse_replacement.reserve(order.size());
    for (const auto index : order) {
        inverse_expected.push_back(set.replacement[index]);
        inverse_replacement.push_back(set.expected[index]);
    }

    // No identity mutations and no identity allocation: every note the command
    // names is a note the clip already owned and still owns.
    auto next_project =
        replace_note_content(project, *sequence, *track, *clip, std::move(next_notes).value(), {},
                             std::nullopt, transaction, command);
    if (!next_project)
        return runtime::Err(next_project.error());
    return runtime::Ok(NoteCommandReduction{
        std::move(next_project).value(),
        SetNoteEvents{set.sequence_id, set.track_id, set.clip_id, std::move(inverse_expected),
                      std::move(inverse_replacement)},
        {set.clip_id, set.track_id, set.sequence_id,
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
    const auto* notes = std::get_if<MidiContent>(&clip->content());
    if (!notes)
        return reject_reduction<NoteCommandReduction>(ConflictCode::WrongTargetKind, transaction,
                                                      command, replace.clip_id);

    auto expected_notes = MidiContent::create(replace.expected);
    if (!expected_notes)
        return runtime::Err(model_failure(transaction, command, expected_notes.error()));
    if (notes->notes().size() != expected_notes->notes().size() ||
        !std::equal(notes->notes().begin(), notes->notes().end(), expected_notes->notes().begin(),
                    equal_note))
        return reject_reduction<NoteCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                      transaction, command, replace.clip_id);

    // Canonicalization reorders notes and never adds or removes one — a
    // duplicate identity fails `create` outright — so the payload's identities
    // are already the identities the rebuilt content will own.
    std::vector<ItemId> replacement_ids;
    replacement_ids.reserve(replace.replacement.size());
    for (const auto& note : replace.replacement)
        replacement_ids.push_back(note.id);
    std::sort(replacement_ids.begin(), replacement_ids.end());

    // A modifier keys on a note identity, so one whose note the replacement
    // drops has nothing left to key to and goes with it; `create` rejects a
    // modifier naming a note the content does not carry. An expression lane
    // keys on a MidiLaneAddress — a channel-voice stream address that names no
    // note — so lanes pass through whole. Filtering them by the surviving note
    // ids as well would look consistent and would delete a clip's controller
    // streams the moment an edit removes a note.
    std::vector<NoteModifier> surviving_modifiers;
    surviving_modifiers.reserve(notes->modifiers().size());
    for (const auto& modifier : notes->modifiers())
        if (std::binary_search(replacement_ids.begin(), replacement_ids.end(), modifier.note_id))
            surviving_modifiers.push_back(modifier);

    // A payload that names the modifiers it expects gates them the same way the
    // note arrays gate the notes: an inverse restoring a removed note's modifier
    // must not overwrite a modifier set someone else has since changed. An empty
    // array is how an authoring caller spells "unstated", so it gates nothing.
    if (!replace.expected_modifiers.empty()) {
        std::vector<NoteModifier> expected_modifiers = replace.expected_modifiers;
        std::sort(expected_modifiers.begin(), expected_modifiers.end(),
                  [](const NoteModifier& lhs, const NoteModifier& rhs) {
                      return lhs.note_id < rhs.note_id;
                  });
        if (notes->modifiers().size() != expected_modifiers.size() ||
            !std::equal(notes->modifiers().begin(), notes->modifiers().end(),
                        expected_modifiers.begin()))
            return reject_reduction<NoteCommandReduction>(ConflictCode::ExpectedValueMismatch,
                                                          transaction, command, replace.clip_id);
    }

    // The command's payload is notes, so everything else the clip authors
    // carries across: the modifiers, the seed their probability draws are
    // derived from, and the controller and expression lanes. Rebuilding from
    // the notes alone would silently erase all three. A payload that states its
    // own modifiers replaces the surviving set outright rather than adding to
    // it — that is how an inverse reinstates the modifier of a note this edit's
    // replacement brings back, which no filter over live content can recover.
    std::vector<NoteModifier> next_modifiers = replace.replacement_modifiers;
    if (next_modifiers.empty())
        next_modifiers = std::move(surviving_modifiers);
    auto next_notes = MidiContent::create(
        replace.replacement, std::move(next_modifiers), notes->modifier_seed(),
        std::vector<MidiExpressionLane>(notes->lanes().begin(), notes->lanes().end()));
    if (!next_notes)
        return runtime::Err(model_failure(transaction, command, next_notes.error()));

    std::vector<ItemId> expected_ids;
    expected_ids.reserve(expected_notes->notes().size());
    for (const auto& note : expected_notes->notes())
        expected_ids.push_back(note.id);
    std::sort(expected_ids.begin(), expected_ids.end());

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
    // The inverse names both modifier sets outright. Restoring the notes alone
    // would leave a removed note's modifier unrecoverable: it is gone from the
    // content the inverse reduces against, so no filter over live state can
    // bring it back.
    const std::vector<NoteModifier> modifiers_before(notes->modifiers().begin(),
                                                     notes->modifiers().end());
    const std::vector<NoteModifier> modifiers_after(next_notes->modifiers().begin(),
                                                    next_notes->modifiers().end());
    auto next_project =
        replace_note_content(project, *sequence, *track, *clip, std::move(next_notes).value(),
                             identity_changes, identity_plan->next_item_id, transaction, command);
    if (!next_project)
        return runtime::Err(next_project.error());
    return runtime::Ok(NoteCommandReduction{
        std::move(next_project).value(),
        ReplaceNoteContent{replace.sequence_id, replace.track_id, replace.clip_id,
                           canonical_replacement, canonical_expected, modifiers_after,
                           modifiers_before},
        {replace.clip_id, replace.track_id, replace.sequence_id,
         DirtyFlags::Content | DirtyFlags::Notes}});
}

} // namespace

bool is_note_command(const Command& command) noexcept {
    return std::visit([]<typename T>(const T&) { return is_note_command_type<T>; }, command);
}

runtime::Result<NoteCommandReduction, TransactionError>
reduce_note_command(const Project& project, const Command& command, const Transaction& transaction,
                    CommandId command_id, bool allow_tombstone_restore) {
    // The family predicate above this call and the arms below it are two
    // statements of the same list, and a chain of get_if proves nothing about
    // its own coverage — so a command added to is_note_command_type without an
    // arm here used to fall through to a runtime ModelInvariant. Visiting
    // instead puts that case in front of the compiler, which is where the outer
    // dispatch already resolves the same question.
    return std::visit(
        [&]<typename T>(const T& value) -> runtime::Result<NoteCommandReduction, TransactionError> {
            if constexpr (std::is_same_v<T, SetNoteVelocity>)
                return reduce_set_note_velocity(project, value, transaction, command_id);
            else if constexpr (std::is_same_v<T, ReplaceNoteContent>)
                return reduce_replace_note_content(project, value, transaction, command_id,
                                                   allow_tombstone_restore);
            else if constexpr (std::is_same_v<T, SetNoteEvents>)
                return reduce_set_note_events(project, value, transaction, command_id);
            else {
                static_assert(!is_note_command_type<T>,
                              "a note command claimed in transaction_dispatch_internal.hpp has no "
                              "arm here; add one, or drop it from is_note_command_type");
                // Reached only by an alternative no family claims, which the
                // caller's predicate already excludes. Kept as a rejection
                // rather than std::unreachable(): this TU is -fno-exceptions,
                // so being wrong here would abort the process instead of
                // failing one transaction.
                return reject_reduction<NoteCommandReduction>(ConflictCode::ModelInvariant,
                                                              transaction, command_id);
            }
        },
        command);
}

} // namespace pulp::timeline::detail
