// Three.js demo AU v3 factory entry. Mirrors the pattern
// established by `examples/ios-auv3-synth/src/au_v3_entry.cpp`: this
// TU registers the processor factory with the shared AU v3 entry so
// the .appex's `PulpAUFactory` can construct the audio unit when the
// host calls into `createAudioUnitWithComponentDescription:error:`.
// Without this TU the .appex links but the host finds
// `registered_factory()` == null and refuses to instantiate the AU.

#include "threejs_demo.hpp"

#include <pulp/format/au_v3_entry.hpp>

#include <memory>

namespace {

std::unique_ptr<pulp::format::Processor> create_threejs_demo() {
    return std::make_unique<pulp::examples::ios_threejs::PulpThreeJsDemo>();
}

}  // namespace

PULP_AUV3_PLUGIN(create_threejs_demo)
