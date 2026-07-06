#include "spectral_lab.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID SpectralLabUID(0x50554C50, 0x53704C62, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(SpectralLabUID, "SpectralLab", Steinberg::Vst::PlugType::kFx,
                  "Pulp", "1.0.1", "https://github.com/danielraffel/pulp",
                  pulp::examples::create_spectral_lab)
