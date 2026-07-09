#include "function_processor.hpp"
#include <pulp/format/vst3_entry.hpp>

// Frozen: a VST3 host keys saved state on this UID. Never regenerate it.
static const Steinberg::FUID BrewFunctionUID(0x42744272, 0x464E4354, 0x00000001,
                                             0x00000001);

PULP_VST3_PLUGIN(BrewFunctionUID, "Function", Steinberg::Vst::PlugType::kFx,
                 "Bitches Brew", "0.1.0",
                 "https://github.com/danielraffel/bitches-brew",
                 pulp::examples::brew::create_function)
