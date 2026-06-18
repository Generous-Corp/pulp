#include "src/reference_processor.hpp"
#include <pulp/format/au_v2_entry.hpp>

// MIDI-receiving effect (aumf): the entry registers through
// AUMIDIEffectFactory so MusicDeviceMIDIEvent dispatches to the adapter's
// HandleMIDIEvent. Must stay paired with the aumf type in Info.plist.au and
// descriptor().accepts_midi = true.
PULP_AU_MIDI_PLUGIN(BendrAU, bendr::create_reference)
