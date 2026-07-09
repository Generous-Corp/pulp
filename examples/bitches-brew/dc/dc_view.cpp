// Out-of-line create_view() so the editor's headers stay out of the
// audio-only translation units.
#include "dc_processor.hpp"
#include "dc_ui.hpp"

namespace pulp::examples::brew {

std::unique_ptr<view::View> DcProcessor::create_view() {
    return std::make_unique<DcUi>(state());
}

}  // namespace pulp::examples::brew
