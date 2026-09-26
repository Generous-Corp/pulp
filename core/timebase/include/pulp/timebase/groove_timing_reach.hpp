#pragma once

#include <pulp/timebase/groove_kernel.hpp>
#include <pulp/timebase/inline_groove_projector.hpp>

#include <algorithm>
#include <cstdint>

namespace pulp::timebase {

// The largest absolute displacement a groove can apply to an authored tick.
// This is a supremum, so widening an absolute selection window by this value
// cannot miss material that the groove pulls across either edge. The function
// intentionally accepts both the canonical timeline groove and the compact
// realtime projector: both expose the same timing vocabulary.
template <typename Groove> std::int64_t groove_timing_reach(const Groove& groove) noexcept {
    if (groove.states_no_feel() || groove.timing_strength() == 0)
        return 0;

    std::int64_t reach = 0;
    if (const auto grid = groove.swing_grid(); grid.value != 0) {
        const auto extreme = swing_displacement(TickPosition{grid.value}, grid, groove.swing());
        reach = extreme.value < 0 ? -extreme.value : extreme.value;
    }

    std::int64_t widest_step = 0;
    for (const auto& step : groove.steps()) {
        const auto offset = step.timing_offset.value;
        widest_step = std::max(widest_step, offset < 0 ? -offset : offset);
    }
    return reach + widest_step;
}

} // namespace pulp::timebase
