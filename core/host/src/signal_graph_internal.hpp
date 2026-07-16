// signal_graph_internal.hpp — MIDI-block helpers shared by SignalGraph's
// translation units.
//
// clear_midi_block / midi_block_has_drops / copy_midi_block are used by BOTH
// the routed dispatch in signal_graph.cpp AND the legacy serial reference walk
// in signal_graph_reference_walk.cpp. The definitions live in
// pulp/midi/block_ops.hpp so the host and format layers share one bit-exact
// implementation across the host/format boundary; this header re-exports them
// into pulp::host so the existing call sites resolve unqualified.
#pragma once

#include <pulp/midi/block_ops.hpp>

namespace pulp::host {

using midi::clear_midi_block;
using midi::copy_midi_block;
using midi::midi_block_has_drops;

}  // namespace pulp::host
