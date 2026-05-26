// PulpHostBench VST3 entry. Stable UID — never change in-place.

#include "host_bench.hpp"

#include <pulp/format/vst3_entry.hpp>

// Reserved bench UID. The first word "HOST" is intentional; the second
// word "BNCH" makes the asset trivially greppable across crash logs.
static const Steinberg::FUID PulpHostBenchUID(
    0x484F5354,  // 'HOST'
    0x424E4348,  // 'BNCH'
    0x00000001,
    0x00000001);

PULP_VST3_PLUGIN(PulpHostBenchUID, "PulpHostBench",
                  Steinberg::Vst::PlugType::kFx,
                  "Pulp", "1.0.0",
                  "https://github.com/danielraffel/pulp",
                  pulp::examples::create_host_bench_vst3)
