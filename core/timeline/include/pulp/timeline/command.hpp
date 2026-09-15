#pragma once

#include <pulp/timeline/midi_lane.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/modulation.hpp>
#include <pulp/timeline/tuning.hpp>

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

/// Inserts a set of identity-bearing notes into one MIDI clip.
///
/// The set is plural so one gesture consumes one journal command regardless of
/// selection size. Every note identity must be available. `modifiers` may
/// describe only notes in this same payload; a reducer-built inverse uses it to
/// restore modifiers removed with their notes. The clip-level modifier seed and
/// every controller lane carry over unchanged.
struct InsertNotes {
    /// Sequence containing the target track.
    ItemId sequence_id;
    /// Track containing the target MIDI clip.
    ItemId track_id;
    /// MIDI clip that receives the notes.
    ItemId clip_id;
    /// Identity-bearing note values to insert.
    std::vector<NoteEvent> notes;
    /// Optional modifiers attached only to notes in this insertion.
    std::vector<NoteModifier> modifiers;
};

/// Removes a named set of notes under an exact optimistic-value gate.
///
/// Each `expected` entry must equal the current note with the same identity in
/// every field. The reducer captures any attached modifiers in the generated
/// `InsertNotes` inverse, so undo restores the complete note behavior without
/// making forward callers restate clip-owned modifier data.
struct RemoveNotes {
    /// Sequence containing the target track.
    ItemId sequence_id;
    /// Track containing the target MIDI clip.
    ItemId track_id;
    /// MIDI clip from which the notes are removed.
    ItemId clip_id;
    /// Exact current note values whose identities are removed.
    std::vector<NoteEvent> expected;
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

/// Replaces a sequence's complete dynamics lane under an exact value gate.
///
/// Both lanes are non-optional because the model states no absence: a sequence
/// that authors no intensity carries an empty lane, so `nullopt` would invent a
/// second meaning for "states nothing yet" that no reader could distinguish.
struct SetDynamicsLane {
    /// Sequence whose dynamics lane changes.
    ItemId sequence_id;
    /// Required current lane content, compared in full.
    DynamicsLane expected;
    /// Lane content written when the gate matches.
    DynamicsLane replacement;
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

/// Inserts a typed device declaration into a track's authored chain.
struct InsertDevice {
    /// Sequence containing the target track.
    ItemId sequence_id;
    /// Track receiving the placement.
    ItemId track_id;
    /// Complete typed declaration to insert.
    DevicePlacement placement;
    /// Authored successor, or empty to append.
    std::optional<ItemId> before_device_id = std::nullopt;
};

/// Removes a device declaration by identity.
struct RemoveDevice {
    /// Sequence containing the target track.
    ItemId sequence_id;
    /// Track owning the placement.
    ItemId track_id;
    /// Placement identity to remove.
    ItemId device_id;
};

/// Moves a device under an exact optimistic authored-position gate.
struct MoveDevice {
    /// Sequence containing the target track.
    ItemId sequence_id;
    /// Track owning the placement.
    ItemId track_id;
    /// Placement identity to move.
    ItemId device_id;
    /// Required current successor, or empty when currently last.
    std::optional<ItemId> expected_before_device_id = std::nullopt;
    /// Replacement successor, or empty to move last.
    std::optional<ItemId> replacement_before_device_id = std::nullopt;
};

/// Replaces device kind, binding, slot, stage, bypass, and wet/dry atomically.
struct RetargetDevice {
    /// Sequence containing the target track.
    ItemId sequence_id;
    /// Track owning the placement.
    ItemId track_id;
    /// Placement identity to retarget.
    ItemId device_id;
    /// Required current configuration.
    DeviceConfiguration expected;
    /// Replacement configuration.
    DeviceConfiguration replacement;
};

/// Replaces the optional content-addressed opaque state reference.
struct SetDeviceState {
    /// Sequence containing the target track.
    ItemId sequence_id;
    /// Track owning the placement.
    ItemId track_id;
    /// Placement identity whose state changes.
    ItemId device_id;
    /// Required current state content identity.
    std::optional<ContentHash> expected;
    /// Replacement state content identity.
    std::optional<ContentHash> replacement;
};

/// Inserts a controller/expression lane into a MIDI clip.
///
/// The lane arrives whole — identity, address, and every authored point — and
/// the content model is the single authority on whether it may join the clip:
/// identity distinctness across notes, points, and lanes, and one lane per
/// address, are enforced by `MidiContent::create` and propagated from there.
struct InsertMidiExpressionLane {
    /// Sequence owning the track that owns the clip.
    ItemId sequence_id;
    /// Track owning the clip.
    ItemId track_id;
    /// MIDI clip the lane joins.
    ItemId clip_id;
    /// Lane to insert, with its identity, address, and authored points.
    MidiExpressionLane lane;
};

/// Removes a controller/expression lane from a MIDI clip by identity.
///
/// Removal is the destructive intent a capability mask denies by default, so a
/// writer that may edit a lane's values does not thereby gain the right to
/// abandon the stream.
struct RemoveMidiExpressionLane {
    /// Sequence owning the track that owns the clip.
    ItemId sequence_id;
    /// Track owning the clip.
    ItemId track_id;
    /// MIDI clip the lane leaves.
    ItemId clip_id;
    /// Identity of the lane to remove.
    ItemId lane_id;
};

/// Replaces one lane's authored points under an exact optimistic-value gate.
///
/// The lane keeps its identity and its address: this command changes what a
/// stream says, never which stream it is. Re-addressing a lane is a remove and
/// an insert, because the two operations a caller means by it — abandoning one
/// stream and authoring another — carry different authority.
struct SetMidiExpressionLanePoints {
    /// Sequence owning the track that owns the clip.
    ItemId sequence_id;
    /// Track owning the clip.
    ItemId track_id;
    /// MIDI clip owning the lane.
    ItemId clip_id;
    /// Identity of the lane whose points change.
    ItemId lane_id;
    /// Required current points of that lane, compared in full and in canonical order.
    std::vector<MidiLanePoint> expected;
    /// Points written when the gate matches.
    std::vector<MidiLanePoint> replacement;
};

/// Replaces the project-wide tuning statement under an exact value gate.
///
/// An absent `replacement` states no tuning at all, which is not the same claim
/// as stating equal temperament: a document that never chose plays in whatever
/// the host defaults to, while one that chose equal temperament has named it.
/// Clearing is therefore an authored act on a value the project keeps, not the
/// removal of anything the document owns, which is why the intent is Modify.
struct SetProjectTuning {
    /// Required current project tuning, compared exactly, absence included.
    std::optional<TuningReference> expected;
    /// Tuning written when the gate matches.
    std::optional<TuningReference> replacement;
};

/// Replaces one track's tuning override under an exact value gate.
///
/// Absence means the track plays in whatever the project states, so clearing an
/// override hands the track back to the project rather than silencing it.
struct SetTrackTuning {
    /// Sequence owning the track.
    ItemId sequence_id;
    /// Track whose tuning override changes.
    ItemId track_id;
    /// Required current override, compared exactly, absence included.
    std::optional<TuningReference> expected;
    /// Override written when the gate matches.
    std::optional<TuningReference> replacement;
};

/// Replaces one sequence-owned region under an exact value gate.
///
/// Identity is pinned: `expected.id` and `replacement.id` must be equal and must
/// name a region the sequence owns. A Modify command that could also swap
/// identity would be a removal and a creation laundered through a signature
/// that declares neither, and the removal is the axis an untrusted writer is
/// denied by default. Pinning it is what makes correcting a region's
/// SectionRole reachable for the writer profile that authored it.
struct SetRegion {
    /// Sequence owning the region.
    ItemId sequence_id;
    /// Required current region, compared in full including its role.
    SequenceRegion expected;
    /// Region written when the gate matches, carrying the same identity.
    SequenceRegion replacement;
};

/// Inserts a track-owned modulation source.
///
/// Identity is authored by the caller rather than minted here, so the route
/// that will read this source can be written in the same transaction.
struct InsertModulator {
    /// Sequence owning the track.
    ItemId sequence_id;
    /// Track that will own the modulator.
    ItemId track_id;
    /// Complete modulator declaration, carrying its own identity.
    Modulator modulator;
};

/// Removes a track-owned modulation source by identity.
///
/// A source a route still reads cannot be removed: the model refuses a track
/// whose routes name a source it does not hold, so the refusal arrives as a
/// model failure rather than as a document that silently loses its routing.
struct RemoveModulator {
    /// Sequence owning the track.
    ItemId sequence_id;
    /// Track owning the modulator.
    ItemId track_id;
    /// Identity of the modulator to remove.
    ItemId modulator_id;
};

/// Replaces a modulation source's declaration under an exact value gate.
///
/// Identity is pinned: `expected.id`, `replacement.id`, and `modulator_id` must
/// all be equal. A Modify that could also swap identity is a removal and a
/// creation wearing a signature that declares neither, and removal is the axis
/// an untrusted writer is denied by default — so an unpinned identity would
/// hand a writer holding only Modify the operation its mask refuses.
struct SetModulator {
    /// Sequence owning the track.
    ItemId sequence_id;
    /// Track owning the modulator.
    ItemId track_id;
    /// Identity of the modulator whose declaration changes.
    ItemId modulator_id;
    /// Required current declaration, compared in full.
    Modulator expected;
    /// Declaration written when the gate matches, carrying the same identity.
    Modulator replacement;
};

/// Inserts a track-owned macro control.
struct InsertMacro {
    /// Sequence owning the track.
    ItemId sequence_id;
    /// Track that will own the macro.
    ItemId track_id;
    /// Complete macro declaration, carrying its own identity.
    MacroControl macro;
};

/// Removes a track-owned macro control by identity.
///
/// As with a modulation source, a macro a route still reads cannot be removed;
/// the model refuses the resulting track rather than dropping the routes.
struct RemoveMacro {
    /// Sequence owning the track.
    ItemId sequence_id;
    /// Track owning the macro.
    ItemId track_id;
    /// Identity of the macro to remove.
    ItemId macro_id;
};

/// Replaces a macro control's declaration under an exact value gate.
///
/// Identity is pinned the same way SetModulator pins it, and for the same
/// reason. The gate covers the whole macro, name and position together, which
/// is what an authoring edit that rewrites both should gate on.
struct SetMacro {
    /// Sequence owning the track.
    ItemId sequence_id;
    /// Track owning the macro.
    ItemId track_id;
    /// Identity of the macro whose declaration changes.
    ItemId macro_id;
    /// Required current declaration, compared in full.
    MacroControl expected;
    /// Declaration written when the gate matches, carrying the same identity.
    MacroControl replacement;
};

/// Replaces only a macro's authored position under an exact value gate.
///
/// A whole-value gate would make a performer moving a macro also supply its
/// current name, so a concurrent rename would abort an edit that did not
/// conflict with it — the gate manufacturing a conflict out of two disjoint
/// edits. This is the narrow command beside the broad one, the same shape
/// SetNoteVelocity has beside SetNoteEvents. The overlap is deliberate:
/// SetMacro may also change the value, and an edit that rewrites the whole
/// macro should gate on the whole macro.
///
/// The float compares exactly rather than within a tolerance, and that is safe
/// rather than fragile: the persisted spelling is the IEEE-754 bit pattern, so
/// a value that round-trips through the wire compares equal to itself.
struct SetMacroValue {
    /// Sequence owning the track.
    ItemId sequence_id;
    /// Track owning the macro.
    ItemId track_id;
    /// Identity of the macro whose position changes.
    ItemId macro_id;
    /// Required current normalized position, compared exactly.
    float expected = 0.0f;
    /// Normalized position written when the gate matches.
    float replacement = 0.0f;
};

/// Inserts one authored source-to-parameter connection.
///
/// Identity is authored by the caller, as it is for the source this reads, so a
/// source and the route that reads it can be stated in one transaction.
struct InsertModulationRoute {
    /// Sequence owning the track.
    ItemId sequence_id;
    /// Track that will own the route.
    ItemId track_id;
    /// Complete route declaration, carrying its own identity.
    ModulationRoute route;
};

/// Removes one authored connection by identity.
///
/// Removing a route is the one modulation removal nothing else can refuse: a
/// route is read by no other document member, so the source it named stays and
/// only the connection goes.
struct RemoveModulationRoute {
    /// Sequence owning the track.
    ItemId sequence_id;
    /// Track owning the route.
    ItemId track_id;
    /// Identity of the route to remove.
    ItemId route_id;
};

/// Replaces a route's source, target, depth, and bypass under an exact gate.
///
/// Identity is pinned the way SetModulator pins it, and for the same reason.
/// The gate covers the whole route rather than the depth alone: a route has no
/// performed field, so there is no high-frequency edit for a narrow gate to
/// protect, and inventing one would be vocabulary bought with nothing.
///
/// `enabled` is gated like every other member. A disabled route keeps its
/// identity, depth, and target so that re-enabling restores what was there,
/// which is only true if a bypass is a value an edit states rather than a
/// state an edit discards.
struct SetModulationRoute {
    /// Sequence owning the track.
    ItemId sequence_id;
    /// Track owning the route.
    ItemId track_id;
    /// Identity of the route whose connection changes.
    ItemId route_id;
    /// Required current route, compared in full including its bypass.
    ModulationRoute expected;
    /// Route written when the gate matches, carrying the same identity.
    ModulationRoute replacement;
};

/// Exhaustive set of durable Timeline document mutations.
using Command = std::variant<
    InsertClip, RemoveClip, InsertAutomationLane, RemoveAutomationLane, MoveClip, SetNoteVelocity,
    ReplaceNoteContent, SetClipPlaybackProperties, SetTempoMap, SetMeterMap, CreateAsset,
    RemoveAsset, InsertTakeLane, RemoveTakeLane, SetRecordArm, InsertTake, RemoveTake,
    SetActiveTakeLane, SetTakeComp, SetTrackFreeze, InsertMarker, RemoveMarker, InsertRegion,
    RemoveRegion, SetChordScaleLane, SetGroove, InsertScene, RemoveScene, InsertSlot, RemoveSlot,
    InsertSequence, CloneSequence, RemoveSequence, SetClipSequenceRef, SetTrackMixer, InsertTrack,
    RemoveTrack, SetTrackName, MoveTrack, SetNoteEvents, InsertNotes, RemoveNotes, InsertDevice,
    RemoveDevice, MoveDevice, RetargetDevice, SetDeviceState, SetDynamicsLane,
    InsertMidiExpressionLane, RemoveMidiExpressionLane, SetMidiExpressionLanePoints,
    SetProjectTuning, SetTrackTuning, SetRegion, InsertModulator, RemoveModulator, SetModulator,
    InsertMacro, RemoveMacro, SetMacro, SetMacroValue, InsertModulationRoute, RemoveModulationRoute,
    SetModulationRoute>;

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
build_diverge_transaction(const Project& project, ItemLocation clip, TransactionId transaction_id,
                          DocumentRevision expected_revision, CommandId clone_command_id,
                          CommandId retarget_command_id,
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
