// Gong VST3 entry point
#include "gong_instrument.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID GongUID(0x50554C50, 0x474F4E47, 0x504C4154, 0x00000001);

PULP_VST3_PLUGIN(GongUID, "Gong",
                 Steinberg::Vst::PlugType::kInstrumentSynth,
                 "Pulp", "1.0.0", "https://github.com/danielraffel/pulp",
                 pulp::examples::create_gong)
