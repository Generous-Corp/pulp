#pragma once

/// @file motion_preferences.hpp
/// System-level reduced-motion preferences. Sibling of `AppearanceTracker`:
/// reads the OS accessibility setting on first use, exposes a polling +
/// callback surface, and accepts a test override that wins over the OS
/// value. Animation primitives (`Tween`, `ValueAnimation`, `TransitionSpec`,
/// `AnimatorSet`) read this at construction / reset to honor the policy:
///
///   - Full     — animate at the configured duration (default).
///   - Reduced  — scale the configured duration by `duration_scale`.
///   - Off      — jump straight to the target value; emit no intermediate
///                tween samples.
///
/// Off by default in the sense that nothing reads it unless an animation
/// primitive starts; the singleton lives behind `current()`.

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::view {

/// Reduced-motion policy. Animation primitives honor this on construction
/// or `reset()`.
enum class MotionPolicy {
    Full,     ///< Animate normally.
    Reduced,  ///< Animate, but scale duration by `duration_scale`.
    Off,      ///< Skip animation; jump to the target value.
};

/// Process-wide reduced-motion preference tracker.
///
/// On macOS, reads `[NSWorkspace sharedWorkspace].accessibilityDisplayShouldReduceMotion`.
/// On Windows, reads `SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, ...)`.
/// On other platforms, defaults to `Full`.
class MotionPreferences {
public:
    /// Process-wide instance. Reads the OS value on first construction.
    static MotionPreferences& instance();

    /// Convenience: the effective policy right now.
    static MotionPolicy current() { return instance().policy(); }

    /// Convenience: the effective duration scale right now.
    static double current_duration_scale() {
        return instance().duration_scale();
    }

    /// Current effective policy. Honors override when set, else returns
    /// the last polled OS value.
    MotionPolicy policy() const;

    /// Current duration scale (default 1.0). Clamped to [0.0, 2.0].
    /// Only meaningful when `policy() == Reduced`; primitives may use it
    /// regardless if they want a softer slowdown under `Full`.
    double duration_scale() const;

    /// Set the duration scale. Clamped to [0.0, 2.0]. Sticks across
    /// override changes; not derived from the OS.
    void set_duration_scale(double scale);

    /// Override the OS-reported policy. Pass `std::nullopt` to revert to
    /// OS detection. Intended for tests and the JS bridge.
    void set_override(std::optional<MotionPolicy> p);

    /// Returns true if an override is currently in effect.
    bool has_override() const noexcept;

    /// Poll the OS for a changed reduced-motion setting. Returns true
    /// when the effective policy changes since the last poll. When an
    /// override is in effect, this is a no-op and returns false.
    bool poll();

    /// Register a callback that fires whenever the effective policy
    /// changes (via `poll()` or `set_override()`).
    void on_policy_changed(std::function<void(MotionPolicy)> callback);

    /// Reset state for tests: clear override, reset duration_scale to
    /// 1.0, drop callbacks, re-read OS value.
    void reset_for_tests();

private:
    MotionPreferences();
    ~MotionPreferences();
    MotionPreferences(const MotionPreferences&) = delete;
    MotionPreferences& operator=(const MotionPreferences&) = delete;

    /// Platform-specific OS detection.
    static MotionPolicy detect_system_policy();

    /// Compute the effective policy under `mtx_`. Caller holds the lock.
    MotionPolicy policy_locked() const;

    /// Snapshot a copy of the callback under `mtx_`, drop the lock, and
    /// invoke. Avoids self-deadlock when a callback re-enters
    /// `MotionPreferences::set_override` / `set_duration_scale`.
    void notify_changed(MotionPolicy p);

    /// Protects every member below; never held across a callback fire.
    mutable std::mutex mtx_;
    MotionPolicy last_os_ = MotionPolicy::Full;
    std::optional<MotionPolicy> override_;
    double duration_scale_ = 1.0;
    std::function<void(MotionPolicy)> callback_;
};

// ── Free-function convenience shims ──────────────────────────────────
//
// Animation primitives use these to keep the call site cheap and avoid
// pulling the full singleton header into hot paths in the future.

/// Apply the current MotionPolicy to a configured duration. Returns the
/// effective duration in seconds. When the policy is `Off`, returns 0.
inline float apply_motion_policy_to_duration(float configured_seconds) {
    auto& prefs = MotionPreferences::instance();
    switch (prefs.policy()) {
        case MotionPolicy::Off:
            return 0.0f;
        case MotionPolicy::Reduced:
            return configured_seconds *
                   static_cast<float>(prefs.duration_scale());
        case MotionPolicy::Full:
        default:
            return configured_seconds;
    }
}

/// Returns true when the current policy says "do not animate" — i.e.
/// the primitive should jump straight to its target value.
inline bool motion_policy_is_off() {
    return MotionPreferences::instance().policy() == MotionPolicy::Off;
}

/// Returns the current policy string for fixture headers.
inline const char* motion_policy_to_string(MotionPolicy p) {
    switch (p) {
        case MotionPolicy::Off:     return "off";
        case MotionPolicy::Reduced: return "reduced";
        case MotionPolicy::Full:
        default:                    return "full";
    }
}

/// Parse a policy string (case-sensitive). Returns Full on unknown.
inline MotionPolicy motion_policy_from_string(const std::string_view s) {
    if (s == "off")     return MotionPolicy::Off;
    if (s == "reduced") return MotionPolicy::Reduced;
    return MotionPolicy::Full;
}

} // namespace pulp::view
