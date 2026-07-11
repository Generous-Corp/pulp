#pragma once

// Shared sample-offset mapping for outbound MIDI across format adapters.
//
// A Processor emits MIDI into `midi_out` during process(), tagging each event
// with a per-event `sample_offset` relative to the start of the current block.
// Every format adapter (AU v2, VST3, CLAP) then hands those events to its host
// with a host-native timestamp. The contract PAM depends on is a cross-format
// parity invariant:
//
//     an event emitted at in-block sample offset N is delivered to the host at
//     offset N, for every valid offset 0 <= N < frame_count.
//
// The three hosts differ ONLY in how they treat out-of-range offsets (a stray
// value a well-behaved Processor never produces):
//   * CLAP `clap_event_header.time` is an unsigned frame index, so a negative
//     offset is clamped up to 0.
//   * AU v2 `MIDIPacket.timeStamp` is a per-block sample offset; the CoreMIDI
//     packet list is time-ordered, so an offset past the block is clamped to
//     the last frame to keep it inside the block.
//   * VST3 `Event.sampleOffset` is a signed int32 the host already clamps, so
//     the adapter passes it through unchanged.
//
// Routing all three adapters through these named helpers turns the parity
// invariant into a single tested contract (test/test_midi_out_offset_parity.cpp)
// rather than three independent open-coded conversions that can silently drift.
//
// Pure functions, header-only, no platform or SDK dependencies.

#include <cstdint>

namespace pulp::format::detail {

/// VST3 outbound offset: `Event.sampleOffset` is a signed int32 the host
/// clamps, so the adapter forwards the Processor's offset unchanged.
constexpr std::int32_t vst3_output_offset(std::int32_t sample_offset) noexcept {
    return sample_offset;
}

/// CLAP outbound offset: `clap_event_header.time` is an unsigned frame index,
/// so a negative offset (never produced by a well-behaved Processor) clamps to
/// the block start.
constexpr std::uint32_t clap_output_offset(std::int32_t sample_offset) noexcept {
    return sample_offset < 0 ? 0u : static_cast<std::uint32_t>(sample_offset);
}

/// AU v2 outbound offset: `MIDIPacket.timeStamp` is an in-block sample offset.
/// Clamp into `[0, frame_count - 1]` so a stray out-of-block offset never lands
/// a packet past the block (mirrors the AU v3 input-side defensive clamp). A
/// zero `frame_count` clamps everything to 0.
constexpr std::int32_t au_output_offset(std::int32_t sample_offset,
                                        std::uint32_t frame_count) noexcept {
    if (sample_offset < 0) return 0;
    if (frame_count > 0 &&
        sample_offset > static_cast<std::int32_t>(frame_count) - 1) {
        return static_cast<std::int32_t>(frame_count) - 1;
    }
    return sample_offset;
}

}  // namespace pulp::format::detail
