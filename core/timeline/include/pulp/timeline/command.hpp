#pragma once

#include <pulp/timeline/model.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace pulp::timeline {

/** @addtogroup timeline_editing
 * @{
 */

/// Stable non-zero identity of a document writer.
struct WriterId {
    std::uint64_t value = 0;
    /// Returns whether the writer identity is nonzero.
    constexpr bool valid() const noexcept {
        return value != 0;
    }
    constexpr auto operator<=>(const WriterId&) const = default;
};

/// Writer-scoped, monotonically assigned command identity.
struct CommandId {
    WriterId writer;
    std::uint64_t sequence = 0;
    /// Returns whether both the writer and sequence identities are nonzero.
    constexpr bool valid() const noexcept {
        return writer.valid() && sequence != 0;
    }
    constexpr auto operator<=>(const CommandId&) const = default;
};

/// Writer-scoped, monotonically assigned transaction identity.
struct TransactionId {
    WriterId writer;
    std::uint64_t sequence = 0;
    /// Returns whether both the writer and sequence identities are nonzero.
    constexpr bool valid() const noexcept {
        return writer.valid() && sequence != 0;
    }
    constexpr auto operator<=>(const TransactionId&) const = default;
};

/// Monotonic document snapshot revision used for optimistic admission.
struct DocumentRevision {
    std::uint64_t value = 0;
    constexpr auto operator<=>(const DocumentRevision&) const = default;
};

/// Writer-scoped identity joining gesture transactions into one undo unit.
struct UndoGroupId {
    WriterId writer;
    std::uint64_t sequence = 0;
    /// Returns whether both the writer and sequence identities are nonzero.
    constexpr bool valid() const noexcept {
        return writer.valid() && sequence != 0;
    }
    constexpr auto operator<=>(const UndoGroupId&) const = default;
};

/// Lifecycle position of a transaction within an interactive gesture.
///
/// `End` and `Cancel` are the two closing phases and share the mechanical close
/// path: both require a matching open gesture, both clear it, and both close the
/// undo group. They differ in what they assert about the edits already applied —
/// `End` says they stand, `Cancel` says they do not. A cancel necessarily arrives
/// AFTER its transactions have been applied, so closing is all the session does:
/// the revert is the caller's existing one-call `DocumentSession::undo()` over the
/// now-closed group, not a second reduction path hidden inside commit.
enum class GesturePhase : std::uint8_t { Single, Begin, Update, End, Cancel };

/// Inserts an identity-bearing clip into a track.
struct InsertClip {
    ItemId sequence_id;
    ItemId track_id;
    Clip clip;
};

/// Removes a clip by its owning coordinates and identity.
struct RemoveClip {
    ItemId sequence_id;
    ItemId track_id;
    ItemId clip_id;
};

/// Inserts an automation lane into a track.
struct InsertAutomationLane {
    ItemId sequence_id;
    ItemId track_id;
    AutomationLane lane;
};

/// Removes an automation lane by identity.
struct RemoveAutomationLane {
    ItemId sequence_id;
    ItemId track_id;
    ItemId lane_id;
};

/// Replaces a clip's time range under an exact optimistic-value gate.
struct MoveClip {
    ItemId sequence_id;
    ItemId track_id;
    ItemId clip_id;
    ClipTimeRange expected_range;
    ClipTimeRange replacement_range;
};

/// Replaces one note velocity under an exact optimistic-value gate.
struct SetNoteVelocity {
    ItemId sequence_id;
    ItemId track_id;
    ItemId clip_id;
    ItemId note_id;
    std::uint16_t expected_velocity = 0;
    std::uint16_t replacement_velocity = 0;
};

