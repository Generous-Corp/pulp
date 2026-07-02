// Out-of-line create_view() so the UI header (which includes the canvas / view
// stack) stays out of the audio-only translation units.
#include "gpu_nam_processor.hpp"
#include "gpu_nam_ui.hpp"

namespace pulp::examples {

std::unique_ptr<view::View> GpuNamProcessor::create_view() {
    return std::make_unique<GpuNamUi>(state(), *this);
}

} // namespace pulp::examples
