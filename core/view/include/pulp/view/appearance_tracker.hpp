#pragma once

#include <pulp/view/theme.hpp>
#include <functional>
#include <string>
#include <memory>

namespace pulp::view {

/// System appearance mode
enum class Appearance { light, dark };

/// User-facing theme preference. `system` follows the OS appearance live;
/// `light`/`dark` pin it regardless of the OS. A developer who doesn't want to
/// offer the choice simply leaves the manager in one mode and never exposes a
/// control — there is no requirement to support all three.
enum class ThemeMode { system, light, dark };

/// Tracks OS-level appearance (light/dark mode) and auto-switches themes.
///
/// On macOS, observes NSAppearance changes via NSApp effective appearance.
/// On Windows, monitors the AppsUseLightTheme registry key.
/// On Linux/other platforms, defaults to dark and does not auto-detect.
class AppearanceTracker {
public:
    AppearanceTracker();
    ~AppearanceTracker();

    /// Get current system appearance.
    Appearance current_appearance() const;

    /// Poll for appearance changes. Call this periodically (e.g., once per frame).
    /// Returns true if the appearance changed since last poll.
    bool poll();

    /// Register a callback for appearance changes.
    /// The callback receives the new appearance.
    void on_appearance_changed(std::function<void(Appearance)> callback);

    /// Lock to a specific appearance, ignoring OS changes.
    void lock(Appearance appearance);

    /// Unlock and resume tracking OS appearance.
    void unlock();

    /// Check if appearance is locked.
    bool is_locked() const { return locked_; }

    /// Get the locked appearance (only valid when is_locked() is true).
    Appearance locked_appearance() const { return locked_appearance_; }

private:
    Appearance last_known_ = Appearance::dark;
    bool locked_ = false;
    Appearance locked_appearance_ = Appearance::dark;
    std::function<void(Appearance)> callback_;

    /// Platform-specific detection
    static Appearance detect_system_appearance();
};

/// Manages the connection between AppearanceTracker and Theme switching.
/// Holds a light theme, dark theme, and optional locked theme.
class ThemeManager {
public:
    ThemeManager();

    /// Set the theme pair for automatic appearance-based switching.
    void set_theme_pair(Theme light_theme, Theme dark_theme);

    /// Get the active theme based on current appearance (or locked theme).
    const Theme& active_theme() const;

    /// Lock to a specific theme, ignoring appearance tracking.
    void lock_theme(Theme theme);

    /// Lock to an appearance (light/dark) but still use the theme pair.
    void lock_appearance(Appearance appearance);

    /// Unlock and resume automatic theme switching.
    void unlock();

    /// User-facing theme preference. `set_mode(system)` resumes OS tracking;
    /// `light`/`dark` pin the corresponding theme from the pair. This is the
    /// recommended surface — it maps onto lock_appearance()/unlock() so the
    /// `system`/`light`/`dark` choice round-trips cleanly via `mode()`.
    void set_mode(ThemeMode m);
    ThemeMode mode() const;

    /// Check if a specific theme is locked.
    bool is_locked() const { return theme_locked_ || tracker_.is_locked(); }

    /// Access the underlying tracker for direct appearance queries.
    AppearanceTracker& tracker() { return tracker_; }
    const AppearanceTracker& tracker() const { return tracker_; }

    /// Poll for changes. Returns true if the active theme changed.
    bool poll();

    /// Register a callback for when the active theme changes.
    void on_theme_changed(std::function<void(const Theme&)> callback);

private:
    AppearanceTracker tracker_;
    Theme light_theme_;
    Theme dark_theme_;
    Theme locked_theme_;
    bool theme_locked_ = false;
    std::function<void(const Theme&)> theme_callback_;
};

} // namespace pulp::view
