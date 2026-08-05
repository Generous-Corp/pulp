// PulpDesignArp AU v2 entry point.
// Factory function: PulpDesignArpAUFactory (must match Info.plist.au factoryFunction).

#include "design_panel_midi_effect.hpp"

#include <pulp/format/au_v2_midi_effect_entry.hpp>

PULP_AU_MIDI_EFFECT(PulpDesignArpAU, pulp::examples::create_design_panel_midi_effect)
