#include <pulp/view/value_source_binding.hpp>

#include <algorithm>

#include <pulp/view/frame_clock.hpp>
#include <pulp/view/view.hpp>

namespace pulp::view {

// ── FrameClockBinding ───────────────────────────────────────────────────────

FrameClockBinding::FrameClockBinding(View& owner, Tick tick, void* context)
    : owner_(owner), tick_(tick), context_(context) {
    owner_.register_value_binding(this);
}

FrameClockBinding::~FrameClockBinding() {
    stop();
    owner_.unregister_value_binding(this);
}

void FrameClockBinding::refresh() {
    FrameClock* clock = owner_.frame_clock();
    if (sub_id_ != -1) {
        if (clock == subscribed_clock_ && wanted_) return; // already on the right clock
        stop();                                            // clock changed / unbound
    }
    if (!wanted_) return; // nothing to read
    if (!clock) return;   // no clock reachable yet (preview / pre-host)
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
    // The clock-mismatch test is a backstop, NOT the re-pointing mechanism: the
    // owner re-points every enrolled binding from its non-virtual funnel, and
    // the only two writers of the reachable clock (View::set_frame_clock and the
    // parent_ assignments in add_child/remove_child) all run it, so no in-tree
    // path arrives here mismatched. It stays because the failure it would
    // otherwise catch is not a stale reading but a dangling unsubscribe: a
    // binding stranded on a clock its owner can no longer reach still holds that
    // clock's raw pointer, and stop() would dereference it. One predictable
    // compare per binding per frame is worth closing that off against a future
    // View that mutates the tree without notifying.
    //
    // Returning false auto-unsubscribes, so clear the cached state WITHOUT
    // calling unsubscribe — we are inside the clock's own dispatch.
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
    // Drop the last snapshot unconditionally. It is the OLD source's reading,
    // so it is stale whether we just unbound (nothing feeds this view now) or
    // re-pointed at a different source (a channel strip retargeted to another
    // plugin instance would paint the previous strip's levels until the next
    // tick). Either way the contract is the same: never a reading no bound
    // source is producing.
    frame_ = MeterFrame{};
    update_wanted();
    refresh();
}

void MeterSourceBinding::set_active(bool active) {
    if (active_ == active) return;
    active_ = active;
    if (!active_) frame_ = MeterFrame{};  // parked: read zero, not the last live value
    update_wanted();
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
    value_ = 0.0f;  // see MeterSourceBinding::set_source
    update_wanted();
    refresh();
}

void ScalarSourceBinding::set_active(bool active) {
    if (active_ == active) return;
    active_ = active;
    if (!active_) value_ = 0.0f;
    update_wanted();
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
