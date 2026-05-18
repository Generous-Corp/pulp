#include <pulp/view/motion_preferences.hpp>

#include <algorithm>

#if __APPLE__
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#define PULP_HAS_MAC_MOTION_PREFS 1
#endif
#endif

#if PULP_HAS_MAC_MOTION_PREFS
// Implemented in platform/mac/motion_preferences_mac.mm
namespace pulp::view::platform {
    MotionPolicy detect_mac_motion_policy();
}
#elif _WIN32
// Implemented in platform/win/motion_preferences_win.cpp
namespace pulp::view::platform {
    MotionPolicy detect_win_motion_policy();
}
#endif

namespace pulp::view {

// ── MotionPreferences ───────────────────────────────────────────────────────

MotionPreferences& MotionPreferences::instance() {
    static MotionPreferences singleton;
    return singleton;
}

MotionPreferences::MotionPreferences() {
    // Construction runs once under the `static` init guard; no
    // contention on `mtx_` is possible at this point.
    last_os_ = detect_system_policy();
}

MotionPreferences::~MotionPreferences() = default;

MotionPolicy MotionPreferences::policy_locked() const {
    if (override_) return *override_;
    return last_os_;
}

MotionPolicy MotionPreferences::policy() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return policy_locked();
}

double MotionPreferences::duration_scale() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return duration_scale_;
}

bool MotionPreferences::has_override() const noexcept {
    std::lock_guard<std::mutex> lock(mtx_);
    return override_.has_value();
}

void MotionPreferences::set_duration_scale(double scale) {
    if (scale < 0.0) scale = 0.0;
    if (scale > 2.0) scale = 2.0;
    std::lock_guard<std::mutex> lock(mtx_);
    duration_scale_ = scale;
}

void MotionPreferences::set_override(std::optional<MotionPolicy> p) {
    MotionPolicy after{};
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const auto before = policy_locked();
        override_ = p;
        after = policy_locked();
        changed = (before != after);
    }
    if (changed) notify_changed(after);
}

bool MotionPreferences::poll() {
    MotionPolicy current{};
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (override_) return false;
        current = detect_system_policy();
        if (current != last_os_) {
            last_os_ = current;
            changed = true;
        }
    }
    if (changed) notify_changed(current);
    return changed;
}

void MotionPreferences::on_policy_changed(std::function<void(MotionPolicy)> cb) {
    std::lock_guard<std::mutex> lock(mtx_);
    callback_ = std::move(cb);
}

void MotionPreferences::reset_for_tests() {
    std::lock_guard<std::mutex> lock(mtx_);
    override_.reset();
    duration_scale_ = 1.0;
    callback_ = {};
    last_os_ = detect_system_policy();
}

void MotionPreferences::notify_changed(MotionPolicy p) {
    // Snapshot the callback under the lock so a re-entrant callback
    // (one that calls back into MotionPreferences::set_override or
    // set_duration_scale) can't self-deadlock.
    std::function<void(MotionPolicy)> cb_copy;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        cb_copy = callback_;
    }
    if (cb_copy) cb_copy(p);
}

MotionPolicy MotionPreferences::detect_system_policy() {
#if PULP_HAS_MAC_MOTION_PREFS
    return platform::detect_mac_motion_policy();
#elif _WIN32
    return platform::detect_win_motion_policy();
#else
    return MotionPolicy::Full;
#endif
}

} // namespace pulp::view
