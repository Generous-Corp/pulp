// iOS AUv3 sine-synth entry — registers the processor factory with the AU
// v3 shared entry. Without this TU the .appex links but the host's call to
// `PulpAUFactory` finds `registered_factory()` == null and refuses to
// instantiate the AU. Phase 3.5 added the PULP_AUV3_PLUGIN helper to make
// this wiring symmetrical with the CLAP / AU v2 entries.

#include "sine_synth.hpp"
#include <pulp/format/au_v3_entry.hpp>

namespace {
std::unique_ptr<pulp::format::Processor> create_sine_synth() {
    return std::make_unique<pulp::examples::ios_synth::SineSynth>();
}
} // namespace

PULP_AUV3_PLUGIN(create_sine_synth)
