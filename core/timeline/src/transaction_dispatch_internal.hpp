#pragma once

#include <pulp/timeline/command.hpp>

#include <type_traits>

namespace pulp::timeline::detail {

// Which reduce branch claims which Command alternative, stated once.
//
// The dispatch in transaction.cpp is a chain of family predicates interleaved
// with inline branches, and the chain proves nothing about its own coverage.
// pulp-timeline builds -fno-exceptions, so an alternative that falls off the end
// of it does not raise bad_variant_access and does not become a conflict — it
// aborts the process.
//
// Each family predicate is derived from the matching list below rather than
// repeating it, so the two cannot drift, and transaction.cpp asserts that the
// lists together claim every alternative exactly once. Claiming none fails to
// compile; claiming twice fails as well, which matters because a count-only
// check cannot see that case: one double claim and one unclaimed alternative
// sum to the right total and cancel each other out.
template <typename T, typename... Claimed>
inline constexpr bool claimed_by = (std::is_same_v<T, Claimed> || ...);

template <typename T>
inline constexpr bool is_automation_command_type =
    claimed_by<T, InsertAutomationLane, RemoveAutomationLane>;

template <typename T>
inline constexpr bool is_take_command_type =
    claimed_by<T, InsertTakeLane, RemoveTakeLane, SetRecordArm, InsertTake, RemoveTake,
               SetActiveTakeLane, SetTakeComp>;

template <typename T>
inline constexpr bool is_marker_command_type =
    claimed_by<T, InsertMarker, RemoveMarker, InsertRegion, RemoveRegion>;

template <typename T>
inline constexpr bool is_scene_command_type =
    claimed_by<T, InsertScene, RemoveScene, InsertSlot, RemoveSlot>;

template <typename T>
inline constexpr bool is_track_command_type = claimed_by<T, InsertTrack, RemoveTrack, MoveTrack>;

template <typename T>
inline constexpr bool is_track_state_command_type =
    claimed_by<T, SetTrackFreeze, SetTrackMixer, SetTrackName>;

template <typename T>
inline constexpr bool is_sequence_command_type =
    claimed_by<T, InsertSequence, CloneSequence, RemoveSequence, SetClipSequenceRef>;

template <typename T>
inline constexpr bool is_note_command_type = claimed_by<T, SetNoteVelocity, ReplaceNoteContent>;

// Alternatives reduced by an inline branch in transaction.cpp rather than by a
// family reducer. They have no predicate of their own, so the list is their
// only statement of the claim.
template <typename T>
inline constexpr bool is_inline_command_type =
    claimed_by<T, InsertClip, RemoveClip, MoveClip, SetTempoMap, SetMeterMap, CreateAsset,
               RemoveAsset, SetChordScaleLane, SetGroove, SetClipPlaybackProperties>;

} // namespace pulp::timeline::detail
