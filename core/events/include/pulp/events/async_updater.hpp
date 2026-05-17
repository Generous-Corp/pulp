#pragma once

// AsyncUpdater — coalesce rapid updates to a single callback on the event thread.
// Thread-safe: can be triggered from any thread.

#include <algorithm>
#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::events {

/// Coalescing async update — trigger from any thread, callback runs on event thread.
/// Multiple triggers between callbacks are coalesced into a single call.
class AsyncUpdater {
public:
    AsyncUpdater() = default;
    virtual ~AsyncUpdater() = default;

    /// Trigger an async update. Safe to call from any thread.
    /// If already triggered but not yet handled, the call is coalesced.
    void trigger_async_update() {
        if (!pending_.exchange(true, std::memory_order_acq_rel)) {
            // First trigger since last handle — post to event thread
            post_to_event_thread();
        }
    }

    /// Cancel a pending update.
    void cancel_pending_update() {
        pending_.store(false, std::memory_order_release);
    }

    /// Whether an update is pending.
    bool is_update_pending() const {
        return pending_.load(std::memory_order_acquire);
    }

    /// Override this to handle the async update (runs on event thread).
    virtual void handle_async_update() = 0;

    /// Called by the event loop to process pending updates.
    void process_pending() {
        if (pending_.exchange(false, std::memory_order_acq_rel)) {
            handle_async_update();
        }
    }

private:
    std::atomic<bool> pending_{false};

    // Consumers should poll process_pending() from their event loop.
    void post_to_event_thread() {}
};

/// Lambda-based async updater (convenience)
class LambdaAsyncUpdater : public AsyncUpdater {
public:
    explicit LambdaAsyncUpdater(std::function<void()> callback)
        : callback_(std::move(callback)) {}

    void handle_async_update() override {
        if (callback_) callback_();
    }

private:
    std::function<void()> callback_;
};

/// MultiTimer — manages multiple named timers from a single object.
class MultiTimer {
public:
    MultiTimer() = default;
    virtual ~MultiTimer() = default;

    /// Start a timer with the given ID and interval in milliseconds.
    void start_timer(int timer_id, int interval_ms);

    /// Stop a specific timer.
    void stop_timer(int timer_id);

    /// Whether a timer is running.
    bool is_timer_running(int timer_id) const;

    /// Override to handle timer callbacks.
    virtual void timer_callback(int timer_id) = 0;

    /// Stop all timers.
    void stop_all_timers();

private:
    struct TimerEntry {
        int id;
        int interval_ms;
        bool running = false;
    };
    std::vector<TimerEntry> timers_;
};

/// ActionBroadcaster — string-based action dispatch (for menu commands, etc.)
class ActionBroadcaster {
public:
    using ActionCallback = std::function<void(std::string_view action)>;

    /// Send an action to all listeners
    void send_action(std::string_view action) {
        std::vector<std::pair<int, ActionCallback>> callbacks;
        callbacks.reserve(listeners_.size());
        for (const auto& [id, fn] : listeners_) {
            callbacks.push_back({id, fn});
        }
        for (auto& [id, fn] : callbacks) {
            if (fn && has_listener(id)) fn(action);
        }
    }

    /// Add a listener. Returns an ID for removal.
    int add_listener(ActionCallback fn) {
        int id = next_id_++;
        listeners_.push_back({id, std::move(fn)});
        return id;
    }

    /// Remove a listener by ID.
    void remove_listener(int id) {
        listeners_.erase(
            std::remove_if(listeners_.begin(), listeners_.end(),
                          [id](auto& p) { return p.first == id; }),
            listeners_.end());
    }

private:
    std::vector<std::pair<int, ActionCallback>> listeners_;
    int next_id_ = 0;

    bool has_listener(int id) const {
        return std::any_of(listeners_.begin(), listeners_.end(),
                          [id](const auto& p) { return p.first == id; });
    }
};

/// ScopedLowPowerModeDisabler — prevents OS power throttling during audio processing.
/// On macOS: sets QoS to user-interactive. On Windows: calls SetThreadExecutionState.
class ScopedLowPowerModeDisabler {
public:
    ScopedLowPowerModeDisabler();
    ~ScopedLowPowerModeDisabler();

    ScopedLowPowerModeDisabler(const ScopedLowPowerModeDisabler&) = delete;
    ScopedLowPowerModeDisabler& operator=(const ScopedLowPowerModeDisabler&) = delete;

private:
    bool active_ = false;
};

}  // namespace pulp::events
