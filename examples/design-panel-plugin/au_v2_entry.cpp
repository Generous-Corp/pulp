// PulpDesignPanel AU v2 entry point.
// Factory function: PulpDesignPanelAUFactory (must match Info.plist.au factoryFunction).

#include "design_panel_plugin.hpp"

#include <pulp/format/au_v2_entry.hpp>

PULP_AU_PLUGIN(PulpDesignPanelAU, pulp::examples::create_design_panel)
