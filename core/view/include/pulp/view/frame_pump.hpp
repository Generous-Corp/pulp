#pragma once

/// @file frame_pump.hpp
/// The per-host-tick "pump then gate" step shared by every render loop.
///
/// Every host (native window, plugin-view host, foreign-host embed tick) runs a
/// per-tick loop that must (1) give idle/wake-from-idle logic a chance to run
/// every tick even when the surface is static, then (2) decide whether this
/// frame actually needs compositing. Doing those in the wrong order is the bug:
/// if the repaint decision runs before the wake-from-idle probes, a control that
/// just became live (a meter starting to move) is missed for a frame. Hosts also
/// tend to duplicate the decision expression, so it drifts between platforms.
///
/// `pump_view_frame` centralises both: it pumps the FrameClock's activity probes
/// (see FrameClock::pump_activity — these run every tick and never imply a
/// repaint), then returns whether the frame should be composited. The host calls
/// this every tick; only when it returns true does the host advance the render
/// clock (`FrameClock::tick`) and paint. A static editor whose probes report "not
/// moving" idles at 0 fps; the moment a probe flips `set_continuous_repaint` (or
/// a render subscriber / animation appears), the very next tick renders.
///
/// Thread affinity: call on the view-owning UI thread only. It walks the view
/// tree (needs_continuous_frames) and fires activity probes without locking, so
/// it must not run concurrently with view-hierarchy or animation mutation.

#include <pulp/view/continuous_frames.hpp>
#include <pulp/view/frame_clock.hpp>

namespace pulp::view {

class View;

/// Pump `clock`'s activity probes with `dt`, then report whether this frame needs
/// compositing. `needs_repaint` is the host's own already-dirty flag (input,
/// resize, explicit invalidate). Returns true if the host should render this
/// frame: `needs_repaint || needs_continuous_frames(root) ||
/// clock.has_active_subscribers()`. Null-safe on `root`.
///
/// Order matters and is the reason this is a helper: the activity pump runs
/// BEFORE the decision, so a probe that flips a liveness flag this tick is
/// reflected in this tick's result.
inline bool pump_view_frame(View* root, FrameClock& clock, float dt,
                            bool needs_repaint) {
    clock.pump_activity(dt);
    return needs_repaint || needs_continuous_frames(root) ||
           clock.has_active_subscribers();
}

} // namespace pulp::view
