#include "src/reference_processor.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID BendrUID(0x42656E64, 0x72526566, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(BendrUID, "Bendr", Steinberg::Vst::PlugType::kFxPitchShift,
                 "Pulp", "0.1.0", "local://bendr", bendr::create_reference)
