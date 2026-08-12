#include <pulp/timeline_editor/track_edit_intent.hpp>

namespace pulp::timeline_editor {

using namespace pulp::timeline;

namespace {

template <typename... Callables>
struct Overloaded : Callables... {
    using Callables::operator()...;
};

template <typename... Callables>
Overloaded(Callables...) -> Overloaded<Callables...>;

} // namespace

runtime::Result<Transaction, ModelError>
lower_track_edit_intent(const TrackEditIntent& intent, const EditIntentIdentity& identity) {
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
    // A destination that is absent means last position, which is a request. A
    // destination that is present and structurally invalid is a malformed
    // gesture, and the two are worth distinguishing here rather than letting an
    // id the front-end never resolved reach the reducer as a missing item.
    if ((intent.expected_before_track_id && !intent.expected_before_track_id->valid()) ||
        (intent.replacement_before_track_id && !intent.replacement_before_track_id->valid()))
        return invalid(ModelErrorCode::InvalidItemId, intent.track_id, intent.sequence_id);

    Transaction result;
    result.id = identity.transaction_id;
    result.expected_revision = identity.expected_revision;
    result.undo_group = identity.undo_group;
    result.gesture_phase = intent.phase;

    switch (intent.kind) {
    case TrackEditIntentKind::Reorder:
        result.commands.push_back({identity.command_id,
                                   MoveTrack{intent.sequence_id, intent.track_id,
                                             intent.expected_before_track_id,
                                             intent.replacement_before_track_id}});
        break;
    default:
        return invalid(ModelErrorCode::InvalidItemId, intent.track_id, intent.sequence_id);
    }
    return runtime::Result<Transaction, ModelError>(runtime::Ok(std::move(result)));
}

runtime::Result<Transaction, ModelError>
lower_track_create_intent(const TrackCreateIntent& intent, const EditIntentIdentity& identity) {
    const auto invalid = [&](ModelErrorCode code, ItemId item = {}, ItemId related = {}) {
        return runtime::Result<Transaction, ModelError>(
            runtime::Err(ModelError{code, item, related}));
    };

    if (!identity.transaction_id.valid() || !identity.command_id.valid() ||
        identity.transaction_id.writer != identity.command_id.writer)
        return invalid(ModelErrorCode::InvalidItemId);
    if (!intent.sequence_id.valid())
        return invalid(ModelErrorCode::InvalidItemId);
    if (intent.before_track_id && !intent.before_track_id->valid())
        return invalid(ModelErrorCode::InvalidItemId, intent.track.id(), intent.sequence_id);

    Transaction result;
    result.id = identity.transaction_id;
    result.expected_revision = identity.expected_revision;
    result.undo_group = identity.undo_group;
    result.gesture_phase = GesturePhase::Single;
    result.commands.push_back(
        {identity.command_id, InsertTrack{intent.sequence_id, intent.track, intent.before_track_id}});
    return runtime::Result<Transaction, ModelError>(runtime::Ok(std::move(result)));
}

runtime::Result<Transaction, ModelError>
lower_track_arrangement_intent(const TrackArrangementIntent& intent,
                               const EditIntentIdentity& identity) {
    return std::visit(
        Overloaded{
            [&](const TrackEditIntent& reorder) {
                return lower_track_edit_intent(reorder, identity);
            },
            [&](const TrackCreateIntent& create) {
                return lower_track_create_intent(create, identity);
            }},
        intent);
}

} // namespace pulp::timeline_editor
