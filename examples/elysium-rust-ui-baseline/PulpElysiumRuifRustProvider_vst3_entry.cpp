#include "elysium_ruif_processor.hpp"

#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID PulpElysiumRuifRustProviderUID(
    0x50554C50, 0x45525253, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(PulpElysiumRuifRustProviderUID,
                 "PulpElysiumRuifRustProvider",
                 Steinberg::Vst::PlugType::kFx,
                 "Pulp",
                 "0.1.0",
                 "https://github.com/danielraffel/pulp",
                 pulp::examples::create_elysium_ruif_rust_provider_processor)
