// PulpKit VST3 entry point
#include "pulp_kit.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID PulpKitUID(0x50554C50, 0x4B495400, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(PulpKitUID, "PulpKit", Steinberg::Vst::PlugType::kInstrumentSynth,
                 "Pulp", "1.0.0", "https://github.com/danielraffel/pulp",
                 pulp::examples::create_pulp_kit)