/// Replaces the complete note event set of one note clip.
///
/// This is the ordinary, durable edit emitted by higher-level note transforms:
/// the journal never records a callback invocation. The expected value makes
/// replay conflict-aware, while swapping expected/replacement is the exact
/// inverse used by undo.
///
/// The modifier arrays are optional. An authoring caller leaves both empty and
/// the reducer carries the clip's existing modifiers across, dropping only the
/// ones whose note the replacement removes. A reducer-built inverse fills them
/// in, because the modifiers of a removed note are exactly what the note array
/// alone cannot say: `replacement_modifiers` is the complete set to install and
/// `expected_modifiers` is the set the clip must currently carry, gating the
/// modifiers the same way the note arrays gate the notes.
struct ReplaceNoteContent {
    ItemId sequence_id;
    ItemId track_id;
    ItemId clip_id;
    std::vector<NoteEvent> expected;
    std::vector<NoteEvent> replacement;
    std::vector<NoteModifier> expected_modifiers;
    std::vector<NoteModifier> replacement_modifiers;
};

/// Replaces the values of a named subset of one note clip's notes.
///
/// The identity set is invariant: `replacement` names exactly the notes
/// `expected` names, pairwise by index, so the command inserts nothing, removes
/// nothing, and touches no identity at all. That is what makes it the shape a
/// drag emits. It carries only the notes under the gesture, where
/// ReplaceNoteContent gates on the clip's entire current note set and so costs
/// the whole array per frame; and swapping expected and replacement is the exact
/// inverse with no set difference to derive.
///
/// `expected` is the optimistic gate, one entry per note, each equal to that
/// note's current value in every field. Notes the payload does not name keep the
/// values they had. So do the clip's modifiers, their seed, and its expression
/// lanes: no note leaves the clip, so no modifier is left keying a note that is
/// gone, and the payload needs no modifier arrays of its own.
struct SetNoteEvents {
    ItemId sequence_id;
    ItemId track_id;
    ItemId clip_id;
    std::vector<NoteEvent> expected;
    std::vector<NoteEvent> replacement;
};

/// Replaces clip-level gain and fade controls under an exact value gate.
struct SetClipPlaybackProperties {
    ItemId sequence_id;
    ItemId track_id;
    ItemId clip_id;
    ClipPlaybackProperties expected;
    ClipPlaybackProperties replacement;
};

/// Replaces the complete project tempo map under an exact value gate.
struct SetTempoMap {
    timebase::TempoMap expected;
    timebase::TempoMap replacement;
};

/// Replaces the complete project meter map under an exact value gate.
struct SetMeterMap {
    timebase::MeterMap expected;
    timebase::MeterMap replacement;
};

/// Adds one sealed recorded or imported media asset.
///
/// The command carries the complete value, including its content hash. Replay
/// never re-captures or re-hashes the media bytes.
struct CreateAsset {
    MediaAsset asset;
};

/// Removes a project-owned media asset by identity.
struct RemoveAsset {
    ItemId asset_id;
};

/// Inserts one take lane and its owned take-identity subtree.
///
/// Referenced media assets must already exist in the project.
struct InsertTakeLane {
    ItemId sequence_id;
    ItemId track_id;
    TakeLane lane;
};

/// Removes a take lane and its owned take identities.
struct RemoveTakeLane {
    ItemId sequence_id;
    ItemId track_id;
    ItemId lane_id;
};

/// Inserts one take into an existing take lane.
struct InsertTake {
    ItemId sequence_id;
    ItemId track_id;
    ItemId lane_id;
    Take take;
};

/// Removes one take from an existing take lane.
struct RemoveTake {
    ItemId sequence_id;
    ItemId track_id;
    ItemId lane_id;
    ItemId take_id;
};

/// Replaces record-arm document intent under an exact optimistic-value gate.
struct SetRecordArm {
    ItemId sequence_id;
    ItemId track_id;
    bool expected = false;
    bool replacement = false;
};

/// Selects the arrangement or an existing take lane under an exact value gate.
///
/// A zero lane identity selects the arrangement.
struct SetActiveTakeLane {
    ItemId sequence_id;
    ItemId track_id;
    ItemId expected_lane_id;
    ItemId replacement_lane_id;
};

