// block_ops.hpp — whole-buffer MIDI block clear / drop-check / copy helpers.
//
// These three operations reset a MIDI block, test whether it dropped any
// message (event / sysex / UMP) against its realtime capacity, and append one
// block's full contents into another while propagating that incompleteness.
// The host graph's reference walk and the format layer's routed executor both
// gather inbound MIDI edge-by-edge, and their bit-exactness (routed-vs-walk
// parity) depends on these semantics being identical on both sides of the
// host/format boundary — which cannot share a header from either layer. They
// live here, in core/midi, the one module both already depend on.
//
// Defined `inline` so every including TU shares one ODR-safe definition.
#pragma once

#include <pulp/midi/buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>

namespace pulp::midi {

// Clear a MIDI block's events, sysex, and attached UMP — the full reset applied
// before gathering a node's inbound MIDI.
inline void clear_midi_block(MidiBuffer& block) noexcept {
    block.clear();
    block.clear_sysex();
    if (auto* ump = block.ump()) ump->clear();
}

// True if the block dropped any event / sysex / UMP message (capacity limit
// hit).
inline bool midi_block_has_drops(const MidiBuffer& block) noexcept {
    if (block.dropped_event_count() > 0 || block.dropped_sysex_count() > 0) {
        return true;
    }
    const auto* ump = block.ump();
    return ump != nullptr && ump->dropped_event_count() > 0;
}

// Append every event / sysex / UMP message from `src` to `dst`. Returns false
// if `src` already carried a drop or an add() dropped here (the incompleteness
// propagated downstream). RT-safe when both buffers are reserved (add() respects
// the realtime capacity limit).
inline bool copy_midi_block(const MidiBuffer& src, MidiBuffer& dst) noexcept {
    bool copied_all = !midi_block_has_drops(src);
    for (const auto& ev : src) {
        if (!dst.add(ev)) copied_all = false;
    }
    for (const auto& sx : src.sysex()) {
        if (sx.data.empty()) {
            if (!dst.add_sysex({}, sx.sample_offset, sx.timestamp)) {
                copied_all = false;
            }
        } else if (!dst.add_sysex_copy(sx.data.data(), sx.data.size(),
                                       sx.sample_offset, sx.timestamp)) {
            copied_all = false;
        }
    }
    const auto* src_ump = src.ump();
    auto* dst_ump = dst.ump();
    if (src_ump != nullptr && dst_ump != nullptr) {
        for (const auto& ev : *src_ump) {
            if (!dst_ump->add(ev)) copied_all = false;
        }
    } else if (src_ump != nullptr && !src_ump->empty()) {
        copied_all = false;
    }
    return copied_all;
}

}  // namespace pulp::midi
