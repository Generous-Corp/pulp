// PulpTranspose — CLAP entry point. The MidiEffect category makes it a CLAP
// note effect: note ports in and out, no audio ports.
#include "transpose_processor.hpp"
#include <pulp/format/clap_entry.hpp>

PULP_CLAP_PLUGIN(pulp::examples::transpose::create_transpose)
