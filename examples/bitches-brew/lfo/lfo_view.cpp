// Out-of-line create_view() so the editor's headers stay out of the
// audio-only translation units.
#include "lfo_processor.hpp"
#include "lfo_ui.hpp"

namespace pulp::examples::brew {

std::unique_ptr<view::View> LfoProcessor::create_view() {
    return std::make_unique<LfoUi>(state(), *this);
}

}  // namespace pulp::examples::brew
