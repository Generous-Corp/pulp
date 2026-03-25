// PulpTone AU v2 entry point
// Factory function: PulpToneAUFactory (must match Info.plist.au factoryFunction)

#include "pulp_tone.hpp"
#include "../../core/format/src/au_v2_adapter.cpp"
#include <pulp/format/au_v2_entry.hpp>

PULP_AU_PLUGIN(PulpToneAU, pulp::examples::create_pulp_tone)
