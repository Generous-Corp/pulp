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
    last_os_ = detect_system_policy();
}

MotionPreferences::~MotionPreferences() = default;

MotionPolicy MotionPreferences::policy() const {
    if (override_) return *override_;
    return last_os_;
}

void MotionPreferences::set_duration_scale(double scale) {
    if (scale < 0.0) scale = 0.0;
    if (scale > 2.0) scale = 2.0;
    duration_scale_ = scale;
}

void MotionPreferences::set_override(std::optional<MotionPolicy> p) {
    auto before = policy();
    override_ = p;
    auto after = policy();
    if (before != after) notify_changed(after);
}

bool MotionPreferences::poll() {
    if (override_) return false;
    auto current = detect_system_policy();
    if (current != last_os_) {
        last_os_ = current;
        notify_changed(current);
        return true;
    }
    return false;
}

void MotionPreferences::on_policy_changed(std::function<void(MotionPolicy)> cb) {
    callback_ = std::move(cb);
}

void MotionPreferences::reset_for_tests() {
    override_.reset();
    duration_scale_ = 1.0;
    callback_ = {};
    last_os_ = detect_system_policy();
}

void MotionPreferences::notify_changed(MotionPolicy p) {
    if (callback_) callback_(p);
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
