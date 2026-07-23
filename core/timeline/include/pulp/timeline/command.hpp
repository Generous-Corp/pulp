#pragma once

#include <pulp/timeline/model.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace pulp::timeline {

struct WriterId {
    std::uint64_t value = 0;
    constexpr bool valid() const noexcept {
        return value != 0;
    }
    constexpr auto operator<=>(const WriterId&) const = default;
};

struct CommandId {
    WriterId writer;
    std::uint64_t sequence = 0;
    constexpr bool valid() const noexcept {
        return writer.valid() && sequence != 0;
    }
    constexpr auto operator<=>(const CommandId&) const = default;
};

struct TransactionId {
    WriterId writer;
    std::uint64_t sequence = 0;
    constexpr bool valid() const noexcept {
        return writer.valid() && sequence != 0;
    }
    constexpr auto operator<=>(const TransactionId&) const = default;
};

struct DocumentRevision {
    std::uint64_t value = 0;
    constexpr auto operator<=>(const DocumentRevision&) const = default;
};

struct UndoGroupId {
    WriterId writer;
    std::uint64_t sequence = 0;
    constexpr bool valid() const noexcept {
        return writer.valid() && sequence != 0;
    }
    constexpr auto operator<=>(const UndoGroupId&) const = default;
};

enum class GesturePhase : std::uint8_t { Single, Begin, Update, End };

struct InsertClip {
    ItemId sequence_id;
    ItemId track_id;
    Clip clip;
};

struct RemoveClip {
    ItemId sequence_id;
    ItemId track_id;
    ItemId clip_id;
};

struct InsertAutomationLane {
    ItemId sequence_id;
    ItemId track_id;
    AutomationLane lane;
};

struct RemoveAutomationLane {
    ItemId sequence_id;
    ItemId track_id;
    ItemId lane_id;
};

struct MoveClip {
    ItemId sequence_id;
    ItemId track_id;
    ItemId clip_id;
    ClipTimeRange expected_range;
    ClipTimeRange replacement_range;
};

struct SetNoteVelocity {
    ItemId sequence_id;
    ItemId track_id;
    ItemId clip_id;
    ItemId note_id;
    std::uint16_t expected_velocity = 0;
    std::uint16_t replacement_velocity = 0;
};

struct SetClipPlaybackProperties {
    ItemId sequence_id;
    ItemId track_id;
    ItemId clip_id;
    ClipPlaybackProperties expected;
    ClipPlaybackProperties replacement;
};

struct SetTempoMap {
    timebase::TempoMap expected;
    timebase::TempoMap replacement;
};

struct SetMeterMap {
    timebase::MeterMap expected;
    timebase::MeterMap replacement;
};

// A recorded or imported media asset enters the document as a sealed input.
// The command carries the whole MediaAsset by value, including the ContentHash
// that is the asset's durable identity. Replay appends the sealed asset by
// reference to that hash and never re-captures or re-hashes media bytes, so the
// same checkpoint plus journal reproduce a byte-identical asset table.
struct CreateAsset {
    MediaAsset asset;
};

struct RemoveAsset {
    ItemId asset_id;
};

// A take lane enters (or leaves) a track as one owned identity subtree: the
// lane plus every take it carries. Each take's MediaRef::asset_id must already
// name a project asset — the recorder emits CreateAsset first, in the same or an
// earlier transaction — so replay reproduces takes as pinned references.
struct InsertTakeLane {
    ItemId sequence_id;
    ItemId track_id;
    TakeLane lane;
};

struct RemoveTakeLane {
    ItemId sequence_id;
    ItemId track_id;
    ItemId lane_id;
};

struct InsertTake {
    ItemId sequence_id;
    ItemId track_id;
    ItemId lane_id;
    Take take;
};

struct RemoveTake {
    ItemId sequence_id;
    ItemId track_id;
    ItemId lane_id;
    ItemId take_id;
};

// Record-arm is document intent, not an identity: expected/replacement gate the
// edit on the current value so concurrent writers cannot silently clobber it.
struct SetRecordArm {
    ItemId sequence_id;
    ItemId track_id;
    bool expected = false;
    bool replacement = false;
};

// Zero selects the arrangement. A non-zero value selects one existing take
// lane as the track's active playlist/comp. Segment selections and their
// derived render artifact can extend that lane without changing this seam.
struct SetActiveTakeLane {
    ItemId sequence_id;
    ItemId track_id;
    ItemId expected_lane_id;
    ItemId replacement_lane_id;
};

// Replaces one lane's canonical segment comp under an exact optimistic gate.
// Both values make undo and deterministic journal replay self-contained.
struct SetTakeComp {
    ItemId sequence_id;
    ItemId track_id;
    ItemId lane_id;
    std::vector<TakeCompSegment> expected;
    std::vector<TakeCompSegment> replacement;
};

using Command = std::variant<InsertClip, RemoveClip, InsertAutomationLane, RemoveAutomationLane,
                             MoveClip, SetNoteVelocity, SetClipPlaybackProperties, SetTempoMap,
                             SetMeterMap, CreateAsset, RemoveAsset, InsertTakeLane, RemoveTakeLane,
                             SetRecordArm, InsertTake, RemoveTake, SetActiveTakeLane, SetTakeComp>;

struct CommandEnvelope {
    CommandId id;
    Command command;
};

struct Transaction {
    TransactionId id;
    DocumentRevision expected_revision;
    std::optional<UndoGroupId> undo_group;
    GesturePhase gesture_phase = GesturePhase::Single;
    std::vector<CommandEnvelope> commands;
};

bool equivalent(const ClipTimeRange& lhs, const ClipTimeRange& rhs) noexcept;
bool equivalent(const Clip& lhs, const Clip& rhs) noexcept;
bool equivalent(const AutomationLane& lhs, const AutomationLane& rhs) noexcept;
bool equivalent(const TakeLane& lhs, const TakeLane& rhs) noexcept;
bool equivalent(const Command& lhs, const Command& rhs) noexcept;
bool equivalent(const Transaction& lhs, const Transaction& rhs) noexcept;
std::size_t retained_size(const Command& command) noexcept;
std::size_t retained_size(const Transaction& transaction) noexcept;

} // namespace pulp::timeline
