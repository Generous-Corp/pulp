// pulp-multi-out — AU v2 INSTRUMENT entry (aumu, MusicDeviceBase).
// CATEGORY Instrument in CMake sets the AU type to aumu; PULP_AU_INSTRUMENT wires
// the MusicDevice base. The descriptor's eight output buses become eight AU
// output elements, so Logic/Live/Cubase list "Voice 1 (Main)" plus seven aux
// outputs and can route each voice to its own mixer channel.
#include "multi_out_synth.hpp"
#include <pulp/format/au_v2_instrument_entry.hpp>

PULP_AU_INSTRUMENT(PulpMultiOutAU,
                   pulp::examples::multi_out::create_multi_out_synth)
