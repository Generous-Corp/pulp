// THIS RUNS OVER THE WHOLE VIEW TREE ONCE PER FRAME, so what it costs per node
// is what it costs per node times every node times 120 a second.
//
// It used to try SIX `dynamic_cast`s per node — CustomShaderHost, Knob, Toggle,
// Fader, ScrollView, EqCurveView — and three of those are mixins reached
// through multiple inheritance, so each miss walked libc++abi's
// `__vmi_class_type_info` search rather than comparing a pointer. Sampled on an
// idle Forge Modular window on an M3 Ultra, that RTTI search plus this walk was
// the single largest CPU cost in the process: larger than the Skia drawing and
// the Yoga layout it was deciding about. One virtual call replaces all six, and
// each widget answers for itself.
//
// The cost of that is a widget that gains an animation has to say so. A new
// shader-capable widget no longer arrives here for free: override
// `needs_frames_self()` alongside inheriting CustomShaderHost, or a time-driven
// shader will freeze the moment nothing else asks for frames.

#include <pulp/view/continuous_frames.hpp>

#include <pulp/view/view.hpp>

namespace pulp::view {

bool needs_continuous_frames(const View* view) {
    if (!view) return false;
    if (view->wants_continuous_repaint()) return true;
    if (view->has_time_driven_gestures()) return true;
    // Whatever this view is mid-doing: a knob's hover glow, a toggle's thumb
    // travel, a fader's hover scale, a scroll offset still easing, an EQ handle
    // settling or its live analyzer, a body shader with a `time` uniform.
    if (view->needs_frames_self()) return true;

    // A running CSS animation on a generic View must keep the render loop
    // alive: tick_animations() advances it every frame, but without a
    // continuous-frame request the loop stalls once needs_repaint_ clears.
    // Mirror tick_animations()'s own gate (it early-outs when the play state
    // is "paused").
    if (view->animation_play_state() != "paused") {
        for (const auto& a : view->active_animations()) {
            if (a.active) return true;
        }
    }

    for (size_t i = 0; i < view->child_count(); ++i) {
        if (needs_continuous_frames(view->child_at(i))) return true;
    }
    return false;
}

} // namespace pulp::view
