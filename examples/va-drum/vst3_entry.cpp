// VaDrum VST3 entry point
#include "va_drum.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID VaDrumUID(0x50554C50, 0x56414452, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(VaDrumUID, "VaDrum", Steinberg::Vst::PlugType::kInstrumentSynth,
                 "Pulp", "1.0.0", "https://github.com/danielraffel/pulp",
                 pulp::examples::create_va_drum)
