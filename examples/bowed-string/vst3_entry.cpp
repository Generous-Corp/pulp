// BowedString VST3 entry point
#include "bowed_string_instrument.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID BowedStringUID(0x50554C50, 0x424F5745, 0x44535452, 0x00000001);

PULP_VST3_PLUGIN(BowedStringUID, "BowedString",
                 Steinberg::Vst::PlugType::kInstrumentSynth,
                 "Pulp", "1.0.0", "https://github.com/danielraffel/pulp",
                 pulp::examples::create_bowed_string)
