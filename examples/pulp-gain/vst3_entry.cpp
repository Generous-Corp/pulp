// PulpGain VST3 entry point
// Uses PULP_VST3_PLUGIN() macro for minimal boilerplate

#include "pulp_gain.hpp"
#include <pulp/format/vst3_entry.hpp>

// Unique ID — stable across versions, never change
static const Steinberg::FUID PulpGainUID(0x50554C50, 0x47414900, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(PulpGainUID, "PulpGain", Steinberg::Vst::PlugType::kFx,
                  "Pulp", "1.0.0", "https://github.com/danielraffel/pulp",
                  pulp::examples::create_pulp_gain)
