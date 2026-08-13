#include <pulp/timeline_editor/edit_intent.hpp>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace pulp::timeline_editor {

using namespace pulp::timeline;

namespace {

bool equal_optional_clip(const std::optional<Clip>& lhs, const std::optional<Clip>& rhs) noexcept {
    if (lhs.has_value() != rhs.has_value())
        return false;
    if (!lhs || !equivalent(*lhs, *rhs))
        return !lhs;

    const auto* left_midi = std::get_if<MidiContent>(&lhs->content());
    if (!left_midi)
        return true;
    const auto& right_midi = std::get<MidiContent>(rhs->content());
    return left_midi->lanes().size() == right_midi.lanes().size() &&
           std::equal(left_midi->lanes().begin(), left_midi->lanes().end(),
                      right_midi.lanes().begin());
}

bool equal_optional_range(const std::optional<ClipTimeRange>& lhs,
                          const std::optional<ClipTimeRange>& rhs) noexcept {
    if (lhs.has_value() != rhs.has_value())
        return false;
    return !lhs || equivalent(*lhs, *rhs);
}

constexpr bool equal_note(const NoteEvent& lhs, const NoteEvent& rhs) noexcept {
    return lhs.id == rhs.id && lhs.start == rhs.start && lhs.duration == rhs.duration &&
           lhs.velocity == rhs.velocity && lhs.pitch == rhs.pitch && lhs.channel == rhs.channel;
}

bool equal_optional_note(const std::optional<NoteEvent>& lhs,
                         const std::optional<NoteEvent>& rhs) noexcept {
    if (lhs.has_value() != rhs.has_value())
        return false;
    return !lhs || equal_note(*lhs, *rhs);
}

std::optional<ModelError> validate_note(const NoteEvent& note) noexcept {
    if (!note.id.valid())
        return ModelError{ModelErrorCode::InvalidItemId, note.id, {}};
    if (note.duration.value <= 0 ||
        note.start.value > std::numeric_limits<std::int64_t>::max() - note.duration.value ||
        note.pitch > 127 || note.channel > 15)
        return ModelError{ModelErrorCode::InvalidNote, note.id, {}};
    return std::nullopt;
}

constexpr bool valid_gesture_phase(GesturePhase phase) noexcept {
    switch (phase) {
    case GesturePhase::Single:
    case GesturePhase::Begin:
    case GesturePhase::Update:
    case GesturePhase::End:
    case GesturePhase::Cancel:
        return true;
    }
    return false;
}

} // namespace

bool EditIntent::operator==(const EditIntent& other) const noexcept {
    return kind == other.kind && phase == other.phase && sequence_id == other.sequence_id &&
           track_id == other.track_id && clip_id == other.clip_id &&
           equal_optional_range(expected_range, other.expected_range) &&
           equal_optional_range(replacement_range, other.replacement_range) &&
           equal_optional_clip(clip, other.clip);
}

bool NoteEditIntent::operator==(const NoteEditIntent& other) const noexcept {
    return kind == other.kind && phase == other.phase && sequence_id == other.sequence_id &&
           track_id == other.track_id && clip_id == other.clip_id &&
           equal_optional_note(expected, other.expected) &&
           equal_optional_note(replacement, other.replacement);
}

