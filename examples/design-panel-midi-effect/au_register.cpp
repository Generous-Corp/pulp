// PulpDesignArp AU — plugin registration
// Registers the processor factory AND provides a symbol the linker won't strip

#include "design_panel_midi_effect.hpp"
#include <pulp/format/registry.hpp>

// Register at static init time
PULP_REGISTER_PLUGIN(pulp::examples::create_design_panel_midi_effect)

// Exported symbol to prevent linker from stripping this TU
extern "C" void pulp_design_arp_force_link() {}
