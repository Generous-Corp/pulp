#include "pulp_effect.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID PulpEffectUID(0x50554C50, 0x45464654, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(PulpEffectUID, "PulpEffect", Steinberg::Vst::PlugType::kFx,
                  "Pulp", "1.0.0", "https://github.com/danielraffel/pulp",
                  pulp::examples::create_pulp_effect)
