#pragma once

/// @file edit_intent.hpp
/// Pointer-neutral editing intents and their lowering to ordinary transactions.

#include <pulp/timeline/command.hpp>
#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline_editor/sequencer_ui_host.hpp>

#include <optional>
#include <span>
#include <utility>

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

/// A note intent that passed validate_note_edit_intent and may cross a host seam.
class ValidatedNoteEditIntent {
  public:
    /// Validates and owns one complete intent, or returns its first error.
    static runtime::Result<ValidatedNoteEditIntent, timeline::ModelError>
    create(NoteEditIntent intent) noexcept;

    /// Returns the immutable validated value retained by this wrapper.
    const NoteEditIntent& value() const noexcept {
        return value_;
    }

    bool operator==(const ValidatedNoteEditIntent& other) const noexcept {
        return value_ == other.value_;
    }

  private:
    explicit ValidatedNoteEditIntent(NoteEditIntent intent) noexcept : value_(std::move(intent)) {}

    NoteEditIntent value_;
};

/// Host binding used by a piano-roll front-end before command lowering.
using NoteEditIntentHost = SequencerUiHostT<ValidatedNoteEditIntent>;

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

/// Confirmed lifecycle state of one continuous edit gesture.
enum class EditGestureIdentityState : std::uint8_t {
    AwaitingBegin,
    Open,
    Closed,
    Exhausted,
};

/// Why an edit-gesture identity operation was refused.
enum class EditGestureIdentityError : std::uint8_t {
    InvalidWriter,
    WriterMismatch,
    InvalidPhase,
    GestureClosed,
    IdentityExhausted,
    IdentityPending,
    ForeignOrStaleIssue,
    IssuedTransactionMismatch,
    SubmissionResultMismatch,
};

/// Result of submitting one issued gesture transaction through its allocator.
struct EditGestureSubmitOutcome {
    /// The exact result returned by DocumentSession::submit().
    runtime::Result<timeline::CommitResult, timeline::TransactionError> document_result;
    /// Confirmed allocator state after that result was applied.
    EditGestureIdentityState state = EditGestureIdentityState::AwaitingBegin;
};

class EditGestureIdentityAllocator;

/// One opaque, single-owner identity issue awaiting a submission outcome.
class EditGestureIdentityIssue {
  public:
    EditGestureIdentityIssue(const EditGestureIdentityIssue&) = delete;
    EditGestureIdentityIssue& operator=(const EditGestureIdentityIssue&) = delete;
    EditGestureIdentityIssue(EditGestureIdentityIssue&& other) noexcept
        : identity_(std::exchange(other.identity_, {})),
          phase_(std::exchange(other.phase_, timeline::GesturePhase::Single)),
          provenance_(std::exchange(other.provenance_, {})) {}
    EditGestureIdentityIssue& operator=(EditGestureIdentityIssue&&) = delete;

    /// Lowers `intent` with this issue's authoritative gesture phase and identity.
    ///
    /// The caller-supplied phase is ignored so the transaction submitted through
    /// its allocator cannot diverge from the phase used for lifecycle advancement.
    [[nodiscard]] runtime::Result<timeline::Transaction, timeline::ModelError>
    lower(EditIntent intent) const;

    /// Returns the authoritative phase used by lower().
    timeline::GesturePhase phase() const noexcept {
        return phase_;
    }

  private:
    friend class EditGestureIdentityAllocator;
    EditGestureIdentityIssue(EditIntentIdentity identity, timeline::GesturePhase phase,
                             timeline::WriterToken::Provenance provenance) noexcept
        : identity_(std::move(identity)), phase_(phase), provenance_(provenance) {}

    void invalidate() noexcept {
        identity_ = {};
        phase_ = timeline::GesturePhase::Single;
        provenance_ = {};
    }

    EditIntentIdentity identity_;
    timeline::GesturePhase phase_ = timeline::GesturePhase::Single;
    timeline::WriterToken::Provenance provenance_;
};

