// PulpDesignSynth AU v2 entry point.
// Factory function: PulpDesignSynthAUFactory (must match Info.plist.au factoryFunction).

#include "design_panel_instrument.hpp"

#include <pulp/format/au_v2_instrument_entry.hpp>

PULP_AU_INSTRUMENT(PulpDesignSynthAU, pulp::examples::create_design_panel_instrument)
