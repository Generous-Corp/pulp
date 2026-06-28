#include "super_convolver.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID SuperConvolverUID(0x50554C50, 0x53704376, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(SuperConvolverUID, "SuperConvolver", Steinberg::Vst::PlugType::kFxReverb,
                  "Pulp", "1.0.6", "https://github.com/danielraffel/pulp",
                  pulp::examples::create_super_convolver)
