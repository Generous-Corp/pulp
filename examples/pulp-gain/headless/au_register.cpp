// PulpGainHeadless AU: processor factory registration at static init time.

#include "pulp_gain_headless.hpp"
#include <pulp/format/registry.hpp>

PULP_REGISTER_PLUGIN(pulp::examples::create_pulp_gain_headless)
