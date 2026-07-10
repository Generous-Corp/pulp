#include "dc_processor.hpp"
#include <pulp/format/vst3_entry.hpp>

// Frozen at first build. A VST3 plug-in is identified by this UID, not by its
// name — changing it orphans every saved session that references the plug-in.
static const Steinberg::FUID BrewDcUID(0x42744272, 0x44432020, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(BrewDcUID, "DC", Steinberg::Vst::PlugType::kFx,
                  "Bitches Brew", "0.1.0",
                  "https://github.com/danielraffel/bitches-brew",
                  pulp::examples::brew::create_dc)
