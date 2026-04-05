// PulpGain AU v2 entry point.
// Factory function: PulpGainAUFactory (must match Info.plist.au factoryFunction).

#include "pulp_gain.hpp"

#include <pulp/format/au_v2_entry.hpp>

PULP_AU_PLUGIN(PulpGainAU, pulp::examples::create_pulp_gain)
