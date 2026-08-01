#include <pulp/timeline_editor/edit_intent.hpp>

#include <utility>

namespace pulp::timeline_editor {

using namespace pulp::timeline;

runtime::Result<Transaction, ModelError>
lower_edit_intent(const EditIntent& intent, const EditIntentIdentity& identity) {
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
            {identity.command_id,
             RemoveClip{intent.sequence_id, intent.track_id, intent.clip_id}});
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
            {identity.command_id,
             MoveClip{intent.sequence_id, intent.track_id, intent.clip_id,
                      *intent.expected_range, *intent.replacement_range}});
        break;
    }

    return runtime::Ok(std::move(result));
}

} // namespace pulp::timeline_editor
