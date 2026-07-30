#include "pulp_delay.hpp"

#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID PulpDelayUID(0x50554C50, 0x444C5900, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(PulpDelayUID, "Pulp Delay", Steinberg::Vst::PlugType::kFx, "Pulp", "1.0.0",
                 "https://github.com/Generous-Corp/pulp", pulp::examples::delay::create_pulp_delay)
