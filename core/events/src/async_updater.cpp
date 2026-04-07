#include <pulp/events/async_updater.hpp>

#include <algorithm>

namespace pulp::events {

// ── MultiTimer ──────────────────────────────────────────────────────────────

void MultiTimer::start_timer(int timer_id, int interval_ms) {
    auto it = std::find_if(timers_.begin(), timers_.end(),
                           [timer_id](auto& e) { return e.id == timer_id; });
    if (it != timers_.end()) {
        it->interval_ms = interval_ms;
        it->running = true;
    } else {
        timers_.push_back({timer_id, interval_ms, true});
    }
}

void MultiTimer::stop_timer(int timer_id) {
    auto it = std::find_if(timers_.begin(), timers_.end(),
                           [timer_id](auto& e) { return e.id == timer_id; });
    if (it != timers_.end())
        it->running = false;
}

bool MultiTimer::is_timer_running(int timer_id) const {
    auto it = std::find_if(timers_.begin(), timers_.end(),
                           [timer_id](auto& e) { return e.id == timer_id; });
    return it != timers_.end() && it->running;
}

void MultiTimer::stop_all_timers() {
    for (auto& t : timers_)
        t.running = false;
}

// ── ScopedLowPowerModeDisabler ──────────────────────────────────────────────

ScopedLowPowerModeDisabler::ScopedLowPowerModeDisabler() {
    // Platform-specific power throttle disable (stub — expanded per-platform)
    active_ = true;
}

ScopedLowPowerModeDisabler::~ScopedLowPowerModeDisabler() {
    // Platform-specific restore (stub)
    active_ = false;
}

}  // namespace pulp::events
