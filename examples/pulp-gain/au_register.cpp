// PulpGain AU — plugin registration
// Registers the processor factory AND provides a symbol the linker won't strip

#include "pulp_gain.hpp"
#include <pulp/format/registry.hpp>

// Register at static init time
PULP_REGISTER_PLUGIN(pulp::examples::create_pulp_gain)

// Exported symbol to prevent linker from stripping this TU
extern "C" void pulp_gain_force_link() {}
