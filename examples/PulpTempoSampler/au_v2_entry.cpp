// PulpTempoSampler AU v2 entry point.
// Factory function: PulpTempoSamplerAUFactory (matches the generated Info.plist).

#include "pulp_tempo_sampler.hpp"
#include <pulp/format/au_v2_entry.hpp>

PULP_AU_PLUGIN(PulpTempoSamplerAU, pulp::examples::create_pulp_tempo_sampler)
