#pragma once

/// @file frame_clock.hpp
/// Single authoritative time source for the view system.
/// Advanced externally by the run-loop host (native window, SDL, or test harness).

#include <functional>
#include <vector>
#include <cstdint>

namespace pulp::view {

class FrameClock {
public:
    /// Advance the clock by dt seconds. Called once per frame by the host.
    void tick(float dt);

    /// Current elapsed time (seconds since first tick).
    float time() const { return time_; }

    /// Delta since last tick.
    float dt() const { return dt_; }

    /// Monotonic frame counter.
    uint64_t frame() const { return frame_; }

    /// Subscribe to frame ticks. Callback receives dt.
    /// Return false from callback to unsubscribe automatically.
    int subscribe(std::function<bool(float dt)> callback);

    /// Remove a subscription by ID.
    void unsubscribe(int id);

    /// True if any subscribers are still active (drives repaint/invalidation).
    bool has_active_subscribers() const;

    // ── Activity channel (wake-from-idle probes) ────────────────────────────
    // Distinct from the render subscribers above. An activity subscriber is an
    // idle probe: the host calls pump_activity() every host tick REGARDLESS of
    // render state, so a view can decide each tick whether it now needs
    // continuous frames (e.g. flip set_continuous_repaint when a meter starts
    // moving) WITHOUT the subscription itself counting as render-liveness. This
    // separation is what lets an editor idle at 0 fps yet still wake: render
    // subscribers (subscribe / has_active_subscribers) mean "keep painting";
    // activity subscribers mean "let me look each tick, but don't paint for me".
    // A wake-from-idle liveness flag driven from here is why a per-tick View
    // vtable hook is unnecessary — self-subscribe via View::on_frame_clock_changed.

    /// Subscribe an activity probe. Fired by pump_activity, on the UI thread.
    /// Activity probes do NOT auto-unsubscribe (no bool return); remove with
    /// unsubscribe_activity. Returns an id unique across both channels.
    int subscribe_activity(std::function<void(float dt)> callback);

    /// Remove an activity subscription by id.
    void unsubscribe_activity(int id);

    /// True if any activity probe is registered. Deliberately does NOT feed the
    /// repaint decision — an activity-only view idles at 0 fps.
    bool has_activity_subscribers() const;

    /// Fire every activity probe with dt. Call once per host tick, on the UI
    /// thread, BEFORE the host's repaint decision. Never advances the render
    /// clock (time/dt/frame) and never fires render subscribers — an idle frame
    /// calls this but skips tick().
    void pump_activity(float dt);

    /// Reset all state (for testing).
    void reset();

private:
    struct Subscriber {
        int id;
        std::function<bool(float dt)> callback;
        bool active = true;
    };
    struct ActivitySubscriber {
        int id;
        std::function<void(float dt)> callback;
        bool active = true;
    };

    std::vector<Subscriber> subscribers_;
    std::vector<ActivitySubscriber> activity_subscribers_;
    float time_ = 0;
    float dt_ = 0;
    uint64_t frame_ = 0;
    int next_id_ = 1;
};

} // namespace pulp::view
