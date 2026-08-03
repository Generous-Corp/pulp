// PulpDesignPanel AU — plugin registration
// Registers the processor factory AND provides a symbol the linker won't strip

#include "design_panel_plugin.hpp"
#include <pulp/format/registry.hpp>

// Register at static init time
PULP_REGISTER_PLUGIN(pulp::examples::create_design_panel)

// Exported symbol to prevent linker from stripping this TU
extern "C" void pulp_gain_force_link() {}
