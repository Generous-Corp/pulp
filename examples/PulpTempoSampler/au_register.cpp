// PulpTempoSampler AU — plugin registration + an un-strippable symbol.

#include "pulp_tempo_sampler.hpp"
#include <pulp/format/registry.hpp>

PULP_REGISTER_PLUGIN(pulp::examples::create_pulp_tempo_sampler)

extern "C" void pulp_tempo_sampler_force_link() {}
