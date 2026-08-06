#include "pulp_delay.hpp"

#include "pulp_delay_editor.hpp"

namespace pulp::examples::delay {

std::unique_ptr<view::View> PulpDelayProcessor::create_view() {
    return ui::build_pulp_delay_editor(state());
}

} // namespace pulp::examples::delay
