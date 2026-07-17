// PreparedPiano VST3 entry point
#include "prepared_piano.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID PreparedPianoUID(0x50554C50, 0x50524550, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(PreparedPianoUID, "PreparedPiano",
                 Steinberg::Vst::PlugType::kInstrumentSynth,
                 "Pulp", "1.0.0", "https://github.com/danielraffel/pulp",
                 pulp::examples::create_prepared_piano)
