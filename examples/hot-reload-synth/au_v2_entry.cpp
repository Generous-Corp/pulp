// Hot-Reload Synth — AU v2 INSTRUMENT entry (aumu, MusicDeviceBase).
// CATEGORY Instrument in CMake sets the AU type to aumu; PULP_AU_INSTRUMENT wires
// the MusicDevice base so Logic lists it under Instruments and feeds it MIDI.
#include "hot_reload_synth_shell.hpp"
#include <pulp/format/au_v2_instrument_entry.hpp>

PULP_AU_INSTRUMENT(HotReloadSynthAU, pulp::examples::create_hot_reload_synth)
