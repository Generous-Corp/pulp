// PulpGain AU — plugin registration
// This registers the PulpGain processor factory so the AU adapter can find it

#include "pulp_gain.hpp"
#include <pulp/format/registry.hpp>

PULP_REGISTER_PLUGIN(pulp::examples::create_pulp_gain)
