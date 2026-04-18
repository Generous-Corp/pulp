#pragma once

/// @file environment.hpp
/// Unified environment API: display, safe-area, keyboard, orientation,
/// color scheme, foreground/background, memory pressure.
///
/// Each platform populates the fields it can observe; unsupported fields
/// retain their default. Listeners receive a snapshot of the new state
/// + a bitmask of which fields changed since the last notification.
///
/// Thread model: Environment is meant to be read from any thread (the
/// snapshot accessor returns a value copy). Listeners are invoked on the
/// thread that delivered the platform event — typically the main UI
/// thread — but no global locking is held during dispatch, so listener
/// bodies that touch shared state must apply their own synchronization.
///
/// This header defines the contract and an in-process notifier. Platform
/// adapters live under `core/platform/platform/{android,ios,mac,win,linux}/`
/// and call Environment::dispatcher().publish(...) when their respective
/// OS callbacks fire (Android `Configuration.onConfigurationChanged`,
/// iOS `UITraitCollection`, macOS `NSApplicationDidChangeScreenParameters`,
/// Windows `WM_DPICHANGED`/`WM_SETTINGCHANGE`, Linux XSettings + Wayland
/// fractional scale).

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pulp::platform {

/// Light/dark color scheme reported by the OS.
enum class ColorScheme : uint8_t {
    light,
    dark,
    /// Platform did not report; treat as `dark` per Pulp's default theme.
    unknown,
};

/// Coarse foreground/background lifecycle state. Mobile platforms drive
/// this from the activity/scene lifecycle; desktop drives from the
/// active-window observer (NSApp resignActive on macOS, WM_ACTIVATEAPP
/// on Windows, _NET_ACTIVE_WINDOW on X11).
enum class LifecycleState : uint8_t {
    foreground,
    /// App is visible but not the focused window (split-screen, banner).
    inactive,
    background,
    unknown,
};

/// Device orientation. `flat` is "screen facing up" — used by mobile to
/// disable orientation-driven layout when the device is lying on a table.
enum class Orientation : uint8_t {
    portrait,
    portrait_upside_down,
    landscape_left,
    landscape_right,
    flat,
    unknown,
};

/// Memory pressure level. Mirrors Android's TRIM_MEMORY tiers + iOS's
/// `didReceiveMemoryWarning` (which becomes `critical`). Desktop
/// platforms emit `normal` only — they don't have a kernel-driven LMK.
enum class MemoryPressure : uint8_t {
    normal,
    /// Some background processes were killed; trim soft caches.
    moderate,
    /// Foreground app is the next likely target; release optional state.
    critical,
};

/// Safe-area insets in CSS-pixel space (already divided by content_scale).
/// Top/bottom cover the iOS notch + home indicator and Android cutouts;
/// left/right cover landscape notches and rounded display corners.
struct SafeAreaInsets {
    float top    = 0.0f;
    float bottom = 0.0f;
    float left   = 0.0f;
    float right  = 0.0f;

    bool is_zero() const noexcept {
        return top == 0.0f && bottom == 0.0f && left == 0.0f && right == 0.0f;
    }
};

/// Soft-keyboard inset (mobile only). The on-screen keyboard occupies
/// `bottom` pixels of the display from the bottom edge; layout should
/// translate / shrink accordingly so focused inputs remain visible.
struct KeyboardInset {
    float bottom = 0.0f;
    /// Animation duration the OS reported for the show/hide transition,
    /// in seconds. Zero when not animating or unknown.
    float animation_duration = 0.0f;
};

/// Logical display geometry. `width` and `height` are in CSS pixels;
/// `physical_*` are the OS's reported pixel resolution. `scale` is the
/// device pixel ratio (NSScreen backingScaleFactor / Android density /
/// Windows DPI / 96.0).
struct DisplayInfo {
    float width        = 0.0f;
    float height       = 0.0f;
    int   physical_width  = 0;
    int   physical_height = 0;
    float scale         = 1.0f;
    /// Refresh rate in Hz. Zero when unknown.
    float refresh_hz    = 0.0f;
    /// Optional human-readable name (e.g. "Built-in Retina Display").
    std::string name;
};

/// Bitmask of fields that changed in the snapshot delivered to a listener.
/// Listeners should inspect this rather than diff the whole struct.
struct EnvironmentChange {
    bool display          : 1;
    bool safe_area        : 1;
    bool keyboard         : 1;
    bool orientation      : 1;
    bool color_scheme     : 1;
    bool lifecycle        : 1;
    bool memory_pressure  : 1;

    EnvironmentChange() noexcept
        : display(false), safe_area(false), keyboard(false),
          orientation(false), color_scheme(false), lifecycle(false),
          memory_pressure(false) {}

    bool any() const noexcept {
        return display || safe_area || keyboard || orientation
            || color_scheme || lifecycle || memory_pressure;
    }
};

/// Snapshot of the full environment state. Listener callbacks receive
/// a `(state, change)` pair so they can branch on `change.color_scheme`
/// without diffing the previous snapshot themselves.
struct EnvironmentState {
    DisplayInfo     display;
    SafeAreaInsets  safe_area;
    KeyboardInset   keyboard;
    Orientation     orientation     = Orientation::unknown;
    ColorScheme     color_scheme    = ColorScheme::unknown;
    LifecycleState  lifecycle       = LifecycleState::unknown;
    MemoryPressure  memory_pressure = MemoryPressure::normal;
};

/// In-process notifier. Listeners survive until the returned token is
/// destroyed (RAII subscription pattern, same as runtime::Logger).
///
/// The notifier is intentionally a singleton — there's exactly one OS
/// reporting these signals per process. Tests can call `inject_for_test`
/// to drive the singleton without a real platform adapter.
class Environment {
public:
    using Listener = std::function<void(const EnvironmentState&,
                                        EnvironmentChange)>;

    /// RAII subscription. Drop to unsubscribe.
    class Token {
    public:
        Token() = default;
        ~Token() { reset(); }
        Token(Token&& other) noexcept;
        Token& operator=(Token&& other) noexcept;
        Token(const Token&) = delete;
        Token& operator=(const Token&) = delete;
        void reset() noexcept;
        bool valid() const noexcept { return id_ != 0; }
    private:
        friend class Environment;
        Token(uint64_t id) : id_(id) {}
        uint64_t id_ = 0;
    };

    /// Get the process singleton.
    static Environment& instance();

    /// Snapshot the current state. Safe from any thread.
    EnvironmentState snapshot() const;

    /// Subscribe to changes. The callback fires after `publish` returns
    /// the new state. The returned token unsubscribes on destruction.
    Token subscribe(Listener listener);

    /// Push a new state. Computes `EnvironmentChange` against the prior
    /// snapshot and dispatches to every listener. Intended for platform
    /// adapter code; tests use `inject_for_test` (which forwards here
    /// after taking the test mutex).
    void publish(const EnvironmentState& next);

    // ── Test hooks ──────────────────────────────────────────────────
    /// Replace the singleton state and notify listeners. Used by unit
    /// tests to simulate platform events without a real OS callback.
    static void inject_for_test(const EnvironmentState& state);

    /// Reset the singleton to defaults and remove every listener.
    /// Tests call this in TEST_CASE setup so prior tests don't leak
    /// listeners or stale state.
    static void reset_for_test();

private:
    Environment() = default;
    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;

    void unsubscribe(uint64_t id);

    mutable std::mutex mu_;
    EnvironmentState   state_;
    struct Entry { uint64_t id; Listener fn; };
    std::vector<Entry> listeners_;
    uint64_t           next_id_ = 1;
};

// ── Per-platform observer bootstraps ─────────────────────────────────
// Hosts call start_environment_observer() once during startup. Each
// platform adapter is idempotent — duplicate calls are a no-op so
// plugin hosts loaded multiple times don't double-register.
//
// Adapters land per platform; this header forward-declares them
// alphabetically. The convenience wrapper at the bottom dispatches to
// the right one based on the build target.

void start_environment_observer_mac();
#if defined(__linux__) && !defined(__ANDROID__)
void start_environment_observer_linux();
#endif
// void start_environment_observer_android(); // follow-up
// void start_environment_observer_ios();     // follow-up
// void start_environment_observer_win();     // follow-up

inline void start_environment_observer() {
#if defined(__APPLE__) && !defined(PULP_IOS)
    start_environment_observer_mac();
#elif defined(__linux__) && !defined(__ANDROID__)
    start_environment_observer_linux();
#endif
    // Other platforms wire their adapters in follow-up PRs; until then
    // their hosts simply see the EnvironmentState defaults.
}

} // namespace pulp::platform
