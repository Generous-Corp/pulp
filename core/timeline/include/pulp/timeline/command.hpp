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

struct InsertSequenceMarker {
    ItemId sequence_id;
    SequenceMarker marker;
};

struct RemoveSequenceMarker {
    ItemId sequence_id;
    ItemId marker_id;
};

struct SetSequenceMarker {
    ItemId sequence_id;
    ItemId marker_id;
    SequenceMarker expected;
    SequenceMarker replacement;
};

struct InsertSequenceRegion {
    ItemId sequence_id;
    SequenceRegion region;
};

struct RemoveSequenceRegion {
    ItemId sequence_id;
    ItemId region_id;
};

struct SetSequenceRegion {
    ItemId sequence_id;
    ItemId region_id;
    SequenceRegion expected;
    SequenceRegion replacement;
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

using Command =
    std::variant<InsertClip, RemoveClip, InsertAutomationLane, RemoveAutomationLane, MoveClip,
                 SetNoteVelocity, SetClipPlaybackProperties, SetTempoMap, SetMeterMap,
                 InsertSequenceMarker, RemoveSequenceMarker, SetSequenceMarker,
                 InsertSequenceRegion, RemoveSequenceRegion, SetSequenceRegion>;

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
bool equivalent(const Command& lhs, const Command& rhs) noexcept;
bool equivalent(const Transaction& lhs, const Transaction& rhs) noexcept;
std::size_t retained_size(const Command& command) noexcept;
std::size_t retained_size(const Transaction& transaction) noexcept;

} // namespace pulp::timeline
