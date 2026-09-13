#pragma once // Private playback implementation detail.

#include <pulp/timebase/tick.hpp>
#include <pulp/timeline/clip.hpp>

#include <vector>

namespace pulp::playback {

/// One placement-fade ramp a flattened leaf sits under, in owner-timeline ticks.
///
/// The two ends name what the ramp reads rather than which way it points:
/// `silent_tick` is where it reads zero and `open_tick` where it reads unity,
/// so a fade in and a fade out are the same shape with the ends exchanged.
/// Progress is `(t - silent_tick) / (open_tick - silent_tick)` clamped to the
/// unit interval, and the authored shape reparameterizes that progress.
///
/// Either end may fall outside the leaf that carries it. Flattening cuts a
/// nested window into leaves at clip boundaries and never at a ramp edge, so a
/// leaf routinely spans only part of one ramp; keeping the whole ramp and
/// evaluating the position is what makes two neighbouring leaves read the same
/// gain at the tick they share.
struct LoweredPlacementFade {
    timebase::TickPosition silent_tick;
    timebase::TickPosition open_tick;
    timeline::ClipFadeShape shape = timeline::ClipFadeShape::Linear;
};

} // namespace pulp::playback
