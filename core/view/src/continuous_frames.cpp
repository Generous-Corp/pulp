#include <pulp/view/continuous_frames.hpp>

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>  // ScrollView
#include <pulp/view/eq_curve_view.hpp>

namespace pulp::view {

bool needs_continuous_frames(const View* view) {
    if (!view) return false;
    if (view->wants_continuous_repaint()) return true;
    if (view->has_time_driven_gestures()) return true;

    // Any shader-capable widget whose shader declares a `time` uniform must
    // keep painting. One cast covers every CustomShaderHost, so a new
    // shader-capable widget is picked up here for free.
    if (auto* cs = dynamic_cast<const CustomShaderHost*>(view)) {
        if (cs->shader_uses_time()) return true;
    }

    // Mid-flight widget animations (hover glow, thumb travel, hover scale).
    if (auto* k = dynamic_cast<const Knob*>(view)) {
        if (k->hover_glow() > 0.01f && k->hover_glow() < 0.99f) return true;
    }
    if (auto* t = dynamic_cast<const Toggle*>(view)) {
        if (t->thumb_position() > 0.01f && t->thumb_position() < 0.99f) return true;
    }
    if (auto* f = dynamic_cast<const Fader*>(view)) {
        if (f->hover_scale() > 1.01f) return true;
    }
    if (auto* sv = dynamic_cast<const ScrollView*>(view)) {
        if (sv->scroll_animating()) return true;
    }
    // EqCurveView handle-radius hover settle — keep painting only while a dot is
    // mid-transition; the flag clears the frame it settles, so the loop idles.
    // The live analyzer also keeps frames alive, but ONLY while it is enabled AND
    // has spectrum data — a default-on but silent editor with no bound spectrum
    // reports idle here, so it does not spin frames on a static surface.
    if (auto* eq = dynamic_cast<const EqCurveView*>(view)) {
        if (eq->hover_animating()) return true;
        if (eq->analyzer_animating()) return true;
    }

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
