// signal_graph_internal.hpp — MIDI-block helpers shared by SignalGraph's
// translation units.
//
// clear_midi_block / midi_block_has_drops / copy_midi_block are used by BOTH
// the routed dispatch in signal_graph.cpp AND the legacy serial reference walk
// in signal_graph_reference_walk.cpp, so they live in this shared header rather
// than in either TU. Defined `inline` so the two TUs share one ODR-safe
// definition with external linkage.
#pragma once

#include <pulp/midi/buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>

namespace pulp::host {

inline void clear_midi_block(midi::MidiBuffer& block) {
    block.clear();
    block.clear_sysex();
    if (auto* ump = block.ump()) ump->clear();
}

inline bool midi_block_has_drops(const midi::MidiBuffer& block) {
    if (block.dropped_event_count() > 0 || block.dropped_sysex_count() > 0) {
        return true;
    }
    const auto* ump = block.ump();
    return ump && ump->dropped_event_count() > 0;
}

inline bool copy_midi_block(const midi::MidiBuffer& src, midi::MidiBuffer& dst) {
    bool copied_all = !midi_block_has_drops(src);
    for (const auto& ev : src) {
        if (!dst.add(ev)) copied_all = false;
    }
    for (const auto& sx : src.sysex()) {
        if (sx.data.empty()) {
            if (!dst.add_sysex({}, sx.sample_offset, sx.timestamp)) {
                copied_all = false;
            }
        } else {
            if (!dst.add_sysex_copy(sx.data.data(), sx.data.size(),
                                    sx.sample_offset, sx.timestamp)) {
                copied_all = false;
            }
        }
    }
    const auto* src_ump = src.ump();
    auto* dst_ump = dst.ump();
    if (src_ump && dst_ump) {
        for (const auto& ev : *src_ump) {
            if (!dst_ump->add(ev)) copied_all = false;
        }
    } else if (src_ump && !src_ump->empty()) {
        copied_all = false;
    }
    return copied_all;
}

}  // namespace pulp::host
