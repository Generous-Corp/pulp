// PulpPluck AU v2 instrument entry point
// MUST use MusicDeviceBase (not AUEffectBase) — instruments have no audio input
#include "pulp_pluck.hpp"
#include <pulp/format/au_v2_instrument_entry.hpp>

PULP_AU_INSTRUMENT(PulpPluckAU, pulp::examples::create_pulp_pluck)
