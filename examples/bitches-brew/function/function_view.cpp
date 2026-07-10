// Out-of-line create_view() so the editor's headers stay out of the
// audio-only translation units.
#include "function_processor.hpp"
#include "function_ui.hpp"

namespace pulp::examples::brew {

std::unique_ptr<view::View> FunctionProcessor::create_view() {
    return std::make_unique<FunctionUi>(state(), *this);
}

}  // namespace pulp::examples::brew