std::optional<ModelError> validate_note_edit_intent(const NoteEditIntent& intent) noexcept {
    if (!intent.sequence_id.valid())
        return ModelError{ModelErrorCode::InvalidItemId, intent.sequence_id, {}};
    if (!intent.track_id.valid())
        return ModelError{ModelErrorCode::InvalidItemId, intent.track_id, {}};
    if (!intent.clip_id.valid())
        return ModelError{ModelErrorCode::InvalidItemId, intent.clip_id, {}};
    if (!valid_gesture_phase(intent.phase))
        return ModelError{ModelErrorCode::InvalidNote, intent.clip_id, {}};
    if (intent.expected) {
        if (auto error = validate_note(*intent.expected))
            return error;
    }
    if (intent.replacement) {
        if (auto error = validate_note(*intent.replacement))
            return error;
    }

    const auto missing = [&]() {
        return std::optional<ModelError>(
            ModelError{ModelErrorCode::MissingItem, intent.clip_id, {}});
    };
    const auto unexpected = [&]() {
        return std::optional<ModelError>(
            ModelError{ModelErrorCode::InvalidNote, intent.clip_id, {}});
    };
    const auto invalid_transform = [&]() {
        return std::optional<ModelError>(
            ModelError{ModelErrorCode::InvalidNote, intent.expected->id, {}});
    };

    switch (intent.kind) {
    case NoteEditIntentKind::Insert:
        if (intent.expected)
            return unexpected();
        return intent.replacement ? std::nullopt : missing();
    case NoteEditIntentKind::Erase:
        if (intent.replacement)
            return unexpected();
        return intent.expected ? std::nullopt : missing();
    case NoteEditIntentKind::Move: {
        if (!intent.expected || !intent.replacement)
            return missing();
        if (intent.expected->id != intent.replacement->id)
            return ModelError{ModelErrorCode::IdentityConflict, intent.expected->id,
                              intent.replacement->id};
        const auto& before = *intent.expected;
        const auto& after = *intent.replacement;
        if (before.duration != after.duration || before.velocity != after.velocity ||
            before.channel != after.channel ||
            (before.start == after.start && before.pitch == after.pitch))
            return invalid_transform();
        return std::nullopt;
    }
    case NoteEditIntentKind::Resize: {
        if (!intent.expected || !intent.replacement)
            return missing();
        if (intent.expected->id != intent.replacement->id)
            return ModelError{ModelErrorCode::IdentityConflict, intent.expected->id,
                              intent.replacement->id};
        const auto& before = *intent.expected;
        const auto& after = *intent.replacement;
        if (before.pitch != after.pitch || before.velocity != after.velocity ||
            before.channel != after.channel ||
            (before.start == after.start && before.duration == after.duration))
            return invalid_transform();
        return std::nullopt;
    }
    case NoteEditIntentKind::SetVelocity: {
        if (!intent.expected || !intent.replacement)
            return missing();
        if (intent.expected->id != intent.replacement->id)
            return ModelError{ModelErrorCode::IdentityConflict, intent.expected->id,
                              intent.replacement->id};
        const auto& before = *intent.expected;
        const auto& after = *intent.replacement;
        if (before.start != after.start || before.duration != after.duration ||
            before.pitch != after.pitch || before.channel != after.channel ||
            before.velocity == after.velocity)
            return invalid_transform();
        return std::nullopt;
    }
    }
    return unexpected();
}

runtime::Result<ValidatedNoteEditIntent, ModelError>
ValidatedNoteEditIntent::create(NoteEditIntent intent) noexcept {
    if (auto error = validate_note_edit_intent(intent))
        return runtime::Result<ValidatedNoteEditIntent, ModelError>(runtime::Err(*error));
    return runtime::Ok(ValidatedNoteEditIntent(std::move(intent)));
}

runtime::Result<Transaction, ModelError> lower_edit_intent(const EditIntent& intent,
                                                           const EditIntentIdentity& identity) {
    const auto invalid = [&](ModelErrorCode code, ItemId item = {}, ItemId related = {}) {
        return runtime::Result<Transaction, ModelError>(
            runtime::Err(ModelError{code, item, related}));
    };

    if (!identity.transaction_id.valid() || !identity.command_id.valid() ||
        identity.transaction_id.writer != identity.command_id.writer)
        return invalid(ModelErrorCode::InvalidItemId);
    // Mirrors the gesture admission rule so a malformed bracket fails before it
    // reaches a session that would reject it with a conflict instead.
    if (intent.phase != GesturePhase::Single &&
        (!identity.undo_group || !identity.undo_group->valid() ||
         identity.undo_group->writer != identity.transaction_id.writer))
        return invalid(ModelErrorCode::InvalidItemId);
    if (!intent.sequence_id.valid() || !intent.track_id.valid())
        return invalid(ModelErrorCode::InvalidItemId);

    Transaction result;
    result.id = identity.transaction_id;
    result.expected_revision = identity.expected_revision;
    result.undo_group = identity.undo_group;
    result.gesture_phase = intent.phase;

    switch (intent.kind) {
    case EditIntentKind::Draw:
        if (!intent.clip)
            return invalid(ModelErrorCode::MissingItem, intent.track_id);
        result.commands.push_back(
            {identity.command_id, InsertClip{intent.sequence_id, intent.track_id, *intent.clip}});
        break;
    case EditIntentKind::Erase:
        if (!intent.clip_id.valid())
            return invalid(ModelErrorCode::MissingItem, intent.track_id);
        result.commands.push_back(
            {identity.command_id, RemoveClip{intent.sequence_id, intent.track_id, intent.clip_id}});
        break;
    // Move and Resize lower to the same command on purpose: a resize IS a move
    // whose replacement range changes extent rather than position. The verbs stay
    // distinct because a front-end distinguishes them at hit-test time — grabbing
    // a clip body versus its edge — not because the document needs two commands.
    case EditIntentKind::Move:
    case EditIntentKind::Resize:
        if (!intent.clip_id.valid())
            return invalid(ModelErrorCode::MissingItem, intent.track_id);
        if (!intent.expected_range || !intent.replacement_range)
            return invalid(ModelErrorCode::InvalidMediaRange, intent.clip_id);
        result.commands.push_back(
            {identity.command_id, MoveClip{intent.sequence_id, intent.track_id, intent.clip_id,
                                           *intent.expected_range, *intent.replacement_range}});
        break;
    }

    return runtime::Ok(std::move(result));
}


