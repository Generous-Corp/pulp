// Out-of-line create_view() so the UI header (which includes the canvas /
// view stack) stays out of the audio-only translation units.
#include "super_convolver.hpp"
#include "super_convolver_ui.hpp"

namespace pulp::examples {

std::unique_ptr<view::View> SuperConvolverProcessor::create_view() {
    return std::make_unique<SuperConvolverUi>(state(), spectrum_bus(), *this);
}

} // namespace pulp::examples
