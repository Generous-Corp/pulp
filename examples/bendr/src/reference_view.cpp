// Out-of-line create_view() so the UI header (which includes the
// processor header) stays out of the audio-only translation units.
#include "reference_processor.hpp"
#include "reference_ui.hpp"

namespace bendr {

std::unique_ptr<pulp::view::View> ReferenceProcessor::create_view() {
    return std::make_unique<ReferenceUi>(state(), &spectrum_bus(), &midi_map(), &latency_ms());
}

} // namespace bendr
