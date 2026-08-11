#pragma once

/// @file track_edit_intent.hpp
/// Pointer-neutral track-arrangement intents and their lowering to transactions.

#include <pulp/timeline_editor/edit_intent.hpp>

#include <optional>
#include <variant>

namespace pulp::timeline_editor {

/** @addtogroup timeline_editing
 * @{
 */

/// What a track-arrangement gesture does to the document.
///
/// Separate from `EditIntentKind` rather than added to it. A clip intent names a
/// clip inside a track and carries clip time ranges; a track intent names a track
/// inside a sequence and carries an insertion point. Folding the two together
/// would mean every clip intent carries track-destination fields that are always
/// empty and every track intent carries clip ranges that are always empty, and
/// nothing in the type could say which combination is meaningful — a front-end
/// would validate by convention instead of by construction.
enum class TrackEditIntentKind : std::uint8_t {
    Reorder,  ///< Move a track to a new authored position. Lowers to MoveTrack.
};

/// One track-arrangement step expressed without reference to the device that
/// produced it.
///
/// The kind field remains the discriminator for the reorder vocabulary and
/// preserves the aggregate shape existing arrangers build. Track creation uses
/// `TrackCreateIntent` below because its complete `Track` payload must be
/// required by construction rather than added here as an optional field.
///
/// Insertion is expressed as "before this track", matching `MoveTrack`, so a
/// front-end that resolved a drop position to a neighbour does not have to
/// convert it to an index that the document would then have to interpret. A
/// `std::nullopt` destination means last position — the same meaning the command
/// gives it, deliberately not a separate "append" verb.
struct TrackEditIntent {
    TrackEditIntentKind kind = TrackEditIntentKind::Reorder;
    timeline::GesturePhase phase = timeline::GesturePhase::Single;

    timeline::ItemId sequence_id;
    /// The track being moved.
    timeline::ItemId track_id;

    /// Optimistic gate: the neighbour the front-end believes `track_id` precedes.
    std::optional<timeline::ItemId> expected_before_track_id;
    /// Requested destination: the neighbour `track_id` should precede.
    std::optional<timeline::ItemId> replacement_before_track_id;
};

/// One complete track insertion expressed without device coordinates.
///
/// `track` is a required value rather than an optional field on
/// `TrackEditIntent`, so a caller cannot request creation without supplying the
/// value that `InsertTrack` persists. Creation is a single document step and
/// therefore carries no gesture phase.
struct TrackCreateIntent {
    timeline::ItemId sequence_id;
    timeline::Track track;
    /// Requested destination. Empty means last position.
    std::optional<timeline::ItemId> before_track_id;
};

/// Reorder or create vocabulary for controls that offer both operations.
using TrackArrangementIntent = std::variant<TrackEditIntent, TrackCreateIntent>;

/// The host an arranger submits track intents to.
///
/// A second binding of `SequencerUiHostT` beside `EditIntentHost`, which is what
/// "a separate channel" means concretely: a view that only rearranges tracks
/// names this one and never acquires the clip vocabulary.
using TrackEditIntentHost = SequencerUiHostT<TrackEditIntent>;

/// The host an arranger offering reorder and creation submits intents to.
using TrackArrangementIntentHost = SequencerUiHostT<TrackArrangementIntent>;

/// Lowers one track intent to the ordinary transaction that performs it.
///
/// Pure, and validates only what a malformed *gesture* would get wrong: identity
/// agreement, the undo-group bracket a non-`Single` phase requires, and ids that
/// are structurally invalid. Whether the destination exists, whether the track is
/// in the sequence, and whether the optimistic gate still holds are the reducer's
/// to answer against a project this function does not have.
///
/// It also does not reject naming the moved track as its own destination, though
/// that request describes no position: `Sequence::move_track` already refuses it
/// with a stated reason, and a second copy of a model rule here could disagree
/// with the one the editing paths enforce.
///
/// @param intent The gesture step to lower.
/// @param identity Transaction, command, revision, and undo group for the result.
/// @return The transaction, or the reason the intent could not be lowered.
runtime::Result<timeline::Transaction, timeline::ModelError>
lower_track_edit_intent(const TrackEditIntent& intent, const EditIntentIdentity& identity);

/// Lowers one complete track-creation intent to `InsertTrack`.
///
/// Pure, and validates identity agreement plus structurally invalid sequence or
/// destination identities. Project membership and identity availability remain
/// reducer concerns. The resulting transaction always has `Single` phase.
///
/// @param intent The complete track insertion to lower.
/// @param identity Transaction, command, revision, and optional undo group.
/// @return The transaction, or the reason the intent could not be lowered.
runtime::Result<timeline::Transaction, timeline::ModelError>
lower_track_create_intent(const TrackCreateIntent& intent, const EditIntentIdentity& identity);

/// Lowers either complete track-arrangement alternative.
///
/// Dispatch is exhaustive: adding an alternative to `TrackArrangementIntent`
/// also requires adding its lowering arm.
///
/// @param intent The reorder or creation step to lower.
/// @param identity Transaction, command, revision, and optional undo group.
/// @return The transaction, or the reason the selected intent could not be lowered.
runtime::Result<timeline::Transaction, timeline::ModelError>
lower_track_arrangement_intent(const TrackArrangementIntent& intent,
                               const EditIntentIdentity& identity);

/// @}

} // namespace pulp::timeline_editor