/// Allocates one undo group and submits a construction-safe stream of gesture identities.
///
/// A transaction can be rejected after its IDs are allocated, so issuing an
/// identity never advances the confirmed lifecycle. submit() validates the
/// issue and its lowered transaction, calls DocumentSession::submit() itself,
/// and derives the transition from that result. A rejection keeps the last
/// confirmed state and permits a fresh-ID retry with the same undo group.
/// Every attempted session submission consumes its issue, and exactly one issue
/// may await submission.
///
/// This is control-thread state, not a concurrent reservation in
/// DocumentSession. The session owns the one-open-gesture rule and is the only
/// submission authority; the allocator retains neither it nor the WriterToken.
/// Opaque writer provenance prevents equal numeric ID streams from separate
/// sessions from being mistaken for one another here. Cancel closes the group
/// after a successful submission; reverting its applied edits remains the
/// caller's subsequent `DocumentSession::undo()` operation.
class EditGestureIdentityAllocator {
  public:
    /// Allocates the gesture's sole undo group at pointer-down.
    static runtime::Result<EditGestureIdentityAllocator, EditGestureIdentityError>
    create(timeline::WriterToken& writer) noexcept {
        const auto provenance = writer.provenance();
        if (!provenance.valid())
            return runtime::Result<EditGestureIdentityAllocator, EditGestureIdentityError>(
                runtime::Err(EditGestureIdentityError::InvalidWriter));

        const auto undo_group = writer.allocate_undo_group_id();
        if (!undo_group.valid())
            return runtime::Result<EditGestureIdentityAllocator, EditGestureIdentityError>(
                runtime::Err(EditGestureIdentityError::IdentityExhausted));

        return runtime::Result<EditGestureIdentityAllocator, EditGestureIdentityError>(
            runtime::Ok(EditGestureIdentityAllocator(provenance, undo_group)));
    }

    EditGestureIdentityAllocator(const EditGestureIdentityAllocator&) = delete;
    EditGestureIdentityAllocator& operator=(const EditGestureIdentityAllocator&) = delete;
    EditGestureIdentityAllocator(EditGestureIdentityAllocator&& other) noexcept
        : provenance_(std::exchange(other.provenance_, {})),
          undo_group_(std::exchange(other.undo_group_, {})),
          state_(std::exchange(other.state_, EditGestureIdentityState::Closed)),
          pending_(std::exchange(other.pending_, false)),
          pending_transaction_(std::exchange(other.pending_transaction_, {})),
          pending_revision_(std::exchange(other.pending_revision_, {})),
          pending_command_(std::exchange(other.pending_command_, {})),
          pending_phase_(std::exchange(other.pending_phase_, timeline::GesturePhase::Single)) {}
    EditGestureIdentityAllocator& operator=(EditGestureIdentityAllocator&&) = delete;

    /// Issues fresh transaction and command IDs for one legal lifecycle step.
    ///
    /// Invalid writers and phases are rejected before either ID stream advances.
    /// If transaction allocation succeeds but command allocation is exhausted,
    /// that transaction ID remains consumed; writer-local IDs are never reused.
    runtime::Result<EditGestureIdentityIssue, EditGestureIdentityError>
    issue(timeline::WriterToken& writer, timeline::DocumentRevision expected_revision,
          timeline::GesturePhase phase) noexcept {
        if (state_ == EditGestureIdentityState::Closed)
            return issue_error(EditGestureIdentityError::GestureClosed);
        if (state_ == EditGestureIdentityState::Exhausted)
            return issue_error(EditGestureIdentityError::IdentityExhausted);
        if (pending_)
            return issue_error(EditGestureIdentityError::IdentityPending);
        if (!writer.provenance().valid())
            return issue_error(EditGestureIdentityError::InvalidWriter);
        if (writer.provenance() != provenance_)
            return issue_error(EditGestureIdentityError::WriterMismatch);
        if (!phase_permitted(phase))
            return issue_error(EditGestureIdentityError::InvalidPhase);

        const auto transaction_id = writer.allocate_transaction_id();
        if (!transaction_id.valid()) {
            state_ = EditGestureIdentityState::Exhausted;
            return issue_error(EditGestureIdentityError::IdentityExhausted);
        }

        const auto command_id = writer.allocate_command_id();
        if (!command_id.valid()) {
            state_ = EditGestureIdentityState::Exhausted;
            return issue_error(EditGestureIdentityError::IdentityExhausted);
        }

        EditIntentIdentity identity{transaction_id, expected_revision, command_id, undo_group_};
        pending_ = true;
        pending_transaction_ = transaction_id;
        pending_revision_ = expected_revision;
        pending_command_ = command_id;
        pending_phase_ = phase;
        return runtime::Result<EditGestureIdentityIssue, EditGestureIdentityError>(
            runtime::Ok(EditGestureIdentityIssue(std::move(identity), phase, provenance_)));
    }

