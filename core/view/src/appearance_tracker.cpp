#include <pulp/view/appearance_tracker.hpp>

#if __APPLE__
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#define PULP_HAS_NSAPPEARANCE 1
#endif
#endif

#if PULP_HAS_NSAPPEARANCE
// Forward declaration — implemented in platform/mac/appearance_mac.mm
namespace pulp::view::platform {
    Appearance detect_mac_appearance();
}
#elif _WIN32
// Forward declaration — implemented in platform/win/appearance_win.cpp
namespace pulp::view::platform {
    Appearance detect_win_appearance();
}
#endif

namespace pulp::view {

// ── AppearanceTracker ───────────────────────────────────────────────────────

AppearanceTracker::AppearanceTracker() {
    last_known_ = detect_system_appearance();
}

AppearanceTracker::~AppearanceTracker() = default;

Appearance AppearanceTracker::current_appearance() const {
    if (locked_) return locked_appearance_;
    return last_known_;
}

bool AppearanceTracker::poll() {
    if (locked_) return false;

    auto current = detect_system_appearance();
    if (current != last_known_) {
        last_known_ = current;
        if (callback_) callback_(current);
        return true;
    }
    return false;
}

void AppearanceTracker::on_appearance_changed(std::function<void(Appearance)> callback) {
    callback_ = std::move(callback);
}

void AppearanceTracker::lock(Appearance appearance) {
    locked_ = true;
    locked_appearance_ = appearance;
    if (callback_) callback_(appearance);
}

void AppearanceTracker::unlock() {
    locked_ = false;
    auto current = detect_system_appearance();
    if (current != last_known_) {
        last_known_ = current;
        if (callback_) callback_(current);
    }
}

Appearance AppearanceTracker::detect_system_appearance() {
#if PULP_HAS_NSAPPEARANCE
    return platform::detect_mac_appearance();
#elif _WIN32
    return platform::detect_win_appearance();
#else
    return Appearance::dark;  // Default on unsupported platforms
#endif
}

// ── ThemeManager ────────────────────────────────────────────────────────────

ThemeManager::ThemeManager() {
    light_theme_ = Theme::light();
    dark_theme_ = Theme::dark();
}

void ThemeManager::set_theme_pair(Theme light_theme, Theme dark_theme) {
    light_theme_ = std::move(light_theme);
    dark_theme_ = std::move(dark_theme);
}

const Theme& ThemeManager::active_theme() const {
    if (theme_locked_) return locked_theme_;

    auto appearance = tracker_.current_appearance();
    return (appearance == Appearance::light) ? light_theme_ : dark_theme_;
}

void ThemeManager::lock_theme(Theme theme) {
    theme_locked_ = true;
    locked_theme_ = std::move(theme);
    if (theme_callback_) theme_callback_(locked_theme_);
}

void ThemeManager::lock_appearance(Appearance appearance) {
    theme_locked_ = false;
    tracker_.lock(appearance);
    if (theme_callback_) theme_callback_(active_theme());
}

void ThemeManager::unlock() {
    theme_locked_ = false;
    tracker_.unlock();
    if (theme_callback_) theme_callback_(active_theme());
}

void ThemeManager::set_mode(ThemeMode m) {
    switch (m) {
        case ThemeMode::system: unlock(); break;
        case ThemeMode::light:  lock_appearance(Appearance::light); break;
        case ThemeMode::dark:   lock_appearance(Appearance::dark); break;
    }
}

ThemeMode ThemeManager::mode() const {
    if (!tracker_.is_locked()) return ThemeMode::system;
    return tracker_.locked_appearance() == Appearance::light ? ThemeMode::light
                                                             : ThemeMode::dark;
}

bool ThemeManager::poll() {
    if (theme_locked_) return false;

    bool changed = tracker_.poll();
    if (changed && theme_callback_) {
        theme_callback_(active_theme());
    }
    return changed;
}

void ThemeManager::on_theme_changed(std::function<void(const Theme&)> callback) {
    theme_callback_ = std::move(callback);
}

} // namespace pulp::view
