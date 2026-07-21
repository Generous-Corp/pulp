// PulpTranspose — AU v2 MIDI-EFFECT entry (aumi, MusicDeviceBase without audio).
// CATEGORY MidiEffect in CMake sets the AU type to aumi; PULP_AU_MIDI_EFFECT
// binds the MIDI-processor adapter to AUMIDIEffectFactory, whose lookup carries
// the MusicDevice MIDIEvent + SysEx selectors the host uses to deliver notes.
#include "transpose_processor.hpp"
#include <pulp/format/au_v2_midi_effect_entry.hpp>

PULP_AU_MIDI_EFFECT(PulpTransposeAU,
                    pulp::examples::transpose::create_transpose)