    /// Validates and submits exactly one issued gesture transaction.
    ///
    /// Protocol mismatches touch neither DocumentSession nor the pending issue.
    /// Once DocumentSession is called, both its success and rejection consume
    /// the issue; only success advances the confirmed lifecycle.
    runtime::Result<EditGestureSubmitOutcome, EditGestureIdentityError>
    submit(timeline::DocumentSession& session, timeline::WriterToken& writer,
           EditGestureIdentityIssue&& issue, timeline::Transaction transaction) {
        if (!matches_pending(issue))
            return submit_error(EditGestureIdentityError::ForeignOrStaleIssue);
        if (!writer.provenance().valid())
            return submit_error(EditGestureIdentityError::InvalidWriter);
        if (writer.provenance() != provenance_)
            return submit_error(EditGestureIdentityError::WriterMismatch);
        if (!matches_pending(transaction))
            return submit_error(EditGestureIdentityError::IssuedTransactionMismatch);

        const auto phase = pending_phase_;
        const auto command = pending_command_;
        auto document_result = session.submit(writer, std::move(transaction));
        issue.invalidate();
        clear_pending();
        if (document_result) {
            if (document_result->applied_commands.size() != 1 ||
                document_result->applied_commands.front() != command) {
                state_ = EditGestureIdentityState::Exhausted;
                return submit_error(EditGestureIdentityError::SubmissionResultMismatch);
            }
            state_ = committed_state_after(phase);
        }
        return runtime::Result<EditGestureSubmitOutcome, EditGestureIdentityError>(runtime::Ok(
            EditGestureSubmitOutcome{std::move(document_result), state_}));
    }

    /// Returns the one undo group shared by every issue from this allocator.
    timeline::UndoGroupId undo_group() const noexcept {
        return undo_group_;
    }

    /// Returns the last submission-confirmed lifecycle state.
    EditGestureIdentityState state() const noexcept {
        return state_;
    }

    /// Returns whether one issued identity still needs submission.
    bool has_pending_identity() const noexcept {
        return pending_;
    }

  private:
    EditGestureIdentityAllocator(timeline::WriterToken::Provenance provenance,
                                 timeline::UndoGroupId undo_group) noexcept
        : provenance_(provenance), undo_group_(undo_group) {}

    bool phase_permitted(timeline::GesturePhase phase) const noexcept {
        if (state_ == EditGestureIdentityState::AwaitingBegin)
            return phase == timeline::GesturePhase::Begin;
        return phase == timeline::GesturePhase::Update || phase == timeline::GesturePhase::End ||
               phase == timeline::GesturePhase::Cancel;
    }

    bool matches_pending(const EditGestureIdentityIssue& issue) const noexcept {
        const auto& identity = issue.identity_;
        return pending_ && issue.provenance_ == provenance_ && issue.phase_ == pending_phase_ &&
               identity.transaction_id == pending_transaction_ &&
               identity.expected_revision == pending_revision_ &&
               identity.command_id == pending_command_ && identity.undo_group == undo_group_;
    }

    bool matches_pending(const timeline::Transaction& transaction) const noexcept {
        return transaction.id == pending_transaction_ &&
               transaction.expected_revision == pending_revision_ &&
               transaction.undo_group == undo_group_ &&
               transaction.gesture_phase == pending_phase_ && transaction.commands.size() == 1 &&
               transaction.commands.front().id == pending_command_;
    }

    static EditGestureIdentityState committed_state_after(timeline::GesturePhase phase) noexcept {
        if (phase == timeline::GesturePhase::Begin || phase == timeline::GesturePhase::Update)
            return EditGestureIdentityState::Open;
        return EditGestureIdentityState::Closed;
    }

    void clear_pending() noexcept {
        pending_ = false;
        pending_transaction_ = {};
        pending_revision_ = {};
        pending_command_ = {};
        pending_phase_ = timeline::GesturePhase::Single;
    }

