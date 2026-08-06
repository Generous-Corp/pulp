#pragma once

#include <compare>

#include <pulp/timebase/tick.hpp>

namespace pulp::timebase {

/// The loop bounds a transport honours, in document ticks.
///
/// It lives in the timebase because that is all it is: two document positions
/// and whether they are in force. Nothing here says how audio is produced or
/// how a region is drawn, so the rung that runs the transport and the rung that
/// draws the ruler describe the same loop with the same type rather than two
/// structurally identical ones that have to be kept in step by hand.
///
/// A disabled loop keeps its bounds. `enabled` states whether wrapping happens,
/// not whether a region exists, so turning a loop off and back on returns the
/// user to the region they set up and a view keeps drawing it meanwhile.
struct LoopRegion {
    bool enabled = false;
    TickPosition start{};
    TickPosition end{};

    constexpr auto operator<=>(const LoopRegion&) const = default;
};

} // namespace pulp::timebase
