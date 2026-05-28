// Phase iOS-D.1 — AU v3 entry for the iOS GPU smoke example.
// Mirrors examples/ios-auv3-synth/src/au_v3_entry.cpp: registers the
// processor factory with the AU v3 shared entry. Without this TU the
// .appex links but `PulpAUFactory` finds `registered_factory()` == null
// and refuses to instantiate the AU.

#include "gpu_smoke.hpp"
#include <pulp/format/au_v3_entry.hpp>

namespace {
std::unique_ptr<pulp::format::Processor> create_gpu_smoke() {
    return std::make_unique<pulp::examples::ios_gpu_smoke::GpuSmoke>();
}
} // namespace

PULP_AUV3_PLUGIN(create_gpu_smoke)
