// pulp-multi-out — CLAP entry point (instrument via the descriptor).
// The descriptor's eight output buses become eight CLAP audio output ports.
#include "multi_out_synth.hpp"
#include <pulp/format/clap_entry.hpp>

PULP_CLAP_PLUGIN(pulp::examples::multi_out::create_multi_out_synth)
