// Phase iOS-D.2 — AU v3 entry for the iOS Chainer example. Mirrors
// examples/ios-auv3-{synth,gpu-smoke}/src/au_v3_entry.cpp.

#include "chainer_synth.hpp"
#include <pulp/format/au_v3_entry.hpp>

namespace {
std::unique_ptr<pulp::format::Processor> create_chainer() {
    return std::make_unique<pulp::examples::ios_chainer::ChainerSynth>();
}
} // namespace

PULP_AUV3_PLUGIN(create_chainer)
