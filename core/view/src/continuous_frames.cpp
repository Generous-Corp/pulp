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

    // One cheap tag read replaces six RTTI searches per node. Constructors set
    // the tag, so each static_cast below follows a concrete type's own answer.
    bool shader_checked_by_tag = false;
    switch (view->runtime_view_kind()) {
    case RuntimeViewKind::knob: {
        const auto* k = static_cast<const Knob*>(view);
        shader_checked_by_tag = true;
        if (k->shader_uses_time() ||
            (k->hover_glow() > 0.01f && k->hover_glow() < 0.99f)) return true;
        break;
    }
    case RuntimeViewKind::fader: {
        const auto* f = static_cast<const Fader*>(view);
        shader_checked_by_tag = true;
        if (f->shader_uses_time() || f->hover_scale() > 1.01f) return true;
        break;
    }
    case RuntimeViewKind::toggle: {
        const auto* t = static_cast<const Toggle*>(view);
        shader_checked_by_tag = true;
        if (t->shader_uses_time() ||
            (t->thumb_position() > 0.01f && t->thumb_position() < 0.99f))
            return true;
        break;
    }
    case RuntimeViewKind::scroll:
        if (static_cast<const ScrollView*>(view)->scroll_animating()) return true;
        break;
    case RuntimeViewKind::eq_curve: {
        const auto* eq = static_cast<const EqCurveView*>(view);
        if (eq->hover_animating() || eq->analyzer_animating()) return true;
        break;
    }
    default:
        break;
    }

    // Concrete shader widgets take the allocation-free tag path above. Keep
    // the mixin as the fail-closed extension point for every other view: a
    // future host may be untagged or inherit an existing non-shader tag.
    if (!shader_checked_by_tag) {
        if (const auto* shader = dynamic_cast<const CustomShaderHost*>(view);
            shader && shader->shader_uses_time())
            return true;
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
