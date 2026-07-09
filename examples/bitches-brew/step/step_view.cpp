// Out-of-line create_view() so the editor's headers stay out of the
// audio-only translation units.
#include "step_processor.hpp"
#include "step_ui.hpp"

namespace pulp::examples::brew {

std::unique_ptr<view::View> StepProcessor::create_view() {
    return std::make_unique<StepUi>(state(), *this);
}

}  // namespace pulp::examples::brew
