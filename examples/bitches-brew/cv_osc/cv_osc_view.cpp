// Out-of-line create_view() so the audio translation units never see the
// view stack.
#include "cv_osc_processor.hpp"
#include "cv_osc_ui.hpp"

namespace pulp::examples::brew {

std::unique_ptr<view::View> CvOscProcessor::create_view() {
    return std::make_unique<CvOscUi>(state(), *this);
}

}  // namespace pulp::examples::brew