/// Replaces a take lane's canonical comp segments under an exact value gate.
struct SetTakeComp {
    ItemId sequence_id;
    ItemId track_id;
    ItemId lane_id;
    std::vector<TakeCompSegment> expected;
    std::vector<TakeCompSegment> replacement;
};

/// Publishes or clears a pre-rendered track artifact under an exact value gate.
struct SetTrackFreeze {
    ItemId sequence_id;
    ItemId track_id;
    std::optional<TrackFreeze> expected;
    std::optional<TrackFreeze> replacement;
};

/// Replaces a sequence's complete chord/scale lane under an exact value gate.
struct SetChordScaleLane {
    ItemId sequence_id;
    ChordScaleLane expected;
    ChordScaleLane replacement;
};

/// Inserts a sequence-owned marker identity.
struct InsertMarker {
    ItemId sequence_id;
    SequenceMarker marker;
};

/// Removes a sequence-owned marker by identity.
struct RemoveMarker {
    ItemId sequence_id;
    ItemId marker_id;
};

/// Inserts a sequence-owned region identity.
struct InsertRegion {
    ItemId sequence_id;
    SequenceRegion region;
};

/// Removes a sequence-owned region by identity.
struct RemoveRegion {
    ItemId sequence_id;
    ItemId region_id;
};

/// Replaces a sequence's complete groove under an exact value gate.
struct SetGroove {
    ItemId sequence_id;
    GrooveTemplate expected;
    GrooveTemplate replacement;
};

/// Inserts a scene at an authored position in a sequence.
struct InsertScene {
    ItemId sequence_id;
    Scene scene;
    // Empty appends. An inverse names the item that originally followed the
    // removed scene so undo restores authored order exactly.
    std::optional<ItemId> before_scene_id = std::nullopt;
};

/// Removes a scene and its owned slots by identity.
struct RemoveScene {
    ItemId sequence_id;
    ItemId scene_id;
};

/// Inserts a slot at an authored position in a scene.
struct InsertSlot {
    ItemId sequence_id;
    ItemId scene_id;
    Slot slot;
    // Empty appends. See InsertScene::before_scene_id.
    std::optional<ItemId> before_slot_id = std::nullopt;
};

/// Removes a slot from a scene by identity.
struct RemoveSlot {
    ItemId sequence_id;
    ItemId scene_id;
    ItemId slot_id;
};

/// Inserts a complete sequence and its owned identity subtree.
struct InsertSequence {
    Sequence sequence;
};

/// Clones a sequence using an explicit, complete owned-identity mapping.
struct CloneSequence {
    ItemId source_sequence_id;
    ItemId cloned_sequence_id;
    // Complete old -> new mapping for every identity owned by the source
    // sequence. Canonical order is ascending by old id.
    std::vector<std::pair<ItemId, ItemId>> id_remap;
};

/// Removes a sequence and its owned identity subtree.
struct RemoveSequence {
    ItemId sequence_id;
};

/// Retargets a sequence-reference clip under an exact optimistic-value gate.
struct SetClipSequenceRef {
    ItemId sequence_id;
    ItemId track_id;
    ItemId clip_id;
    SequenceRef expected;
    SequenceRef replacement;
};

/// Replaces track gain and pan under an exact optimistic-value gate.
struct SetTrackMixer {
    ItemId sequence_id;
    ItemId track_id;
    TrackMixer expected;
    TrackMixer replacement;
};

/// Inserts a track and its complete owned identity subtree at an authored position.
struct InsertTrack {
    ItemId sequence_id;
    Track track;
    // Empty appends. See InsertScene::before_scene_id.
    std::optional<ItemId> before_track_id = std::nullopt;
};

/// Removes a track and its complete owned identity subtree by identity.
struct RemoveTrack {
    ItemId sequence_id;
    ItemId track_id;
};

/// Replaces a track's authored name under an exact optimistic-value gate.
struct SetTrackName {
    ItemId sequence_id;
    ItemId track_id;
    std::string expected;
    std::string replacement;
};

