// Out-of-line create_view() so the editor's headers stay out of the
// audio-only translation units.
#include "quantizer_processor.hpp"
#include "quantizer_ui.hpp"

namespace pulp::examples::brew {

std::unique_ptr<view::View> QuantizerProcessor::create_view() {
    return std::make_unique<QuantizerUi>(state(), *this);
}

}  // namespace pulp::examples::brew
