// Out-of-line create_view() so the UI header (which includes the canvas /
// view stack) stays out of the audio-only translation units.
#include "spectral_lab.hpp"
#include "spectral_lab_ui.hpp"

namespace pulp::examples {

std::unique_ptr<view::View> SpectralLabProcessor::create_view() {
    return std::make_unique<SpectralLabUi>(state(), spectrum_bus(), *this);
}

} // namespace pulp::examples
