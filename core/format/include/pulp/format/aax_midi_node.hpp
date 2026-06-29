#pragma once

// Bridge between AAX MIDI nodes and pulp::midi::MidiBuffer.
//
// Extracted from aax_runtime.cpp's anonymous namespace so the thin SDK glue —
// the AAX_CMidiPacket <-> MidiPacketBytes copy on either side of the SDK-free
// reassembler/fragmenter in aax_midi_packets.hpp — is reachable by an SDK-gated
// unit test (test/test_aax_midi_node.cpp). The reassembly algorithm itself is
// already unit-tested SDK-free (test/test_aax_midi.cpp); this seam exists to
// prove the glue feeds it the right bytes/timestamps from real SDK node types.
// #239 AAX parity.
//
// AAX_IMIDINode is forward-declared so this header stays cheap to include; the
// .cpp and the SDK-gated test pull in the full <AAX_IMIDINode.h> / <AAX.h>. The
// definitions live in aax_midi_node.cpp and are only compiled when AAX support
// is enabled (built per-plugin and into the SDK-gated test).

#include <pulp/midi/buffer.hpp>

class AAX_IMIDINode;

namespace pulp::format::aax {

// Walk an input MIDI node's packet stream into `midi_in`: copy each
// AAX_CMidiPacket into an SDK-free MidiPacketBytes and hand the sequence to
// decode_midi_packets(), which runs the F0/continuation/F7 state machine,
// skips malformed packets, drops a dangling SysEx, and sorts. No-ops on a null
// node/buffer.
void decode_midi_node(AAX_IMIDINode* node, midi::MidiBuffer* midi_in);

// Post `midi_out` to an output MIDI node: short messages verbatim, then each
// SysEx fragmented (via fragment_sysex) into 4-byte AAX_CMidiPacket entries
// that share one timestamp. No-ops on a null node.
void encode_midi_node(AAX_IMIDINode* node, const midi::MidiBuffer& midi_out);

} // namespace pulp::format::aax
