// PulpPluck VST3 entry point
#include "pulp_pluck.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID PulpPluckUID(0x50554C50, 0x504C4B00, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(PulpPluckUID, "PulpPluck", Steinberg::Vst::PlugType::kInstrumentSynth,
                  "Pulp", "1.0.0", "https://github.com/danielraffel/pulp",
                  pulp::examples::create_pulp_pluck)
