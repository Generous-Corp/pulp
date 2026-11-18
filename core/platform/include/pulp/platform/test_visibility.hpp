#pragma once

/// @file test_visibility.hpp
/// Whether windows created by this process may appear on the user's screen.
///
/// A process that runs as a test or self-check sets `PULP_TEST_OFFSCREEN=1`
/// (ctest does this for every registered test). Window hosts and test
/// fixtures then present into a window that is ordered in but fully
/// transparent, click-through, and never activates the application: the
/// compositor, occlusion tracking and drawable acquisition behave as for a
/// real window, and nothing reaches the screen. A developer running an app
/// by hand does not set the variable and sees ordinary windows.
///
/// `PULP_TEST_SHOW_WINDOWS=1` overrides it for tests that need a visible
/// window (for example a real-host smoke run watched by a person).

#include <cstdlib>
#include <cstring>

namespace pulp::platform {

namespace detail {
inline bool test_env_truthy(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v)
        return false;
    return std::strcmp(v, "0") != 0 && std::strcmp(v, "false") != 0 && std::strcmp(v, "off") != 0 &&
           std::strcmp(v, "no") != 0;
}
} // namespace detail

/// True when windows must not be shown on screen.
inline bool test_windows_offscreen() {
    return detail::test_env_truthy("PULP_TEST_OFFSCREEN") &&
           !detail::test_env_truthy("PULP_TEST_SHOW_WINDOWS");
}

} // namespace pulp::platform

#if defined(__APPLE__) && defined(__OBJC__)
#import <AppKit/AppKit.h>

namespace pulp::platform {

/// Under `test_windows_offscreen()`, makes `window` transparent and
/// click-through so that ordering it in never shows anything.
inline void make_window_invisible_for_test(NSWindow* window) {
    if (!window || !test_windows_offscreen())
        return;
    window.alphaValue = 0.0;
    window.ignoresMouseEvents = YES;
    window.hidesOnDeactivate = NO;
}

/// Orders `window` in. Under `test_windows_offscreen()` the window is made
/// transparent and click-through and ordered behind everything (key within
/// the application when `make_key`, but the application is never activated), so it is "visible" to AppKit but never to the user.
/// Otherwise it is shown normally (key + front) when `make_key` is true,
/// or merely ordered front when false.
inline void order_in_window(NSWindow* window, bool make_key = true) {
    if (!window)
        return;
    if (test_windows_offscreen()) {
        make_window_invisible_for_test(window);
        [window orderBack:nil];
        if (make_key)
            [window makeKeyWindow];
        return;
    }
    if (make_key)
        [window makeKeyAndOrderFront:nil];
    else
        [window orderFrontRegardless];
}

/// Dock/menu-bar role for a foreground app: Accessory (no Dock icon, no menu
/// bar, never activated) under offscreen test runs, Regular otherwise.
inline NSApplicationActivationPolicy foreground_activation_policy() {
    return test_windows_offscreen() ? NSApplicationActivationPolicyAccessory
                                    : NSApplicationActivationPolicyRegular;
}

/// Activates the application unless windows are offscreen.
inline void activate_app_for_window() {
    if (test_windows_offscreen())
        return;
    [NSApp activateIgnoringOtherApps:YES];
}

} // namespace pulp::platform
#endif
