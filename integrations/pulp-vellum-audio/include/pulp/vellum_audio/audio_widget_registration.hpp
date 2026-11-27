#pragma once

#include <cstddef>

namespace vellum::value {
class ValueFeed;
}

namespace vellum::view {
class View;
}

namespace pulp::vellum_audio {

/// Register the Vellum data displays that back the script factories
/// createMeter ("pulp.level"), createWaveform ("pulp.series") and
/// createSpectrum ("pulp.bins"). Safe to call more than once.
void register_audio_widgets();

/// Bind and sample the audio displays in an editor's mounted view tree.
void sample_audio_widgets(::vellum::view::View& root, ::vellum::value::ValueFeed* feed);

} // namespace pulp::vellum_audio
