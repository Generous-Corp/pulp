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

    /// Reset all state (for testing).
    void reset();

private:
    struct Subscriber {
        int id;
        std::function<bool(float dt)> callback;
        bool active = true;
    };

    std::vector<Subscriber> subscribers_;
    float time_ = 0;
    float dt_ = 0;
    uint64_t frame_ = 0;
    int next_id_ = 1;
};

} // namespace pulp::view
