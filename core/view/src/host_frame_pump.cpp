#include <pulp/view/host_frame_pump.hpp>

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>  // ScrollView, Tooltip

namespace pulp::view {

float HostFramePump::measure(double now_seconds) {
    // A vsync the host declined to dispatch (see should_dispatch_host_frame) is a
    // vsync we never measured, so `last_` is stale by however long the tree was
    // idle. That gap is idle time, not animation time — at ANY scale, not just
    // beyond the wake threshold. Consume the flag and resume.
    const bool skipped = skipped_.exchange(false, std::memory_order_acq_rel);
    if (!have_last_ || skipped) {
        // First frame, the first frame after suspend(), or the first frame after
        // the host idled: there is no usable previous presentation timestamp.
        // Re-base and hand out one nominal frame.
        have_last_ = true;
        last_ = now_seconds;
        ++resumes_;
        return nominal_dt_;
    }

    const double raw = now_seconds - last_;
    last_ = now_seconds;

    // Duplicate / coalesced callback, or a source that went backwards. Advancing
    // by a negative dt would rewind the shader clock; advancing by a fabricated
    // positive one would invent time. Neither: this frame took no time.
    if (!(raw > 0.0)) return 0.0f;

    if (raw > static_cast<double>(wake_threshold_)) {
        // Too big to be a slow frame — the host stopped pumping (idle at 0 fps,
        // occluded, suspended). Treat as a resume: one nominal frame, re-based.
        ++resumes_;
        return nominal_dt_;
    }

    // Slow or dropped frames pass through RAW so integrators track wall time.
    return static_cast<float>(raw);
}

void HostFramePump::suspend() {
    have_last_ = false;
}

bool should_dispatch_host_frame(HostFramePump& pump, bool needs_repaint,
                                bool continuous, bool has_idle_callback) {
    if (needs_repaint || continuous || has_idle_callback) return true;
    pump.note_frame_skipped();
    return false;
}

void HostFramePump::set_nominal_dt(float dt) {
    if (dt > 0.0f) nominal_dt_ = dt;
}

void HostFramePump::set_wake_threshold(float seconds) {
    if (seconds > 0.0f) wake_threshold_ = seconds;
}

void HostFramePump::reset() {
    last_ = 0;
    have_last_ = false;
    nominal_dt_ = kDefaultNominalDt;
    wake_threshold_ = kDefaultWakeThreshold;
    resumes_ = 0;
    skipped_.store(false, std::memory_order_release);
}

HostFrameTick begin_host_frame(View* root, FrameClock& clock, HostFramePump& pump,
                               double now_seconds, bool needs_repaint) {
    HostFrameTick tick;
    const std::uint64_t resumes_before = pump.resumes();
    tick.dt = pump.measure(now_seconds);
    tick.resumed = pump.resumes() != resumes_before;

    // Activity probes run every host tick, BEFORE the repaint decision, and never
    // imply a repaint themselves — a probe that flips a liveness flag right now
    // is reflected in this tick's decision.
    clock.pump_activity(tick.dt);

    tick.continuous = needs_continuous_frames(root) || clock.has_active_subscribers();
    tick.should_render = needs_repaint || tick.continuous;
    return tick;
}

void advance_host_frame(View* root, FrameClock& clock, float dt) {
    clock.tick(dt);
    if (!root) return;
    root->advance_gesture_recognizers();
    advance_widget_animations(root, dt);
}

void advance_widget_animations(View* view, float dt) {
    if (!view) return;
    if (auto* k = dynamic_cast<Knob*>(view)) k->advance_animations(dt);
    else if (auto* t = dynamic_cast<Toggle*>(view)) t->advance_animations(dt);
    else if (auto* f = dynamic_cast<Fader*>(view)) f->advance_animations(dt);
    else if (auto* sv = dynamic_cast<ScrollView*>(view)) sv->advance_animations(dt);
    else if (auto* tip = dynamic_cast<Tooltip*>(view)) tip->advance_animations(dt);
    // CSS animation timeline (honors animation-play-state: paused internally).
    view->tick_animations(dt);
    for (std::size_t i = 0; i < view->child_count(); ++i)
        advance_widget_animations(view->child_at(i), dt);
}

} // namespace pulp::view
