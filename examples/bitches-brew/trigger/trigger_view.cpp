// Out-of-line create_view() so the editor's headers stay out of the
// audio-only translation units.
#include "trigger_processor.hpp"
#include "trigger_ui.hpp"

namespace pulp::examples::brew {

std::unique_ptr<view::View> TriggerProcessor::create_view() {
    return std::make_unique<TriggerUi>(state(), *this);
}

}  // namespace pulp::examples::brew
