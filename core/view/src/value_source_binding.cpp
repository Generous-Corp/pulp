#include <pulp/view/value_source_binding.hpp>

#include <algorithm>

#include <pulp/view/frame_clock.hpp>
#include <pulp/view/view.hpp>

namespace pulp::view {

// ── FrameClockBinding ───────────────────────────────────────────────────────

void FrameClockBinding::refresh(bool wanted) {
    FrameClock* clock = owner_.frame_clock();
    if (sub_id_ != -1) {
        if (clock == subscribed_clock_ && wanted) return; // already on the right clock
        stop();                                           // clock changed / unbound
    }
    if (!wanted) return; // nothing to read
    if (!clock) return;  // no clock reachable yet (preview / pre-host)
    subscribed_clock_ = clock;
    sub_id_ = clock->subscribe([this](float dt) { return on_frame(dt); });
}

void FrameClockBinding::stop() {
    if (sub_id_ != -1 && subscribed_clock_) {
        subscribed_clock_->unsubscribe(sub_id_);
    }
    sub_id_ = -1;
    subscribed_clock_ = nullptr;
}

bool FrameClockBinding::on_frame(float dt) {
    // Self-heal teardown: if the owner was detached / re-parented so the
    // reachable clock no longer matches the one we subscribed to, drop the
    // subscription. Returning false auto-unsubscribes, so clear the cached
    // state WITHOUT calling unsubscribe (we are inside the clock's own
    // dispatch). This covers detaches that never fire on_detached on every
    // descendant.
    if (owner_.frame_clock() != subscribed_clock_ || !tick_(context_, dt)) {
        sub_id_ = -1;
        subscribed_clock_ = nullptr;
        return false;
    }
    return true;
}

// ── MeterSourceBinding ──────────────────────────────────────────────────────

void MeterSourceBinding::set_source(std::shared_ptr<MeterSource> source, int channel) {
    source_ = std::move(source);
    channel_ = channel < 0 ? 0 : channel;
    // Drop the last snapshot on unbind: nothing feeds this view now, and a
    // reader that kept painting the final reading would show a meter frozen at
    // a level the plugin is no longer producing.
    if (!source_) frame_ = MeterFrame{};
    refresh();
}

bool MeterSourceBinding::tick(void* context, float dt) {
    auto* self = static_cast<MeterSourceBinding*>(context);
    if (!self->source_) return false; // unbound → auto-unsubscribe
    self->frame_ = self->source_->read();
    self->binding_.owner().on_meter_frame(self->frame_, dt);
    return true;
}

// ── ScalarSourceBinding ─────────────────────────────────────────────────────

void ScalarSourceBinding::set_source(std::shared_ptr<ScalarSource> source) {
    source_ = std::move(source);
    if (!source_) value_ = 0.0f; // see MeterSourceBinding::set_source
    refresh();
}

bool ScalarSourceBinding::tick(void* context, float dt) {
    auto* self = static_cast<ScalarSourceBinding*>(context);
    if (!self->source_) return false; // unbound → auto-unsubscribe
    self->value_ = self->source_->read();
    (void)dt;
    return true;
}

} // namespace pulp::view