/// Moves a track in authored order under an exact optimistic-position gate.
///
/// A position names the track the moved track stands before; an empty value
/// names the last position, matching InsertTrack::before_track_id. Swapping
/// expected and replacement is the exact inverse used by undo — far cheaper
/// than RemoveTrack's, which restores a whole owned subtree.
///
/// Authored order is all this touches. The identity order behind
/// Sequence::tracks() and the compiled program both stay as they were.
struct MoveTrack {
    ItemId sequence_id;
    ItemId track_id;
    std::optional<ItemId> expected_before_track_id = std::nullopt;
    std::optional<ItemId> replacement_before_track_id = std::nullopt;
};

/// Exhaustive set of durable Timeline document mutations.
using Command =
    std::variant<InsertClip, RemoveClip, InsertAutomationLane, RemoveAutomationLane, MoveClip,
                 SetNoteVelocity, ReplaceNoteContent, SetClipPlaybackProperties, SetTempoMap,
                 SetMeterMap, CreateAsset, RemoveAsset, InsertTakeLane, RemoveTakeLane,
                 SetRecordArm, InsertTake, RemoveTake, SetActiveTakeLane, SetTakeComp,
                 SetTrackFreeze, InsertMarker, RemoveMarker, InsertRegion, RemoveRegion,
                 SetChordScaleLane, SetGroove, InsertScene, RemoveScene, InsertSlot, RemoveSlot,
                 InsertSequence, CloneSequence, RemoveSequence, SetClipSequenceRef, SetTrackMixer,
                 InsertTrack, RemoveTrack, SetTrackName, MoveTrack, SetNoteEvents>;

/// One command paired with its writer-scoped idempotency identity.
struct CommandEnvelope {
    CommandId id;
    Command command;
};

/// Atomically admitted ordered command batch.
///
/// `expected_revision` provides optimistic concurrency. Gesture metadata affects
/// undo grouping but not command execution order.
struct Transaction {
    TransactionId id;
    DocumentRevision expected_revision;
    std::optional<UndoGroupId> undo_group;
    GesturePhase gesture_phase = GesturePhase::Single;
    std::vector<CommandEnvelope> commands;
};

/// Builds the atomic clone-and-retarget transaction used to diverge a sequence reference.
///
/// `clip` must locate a SequenceRef clip in `project`. The returned transaction
/// clones the referenced sequence with fresh identities and retargets the clip;
/// failure leaves the project and supplied identities unchanged.
runtime::Result<Transaction, ModelError>
build_diverge_transaction(const Project& project, ItemLocation clip,
                          TransactionId transaction_id, DocumentRevision expected_revision,
                          CommandId clone_command_id, CommandId retarget_command_id,
                          std::optional<UndoGroupId> undo_group = std::nullopt);

/// Compares time ranges by authored value rather than storage representation.
bool equivalent(const ClipTimeRange& lhs, const ClipTimeRange& rhs) noexcept;
/// Compares complete authored clip state.
bool equivalent(const Clip& lhs, const Clip& rhs) noexcept;
/// Compares complete authored automation-lane state.
bool equivalent(const AutomationLane& lhs, const AutomationLane& rhs) noexcept;
/// Compares complete authored take-lane state.
bool equivalent(const TakeLane& lhs, const TakeLane& rhs) noexcept;
/// Compares command alternatives and all authored fields.
bool equivalent(const Command& lhs, const Command& rhs) noexcept;
/// Compares transaction identity, admission metadata, and commands.
bool equivalent(const Transaction& lhs, const Transaction& rhs) noexcept;
/// Returns the retained heap-byte estimate used for command/journal limits.
std::size_t retained_size(const Command& command) noexcept;
/// Returns the retained heap-byte estimate of a transaction and its commands.
std::size_t retained_size(const Transaction& transaction) noexcept;

/// @}

} // namespace pulp::timeline
