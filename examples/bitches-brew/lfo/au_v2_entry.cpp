#include "lfo_processor.hpp"
#include <pulp/format/au_v2_entry.hpp>

// The class name is load-bearing: AUSDK_COMPONENT_ENTRY exports
// `<ClassName>Factory`, and the bundle's exported-symbol list names
// `_<cmake-target>AUFactory`. ClassName must therefore be exactly the CMake
// target (`BrewLFO`) with an `AU` suffix, matching case.
//
// The MIDI entry, not the plain one. `Reset By Note` reads note-ons, the
// descriptor sets `accepts_midi`, and the bundle is therefore typed `aumf` — the
// pairing rule in au_v2_entry.hpp. With AUBaseFactory the host's MusicDeviceMIDIEvent
// call returns unimpErr and auval fails outright.
PULP_AU_MIDI_PLUGIN(BrewLFOAU, pulp::examples::brew::create_lfo)
