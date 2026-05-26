// PulpHostBench AU v2 entry.
// Format label "AU" is encoded into the log filename so the per-DAW
// scripts can grep for AU-specific quirk evidence.

#include "host_bench.hpp"

#include <pulp/format/au_v2_entry.hpp>

PULP_AU_PLUGIN(PulpHostBenchAU, pulp::examples::create_host_bench_au)