    static runtime::Result<EditGestureIdentityIssue, EditGestureIdentityError>
    issue_error(EditGestureIdentityError error) noexcept {
        return runtime::Result<EditGestureIdentityIssue, EditGestureIdentityError>(
            runtime::Err(error));
    }

    static runtime::Result<EditGestureSubmitOutcome, EditGestureIdentityError>
    submit_error(EditGestureIdentityError error) {
        return runtime::Result<EditGestureSubmitOutcome, EditGestureIdentityError>(
            runtime::Err(error));
    }

    timeline::WriterToken::Provenance provenance_;
    timeline::UndoGroupId undo_group_;
    EditGestureIdentityState state_ = EditGestureIdentityState::AwaitingBegin;
    bool pending_ = false;
    timeline::TransactionId pending_transaction_;
    timeline::DocumentRevision pending_revision_;
    timeline::CommandId pending_command_;
    timeline::GesturePhase pending_phase_ = timeline::GesturePhase::Single;
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

/// Why a note intent could not be lowered to a transaction.
///
/// Distinct from ModelError because the interesting refusal is not a malformed
/// value: it is a well-formed gesture this lowering deliberately does not serve.
enum class NoteLoweringError : std::uint8_t {
    /// The intent's phase is not Single. See lower_note_edit_intent.
    ContinuousGestureUnsupported,
    /// Insert supplied a note identity the clip already carries.
    DuplicateNoteIdentity,
    /// Erase or a transform named a note the supplied content does not carry.
    NoteNotInClip,
    /// The expected note does not match the one the clip currently carries.
    ExpectedNoteMismatch,
    /// The transaction or command identity is malformed.
    InvalidIdentity,
};

/// Lowers one commit-on-release note intent to the ordinary transaction that
/// performs it.
///
/// `current_notes` is the target clip's note array as the caller believes it to
/// be. It is a parameter rather than a project lookup so this stays pure for the
/// same reason `lower_edit_intent` is: one intent can be lowered twice — once
/// per input device in a parity test, once per retry after a stale revision —
/// without the intent being rewritten. The array becomes the command's expected
/// value, so a caller that passes a stale one gets `ExpectedValueMismatch` from
/// the reducer rather than a silent overwrite.
///
/// SCOPE — `GesturePhase::Single` ONLY, and the boundary is deliberate.
/// `ReplaceNoteContent` carries both note arrays, so a session charges one edit
/// `retained_size(forward) + retained_size(inverse)`. A Single-phase transaction
/// is closed on admission and immediately evictable, so a stream of them costs
/// undo depth and nothing else. A `Begin`/`Update`/.../`End` drag instead
/// coalesces into a group that stays open, and an open group is evictable by
/// nothing — so a per-frame drag accumulates until it dies mid-gesture with
/// `ConflictCode::UndoFull`. Serving a continuous drag needs granular note
/// commands, not a different lowering of this one, so this function refuses a
/// non-Single phase with `ContinuousGestureUnsupported` rather than emitting a
/// transaction that would fail partway through a user's gesture.
///
/// That makes commit-on-release the shape this serves, which is also the right
/// product shape: a drag over a large clip pays a whole-content replacement per
/// frame, while committing on release pays one per edit.
runtime::Result<timeline::Transaction, NoteLoweringError>
lower_note_edit_intent(const ValidatedNoteEditIntent& intent,
                       std::span<const timeline::NoteEvent> current_notes,
                       const EditIntentIdentity& identity);

/// Lowers one intent to the ordinary transaction that performs it.
///
/// Pure: the intent already names its target, so no project lookup is required and
/// the optimistic range gates pass through to the reducer unchanged. A non-`Single`
/// phase requires a valid undo group owned by the transaction's writer, matching
/// what DocumentSession admits, so a malformed gesture is rejected here instead of
/// at commit.
runtime::Result<timeline::Transaction, timeline::ModelError>
lower_edit_intent(const EditIntent& intent, const EditIntentIdentity& identity);

inline runtime::Result<timeline::Transaction, timeline::ModelError>
EditGestureIdentityIssue::lower(EditIntent intent) const {
    intent.phase = phase_;
    return lower_edit_intent(intent, identity_);
}

/// @}

} // namespace pulp::timeline_editor
