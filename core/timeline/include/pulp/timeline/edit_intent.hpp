#pragma once

/// @file edit_intent.hpp
/// Pointer-neutral editing intents and their lowering to ordinary transactions.

#include <pulp/timeline/command.hpp>

#include <optional>

namespace pulp::timeline {

/** @addtogroup timeline_editing
 * @{
 */

/// What an editing gesture does to the document.
///
/// Each verb lowers to a command that already exists; there is deliberately no
/// intent-specific command vocabulary underneath. Selection and marquee are view
/// state rather than document state, and zoom-to-range is viewport state, so none
/// of the three appear here — routing them through this type would push transient
/// UI state into the undo history.
enum class EditIntentKind : std::uint8_t {
    Draw,    ///< Create a clip. Lowers to InsertClip.
    Erase,   ///< Delete a clip. Lowers to RemoveClip.
    Move,    ///< Reposition a clip. Lowers to MoveClip.
    Resize,  ///< Change a clip's extent. Lowers to MoveClip with a changed range.
};

/// One editing step expressed without reference to the device that produced it.
///
/// Carries no coordinates, no button, no pointer id and no modifiers: a front-end
/// resolves those against its hit metrics BEFORE building an intent, so mouse,
/// touch and pen converge on identical values here. That neutrality holds by
/// module layering rather than by convention — `pulp-timeline` links only
/// `pulp::runtime` and `pulp::timebase`, so this header cannot name a view-layer
/// pointer type even by accident.
///
/// `phase` is `pulp::timeline::GesturePhase`, the same enum the transaction it
/// lowers to carries, so a gesture keeps one phase vocabulary end to end.
struct EditIntent {
    EditIntentKind kind = EditIntentKind::Move;
    GesturePhase phase = GesturePhase::Single;

    ItemId sequence_id;
    ItemId track_id;
    /// Target for Erase / Move / Resize. Draw supplies `clip` instead.
    ItemId clip_id;

    /// Optimistic gate for Move / Resize: the range the front-end believes it edits.
    std::optional<ClipTimeRange> expected_range;
    /// Requested range for Move / Resize.
    std::optional<ClipTimeRange> replacement_range;
    /// Payload for Draw.
    std::optional<Clip> clip;
};

/// Identities a lowered transaction needs and an intent deliberately does not carry.
///
/// Keeping these out of the intent is what lets one intent value be lowered twice
/// — once per input device in a parity test, or once per retry after a stale
/// revision — without the intent itself being rewritten.
struct EditIntentIdentity {
    TransactionId transaction_id;
    DocumentRevision expected_revision;
    CommandId command_id;
    std::optional<UndoGroupId> undo_group;
};

/// Lowers one intent to the ordinary transaction that performs it.
///
/// Pure: the intent already names its target, so no project lookup is required and
/// the optimistic range gates pass through to the reducer unchanged. A non-`Single`
/// phase requires a valid undo group owned by the transaction's writer, matching
/// what DocumentSession admits, so a malformed gesture is rejected here instead of
/// at commit.
runtime::Result<Transaction, ModelError>
lower_edit_intent(const EditIntent& intent, const EditIntentIdentity& identity);

/// @}

} // namespace pulp::timeline
