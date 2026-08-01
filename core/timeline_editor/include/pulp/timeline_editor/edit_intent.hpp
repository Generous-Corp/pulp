#pragma once

/// @file edit_intent.hpp
/// Pointer-neutral editing intents and their lowering to ordinary transactions.

#include <pulp/timeline/command.hpp>
#include <pulp/timeline_editor/sequencer_ui_host.hpp>

#include <optional>

namespace pulp::timeline_editor {

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
    Draw,   ///< Create a clip. Lowers to InsertClip.
    Erase,  ///< Delete a clip. Lowers to RemoveClip.
    Move,   ///< Reposition a clip. Lowers to MoveClip.
    Resize, ///< Change a clip's extent. Lowers to MoveClip with a changed range.
};

/// One editing step expressed without reference to the device that produced it.
///
/// Carries no coordinates, no button, no pointer id and no modifiers: a front-end
/// resolves those against its hit metrics BEFORE building an intent, so mouse,
/// touch and pen converge on identical values here.
///
/// Device neutrality is not what puts these verbs in this module. The editor rung
/// and the document model are barred from `view` by the same MODULE_FLOORS closure,
/// so "this header cannot name a pointer type" is true at either address and
/// therefore selects neither. What selects is the opposite direction:
/// `core/timeline`'s floor excludes `timeline_editor`, so with the verbs declared
/// HERE the floor gate rejects a reducer, a migration, or a serializer that reaches
/// for one. Declared in the document model the same verbs sit in every consumer's
/// include path with nothing able to object — a headless importer, a `.pulpgraph`
/// loader, and a plugin that wants only commands would each carry a vocabulary
/// whose Move/Resize split exists solely because a front-end distinguishes a clip
/// body from its edge.
///
/// `phase` is `pulp::timeline::GesturePhase`, the same enum the transaction it
/// lowers to carries, so a gesture keeps one phase vocabulary end to end. It stays
/// in the document model on the same test: an undo group opens and closes on a
/// bracket whether or not an editor is what produced it.
struct EditIntent {
    EditIntentKind kind = EditIntentKind::Move;
    timeline::GesturePhase phase = timeline::GesturePhase::Single;

    timeline::ItemId sequence_id;
    timeline::ItemId track_id;
    /// Target for Erase / Move / Resize. Draw supplies `clip` instead.
    timeline::ItemId clip_id;

    /// Optimistic gate for Move / Resize: the range the front-end believes it edits.
    std::optional<timeline::ClipTimeRange> expected_range;
    /// Requested range for Move / Resize.
    std::optional<timeline::ClipTimeRange> replacement_range;
    /// Payload for Draw.
    std::optional<timeline::Clip> clip;

    bool operator==(const EditIntent& other) const noexcept;
};

/// What a piano-roll gesture does to one note.
///
/// This vocabulary is separate from EditIntentKind because note lowering depends
/// on the granular note commands. A view and a ScriptedUiHost can still exchange
/// and compare the complete edit before those commands are available; no note
/// verb pretends to lower to a clip command in the meantime.
enum class NoteEditIntentKind : std::uint8_t {
    Insert,      ///< Add `replacement` to the target MIDI clip.
    Erase,       ///< Remove `expected` from the target MIDI clip.
    Move,        ///< Replace a note after changing its start and/or pitch.
    Resize,      ///< Replace a note after changing its start and/or duration.
    SetVelocity, ///< Replace a note after changing its velocity.
};

/// One validated note editing step, independent of pointer or view geometry.
///
/// `sequence_id`, `track_id`, and `clip_id` locate the owning MIDI clip. Insert
/// carries only `replacement`, Erase carries only `expected`, and the three
/// transforms carry both with the same note identity. The values are snapshots,
/// not references into MidiContent, so a host may retain the intent safely.
struct NoteEditIntent {
    NoteEditIntentKind kind = NoteEditIntentKind::Move;
    timeline::GesturePhase phase = timeline::GesturePhase::Single;

    timeline::ItemId sequence_id;
    timeline::ItemId track_id;
    timeline::ItemId clip_id;

    std::optional<timeline::NoteEvent> expected;
    std::optional<timeline::NoteEvent> replacement;

    bool operator==(const NoteEditIntent& other) const noexcept;
};

/// Returns the first structural or note-domain error, or nullopt when valid.
///
/// This checks only the device-neutral intent contract. Project membership and
/// optimistic concurrency remain the future transaction builder's responsibility.
std::optional<timeline::ModelError>
validate_note_edit_intent(const NoteEditIntent& intent) noexcept;

/// Host binding used by a piano-roll front-end before command lowering.
using NoteEditIntentHost = SequencerUiHostT<NoteEditIntent>;

/// Identities a lowered transaction needs and an intent deliberately does not carry.
///
/// Keeping these out of the intent is what lets one intent value be lowered twice
/// — once per input device in a parity test, or once per retry after a stale
/// revision — without the intent itself being rewritten.
struct EditIntentIdentity {
    timeline::TransactionId transaction_id;
    timeline::DocumentRevision expected_revision;
    timeline::CommandId command_id;
    std::optional<timeline::UndoGroupId> undo_group;
};

/// The host an editor submits its intents to.
///
/// SequencerUiHostT is parameterized so the playback seam and the intent
/// vocabulary stay free to evolve apart. This alias is the binding that gives the
/// parameter a concrete meaning; without one the template is only ever
/// instantiated with a test stand-in, and an abstraction with no real
/// instantiation is indistinguishable from an unused one. It is declared beside
/// the vocabulary rather than in a header of its own so exactly one place says
/// which vocabulary an editor submits, while the template itself stays ignorant
/// of that answer.
using EditIntentHost = SequencerUiHostT<EditIntent>;

/// Lowers one intent to the ordinary transaction that performs it.
///
/// Pure: the intent already names its target, so no project lookup is required and
/// the optimistic range gates pass through to the reducer unchanged. A non-`Single`
/// phase requires a valid undo group owned by the transaction's writer, matching
/// what DocumentSession admits, so a malformed gesture is rejected here instead of
/// at commit.
runtime::Result<timeline::Transaction, timeline::ModelError>
lower_edit_intent(const EditIntent& intent, const EditIntentIdentity& identity);

/// @}

} // namespace pulp::timeline_editor