namespace {

/// Position of a note identity in a canonical note array, if it is there.
std::optional<std::size_t> index_of_note(std::span<const NoteEvent> notes, ItemId id) noexcept {
    for (std::size_t index = 0; index < notes.size(); ++index)
        if (notes[index].id == id)
            return index;
    return std::nullopt;
}

} // namespace

runtime::Result<Transaction, NoteLoweringError>
lower_note_edit_intent(const ValidatedNoteEditIntent& validated,
                       std::span<const NoteEvent> current_notes,
                       const EditIntentIdentity& identity) {
    const auto refuse = [](NoteLoweringError error) {
        return runtime::Result<Transaction, NoteLoweringError>(runtime::Err(error));
    };

    if (!identity.transaction_id.valid() || !identity.command_id.valid() ||
        identity.transaction_id.writer != identity.command_id.writer)
        return refuse(NoteLoweringError::InvalidIdentity);

    const auto& intent = validated.value();
    if (intent.phase != GesturePhase::Single &&
        (!identity.undo_group || !identity.undo_group->valid() ||
         identity.undo_group->writer != identity.transaction_id.writer))
        return refuse(NoteLoweringError::InvalidIdentity);

    if (intent.kind == NoteEditIntentKind::Insert) {
        if (index_of_note(current_notes, intent.replacement->id))
            return refuse(NoteLoweringError::DuplicateNoteIdentity);
    } else {
        const auto found = index_of_note(current_notes, intent.expected->id);
        if (!found)
            return refuse(NoteLoweringError::NoteNotInClip);
        // The optimistic gate the caller believes it is editing under. Checking
        // it here turns a stale view into a named refusal instead of an
        // ExpectedValueMismatch the caller has to decode after the fact.
        if (!equal_note(current_notes[*found], *intent.expected))
            return refuse(NoteLoweringError::ExpectedNoteMismatch);
    }

    Transaction result;
    result.id = identity.transaction_id;
    result.expected_revision = identity.expected_revision;
    result.undo_group = identity.undo_group;
    result.gesture_phase = intent.phase;
    if (intent.kind == NoteEditIntentKind::Insert) {
        result.commands.push_back(
            {identity.command_id,
             InsertNotes{intent.sequence_id, intent.track_id, intent.clip_id,
                         {*intent.replacement}, {}}});
    } else if (intent.kind == NoteEditIntentKind::Erase) {
        result.commands.push_back(
            {identity.command_id,
             RemoveNotes{intent.sequence_id, intent.track_id, intent.clip_id,
                         {*intent.expected}}});
    } else {
        result.commands.push_back(
            {identity.command_id,
             SetNoteEvents{intent.sequence_id, intent.track_id, intent.clip_id,
                           {*intent.expected}, {*intent.replacement}}});
    }
    return runtime::Ok(std::move(result));
}

} // namespace pulp::timeline_editor
