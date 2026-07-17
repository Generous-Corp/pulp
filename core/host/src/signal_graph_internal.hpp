// signal_graph_internal.hpp — MIDI-block helpers shared by SignalGraph's
// translation units.
//
// clear_midi_block / midi_block_has_drops / copy_midi_block are used by BOTH
// the routed dispatch in signal_graph.cpp AND the legacy serial reference walk
// in signal_graph_reference_walk.cpp. The definitions live in
// pulp/midi/block_ops.hpp so the host and format layers share one bit-exact
// implementation across the host/format boundary; this header re-exports them
// into pulp::host so the existing call sites resolve unqualified.
//
// prepare_midi_block_storage is the one place that fixes a graph MIDI block's
// real-time capacities, shared by the snapshot/compile path in signal_graph.cpp
// and the live-swap warm-up probe in signal_graph_live_swap.cpp so every graph
// MIDI buffer is reserved identically.
#pragma once

#include <pulp/host/parameter_event_queue.hpp>
#include <pulp/midi/block_ops.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>

#include <cstddef>

namespace pulp::host {

using midi::clear_midi_block;
using midi::copy_midi_block;
using midi::midi_block_has_drops;

inline constexpr std::size_t kGraphMidiEventCapacity =
    state::ParameterEventQueue::kCapacity;
inline constexpr std::size_t kGraphMidiSysexCapacity = 128;
inline constexpr std::size_t kGraphMidiSysexPayloadCapacity = 4096;

inline void prepare_midi_block_storage(midi::MidiBuffer& block,
                                       midi::UmpBuffer& ump) {
    block.reserve(kGraphMidiEventCapacity,
                  kGraphMidiSysexCapacity,
                  kGraphMidiSysexPayloadCapacity);
    block.set_realtime_capacity_limit(true);
    ump.reserve(kGraphMidiEventCapacity);
    ump.set_realtime_capacity_limit(true);
    block.attach_ump(&ump);
}

}  // namespace pulp::host
