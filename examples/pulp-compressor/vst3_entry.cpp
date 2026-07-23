#include "pulp_compressor.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID PulpCompressorUID(0x50554C50, 0x434F4D50, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(PulpCompressorUID, "PulpCompressor", Steinberg::Vst::PlugType::kFxDynamics,
                  "Pulp", "1.0.0", "https://github.com/Generous-Corp/pulp",
                  pulp::examples::create_pulp_compressor)
