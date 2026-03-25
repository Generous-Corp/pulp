// PulpTone VST3 entry point

#include "pulp_tone.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID PulpToneUID(0x50554C50, 0x544F4E45, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(PulpToneUID, "PulpTone", Steinberg::Vst::PlugType::kInstrumentSynth,
                  "Pulp", "1.0.0", "https://github.com/danielraffel/pulp",
                  pulp::examples::create_pulp_tone)
