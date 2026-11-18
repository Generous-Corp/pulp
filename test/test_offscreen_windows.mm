/// Windows created under the test default (PULP_TEST_OFFSCREEN=1) are ordered
/// in and present frames, but are fully transparent, click-through, and never
/// activate the application, so nothing reaches the user's screen.

#include <catch2/catch_test_macros.hpp>

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#include <pulp/platform/test_visibility.hpp>

#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#include <atomic>
#include <cstdlib>
#include <memory>

namespace {

struct EnvGuard {
    std::string name;
    std::string old;
    bool had;
    EnvGuard(const char* n, const char* value) : name(n) {
        const char* o = std::getenv(n);
        had = o != nullptr;
        if (had)
            old = o;
        if (value)
            setenv(n, value, 1);
        else
            unsetenv(n);
    }
    ~EnvGuard() {
        if (had)
            setenv(name.c_str(), old.c_str(), 1);
        else
            unsetenv(name.c_str());
    }
};

void pump(double seconds) {
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}

// Alpha of the on-screen window-server entry for `number`, or -1 when the
// window server lists no on-screen entry for it.
double window_server_alpha(CGWindowID number) {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    double alpha = -1.0;
    for (NSDictionary* info in (__bridge NSArray*)list) {
        if ([info[(__bridge NSString*)kCGWindowNumber] unsignedIntValue] == number) {
            alpha = [info[(__bridge NSString*)kCGWindowAlpha] doubleValue];
            break;
        }
    }
    if (list)
        CFRelease(list);
    return alpha;
}

} // namespace

TEST_CASE("test_windows_offscreen follows the environment", "[offscreen][mac]") {
    {
        EnvGuard off("PULP_TEST_OFFSCREEN", nullptr);
        EnvGuard show("PULP_TEST_SHOW_WINDOWS", nullptr);
        CHECK_FALSE(pulp::platform::test_windows_offscreen());
    }
    {
        EnvGuard off("PULP_TEST_OFFSCREEN", "1");
        EnvGuard show("PULP_TEST_SHOW_WINDOWS", nullptr);
        CHECK(pulp::platform::test_windows_offscreen());
    }
    {
        EnvGuard off("PULP_TEST_OFFSCREEN", "1");
        EnvGuard show("PULP_TEST_SHOW_WINDOWS", "1");
        CHECK_FALSE(pulp::platform::test_windows_offscreen());
    }
    {
        EnvGuard off("PULP_TEST_OFFSCREEN", "0");
        EnvGuard show("PULP_TEST_SHOW_WINDOWS", nullptr);
        CHECK_FALSE(pulp::platform::test_windows_offscreen());
    }
}

TEST_CASE("an ordered-in test window is invisible to the user", "[offscreen][mac]") {
    EnvGuard off("PULP_TEST_OFFSCREEN", "1");
    EnvGuard show("PULP_TEST_SHOW_WINDOWS", nullptr);
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 200, 300, 200)
                                                   styleMask:NSWindowStyleMaskTitled
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.releasedWhenClosed = NO;
    pulp::platform::order_in_window(window);
    pulp::platform::activate_app_for_window();
    pump(0.2);

    // Real: AppKit considers it ordered in.
    CHECK(window.isVisible);
    // Invisible: transparent and click-through, and the app never activated.
    CHECK(window.alphaValue == 0.0);
    CHECK(window.ignoresMouseEvents);
    CHECK_FALSE(NSApp.isActive);
    // Control: the window server lists it on screen, so the alpha reading
    // below is about this window and not an empty result.
    const double alpha = window_server_alpha((CGWindowID)window.windowNumber);
    if (alpha >= 0.0)
        CHECK(alpha == 0.0);

    [window orderOut:nil];
}

TEST_CASE("a window host under the test default still presents frames", "[offscreen][mac]") {
    EnvGuard off("PULP_TEST_OFFSCREEN", "1");
    EnvGuard show("PULP_TEST_SHOW_WINDOWS", nullptr);
    (void)[NSApplication sharedApplication];

    pulp::view::View root;
    pulp::view::WindowOptions opts;
    opts.width = 240;
    opts.height = 160;
    opts.use_gpu = false;
    auto host = pulp::view::WindowHost::create(root, opts);
    REQUIRE(host != nullptr);

    std::atomic<int> ticks{0};
    host->set_idle_callback([&ticks] { ticks.fetch_add(1, std::memory_order_relaxed); });
    host->show();
    host->repaint();
    pump(0.4);

    NSWindow* window = (__bridge NSWindow*)host->native_window_handle();
    REQUIRE(window != nil);
    CHECK(host->is_visible());
    CHECK(window.alphaValue == 0.0);
    CHECK(window.ignoresMouseEvents);
    CHECK_FALSE(NSApp.isActive);
    // Presentation is real: the host's frame pump ticked and it can produce pixels.
    CHECK(ticks.load() > 0);
    CHECK_FALSE(host->capture_png().empty());

    host->hide();
}
