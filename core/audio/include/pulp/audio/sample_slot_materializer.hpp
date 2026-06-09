#pragma once

#include <cstdint>
#include <span>

namespace pulp::audio {

class RealtimeSampleRecorder;
class SampleSlotBank;

// Off-real-time helper. The scratch span must have at least one entry per
// recorder channel and is used only to assemble a non-owning BufferView.
bool materialize_completed_recording_to_slot(
    RealtimeSampleRecorder& recorder,
    SampleSlotBank& bank,
    std::uint32_t slot_index,
    std::span<const float*> channel_scratch) noexcept;

// Off-real-time helper for the normal publication path. Reserves, writes,
// completes, publishes, and handles failure cleanup inside SampleSlotBank.
bool publish_completed_recording_to_slot(
    RealtimeSampleRecorder& recorder,
    SampleSlotBank& bank,
    std::span<const float*> channel_scratch,
    std::uint64_t audio_safe_generation = 0) noexcept;

}  // namespace pulp::audio
