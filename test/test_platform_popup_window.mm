// Popup windows the client draws into: placement against the screen, open
// and close without leaking windows, every dismissal path reporting once, and
// the dismissing outside click being consumed before it reaches the editor.

#include <pulp/platform/main_thread.hpp>
#include <pulp/platform/objc_unique_class.hpp>
#include <pulp/platform/popup_window.hpp>

#include <catch2/catch_test_macros.hpp>

#import <Cocoa/Cocoa.h>
#include <objc/runtime.h>

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

using namespace pulp::platform;
using namespace std::chrono_literals;

namespace {

int g_probe_mouse_downs = 0;

void probe_mouse_down(id, SEL, NSEvent*) {
    ++g_probe_mouse_downs;
}

BOOL probe_accepts_first_mouse(id, SEL, NSEvent*) {
    return YES; // an inactive window's first click reaches the view
}

void add_probe_methods(void* cls, void*) {
    class_addMethod(static_cast<Class>(cls), @selector(mouseDown:),
                    reinterpret_cast<IMP>(&probe_mouse_down), "v@:@");
    class_addMethod(static_cast<Class>(cls), @selector(acceptsFirstMouse:),
                    reinterpret_cast<IMP>(&probe_accepts_first_mouse), "c@:@");
}

// Dispatch queued events the way the application's event loop would.
void dispatch_events(std::chrono::milliseconds span) {
    const auto deadline = std::chrono::steady_clock::now() + span;
    while (std::chrono::steady_clock::now() < deadline) {
        NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
        if (event != nil)
            [NSApp sendEvent:event];
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true);
    }
}

template <typename Pred> bool pump_until(Pred done, std::chrono::milliseconds limit = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        dispatch_events(10ms);
    }
    return true;
}

// The editor: a window with a probe content view that counts mouse-downs.
struct Editor {
    NSWindow* window = nil;
    NSView* view = nil;
    Editor() {
        [NSApplication sharedApplication];
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(300, 300, 400, 300)
                                             styleMask:NSWindowStyleMaskTitled
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
        window.releasedWhenClosed = NO;
        Class probe = static_cast<Class>(pulp::platform::mac::unique_objc_class(
            "PulpTestPopupProbeView", (__bridge void*)[NSView class], &add_probe_methods));
        view = [[probe alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
        window.contentView = view;
        [window orderFrontRegardless];
    }
    ~Editor() {
        [window orderOut:nil];
        [view release];
        [window release];
    }
    void post_click(NSPoint in_window) const {
        NSEvent* down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                           location:in_window
                                      modifierFlags:0
                                          timestamp:NSProcessInfo.processInfo.systemUptime
                                       windowNumber:window.windowNumber
                                            context:nil
                                        eventNumber:0
                                         clickCount:1
                                           pressure:1];
        [NSApp postEvent:down atStart:NO];
    }
};

PopupWindowOptions options_for(const Editor& editor) {
    PopupWindowOptions options;
    options.view = (__bridge void*)editor.view;
    options.anchor = {20, 40, 120, 22};
    options.width = 100; // narrower than the anchor: widened to it
    options.height = 150;
    return options;
}

} // namespace

TEST_CASE("popups are placed below, flipped above, and kept on the screen",
          "[platform][popup-window][placement]") {
    const PopupRect visible{0, 25, 1000, 775}; // top-left based, menu bar above
    const PopupRect anchor{100, 200, 80, 20};

    auto below = place_popup(anchor, 200, 300, visible, PopupPlacement::auto_flip);
    REQUIRE(below.x == 100);
    REQUIRE(below.y == 220);
    REQUIRE(below.width == 200);
    REQUIRE(below.height == 300);

    const PopupRect low{100, 700, 80, 20}; // no room below
    auto flipped = place_popup(low, 200, 300, visible, PopupPlacement::auto_flip);
    REQUIRE(flipped.y == 400);

    auto forced_below = place_popup(low, 200, 300, visible, PopupPlacement::below);
    REQUIRE(forced_below.y == 800 - 300); // clamped back on the screen

    const PopupRect right{950, 200, 40, 20};
    auto clamped = place_popup(right, 200, 100, visible, PopupPlacement::auto_flip);
    REQUIRE(clamped.x == 800);

    auto widened = place_popup(anchor, 30, 100, visible, PopupPlacement::below);
    REQUIRE(widened.width == 80);

    const PopupRect squeezed{100, 300, 80, 20}; // neither side fits 700
    auto bigger_side = place_popup(squeezed, 80, 700, visible, PopupPlacement::auto_flip);
    REQUIRE(bigger_side.y >= visible.y);
    REQUIRE(bigger_side.y + bigger_side.height <= visible.y + visible.height);
}

TEST_CASE("a popup opens as a child of the editor's window, sized and placed",
          "[platform][popup-window][native]") {
    Editor editor;
    MainThreadScope scope;
    auto popup = PopupWindow::open(scope, options_for(editor), {});
    REQUIRE(popup != nullptr);
    REQUIRE(popup->is_open());
    NSWindow* panel = (__bridge NSWindow*)popup->native_window();
    REQUIRE([editor.window.childWindows containsObject:panel]);
    REQUIRE(panel.level >= editor.window.level);
    const PopupRect frame = popup->screen_frame();
    REQUIRE(frame.width == 120); // the anchor's width
    REQUIRE(frame.height == 150);
    NSView* content = (__bridge NSView*)popup->content_view();
    REQUIRE(content.wantsLayer);
    REQUIRE(content.window == panel);
    REQUIRE(popup->backing_scale() == static_cast<float>(panel.backingScaleFactor));

    // The anchor's bottom-left corner is the popup's top-left corner.
    const NSRect anchor_in_window =
        [editor.view convertRect:NSMakeRect(20, editor.view.bounds.size.height - 40 - 22, 120, 22)
                          toView:nil];
    const NSRect anchor_on_screen = [editor.window convertRectToScreen:anchor_in_window];
    REQUIRE(std::abs(NSMaxY(panel.frame) - NSMinY(anchor_on_screen)) < 0.5);
    REQUIRE(std::abs(NSMinX(panel.frame) - NSMinX(anchor_on_screen)) < 0.5);

    popup->set_size(300, 80);
    REQUIRE(popup->screen_frame().width == 300);
    REQUIRE(popup->screen_frame().height == 80);
}

TEST_CASE("opening and closing popups leaves no windows behind",
          "[platform][popup-window][native]") {
    Editor editor;
    MainThreadScope scope;
    const NSUInteger windows_before = NSApp.windows.count;
    for (int i = 0; i < 50; ++i) {
        @autoreleasepool {
            auto popup = PopupWindow::open(scope, options_for(editor), {});
            REQUIRE(popup != nullptr);
            if (i % 2 == 0)
                popup->close();
        }
    }
    dispatch_events(20ms);
    REQUIRE(editor.window.childWindows.count == 0);
    REQUIRE(NSApp.windows.count == windows_before);
}

TEST_CASE("each dismissal path reports its reason once", "[platform][popup-window][native]") {
    Editor editor;
    MainThreadScope scope;
    struct Case {
        const char* name;
        std::function<void(PopupWindow&, Editor&)> trigger;
        PopupDismissReason expected;
    };
    const std::vector<Case> cases = {
        {"client", [](PopupWindow& p, Editor&) { p.close(); }, PopupDismissReason::client},
        {"resign key",
         [](PopupWindow& p, Editor&) {
             [[NSNotificationCenter defaultCenter]
                 postNotificationName:NSWindowDidResignKeyNotification
                               object:(__bridge NSWindow*)p.native_window()];
         },
         PopupDismissReason::focus_lost},
        {"app deactivates",
         [](PopupWindow&, Editor&) {
             [[NSNotificationCenter defaultCenter]
                 postNotificationName:NSApplicationDidResignActiveNotification
                               object:NSApp];
         },
         PopupDismissReason::focus_lost},
        {"parent closing",
         [](PopupWindow&, Editor& e) {
             [[NSNotificationCenter defaultCenter]
                 postNotificationName:NSWindowWillCloseNotification
                               object:e.window];
         },
         PopupDismissReason::parent_closed},
        {"escape",
         [](PopupWindow& p, Editor&) {
             NSWindow* panel = (__bridge NSWindow*)p.native_window();
             NSEvent* key = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                             location:NSZeroPoint
                                        modifierFlags:0
                                            timestamp:NSProcessInfo.processInfo.systemUptime
                                         windowNumber:panel.windowNumber
                                              context:nil
                                           characters:@"\033"
                          charactersIgnoringModifiers:@"\033"
                                            isARepeat:NO
                                              keyCode:53];
             [NSApp postEvent:key atStart:NO];
         },
         PopupDismissReason::escape},
    };
    for (const auto& c : cases) {
        INFO(c.name);
        std::vector<PopupDismissReason> reports;
        auto popup = PopupWindow::open(scope, options_for(editor),
                                       [&](PopupDismissReason r) { reports.push_back(r); });
        REQUIRE(popup != nullptr);
        c.trigger(*popup, editor);
        REQUIRE(pump_until([&] { return !reports.empty(); }));
        popup->close(); // already closed: no second report
        dispatch_events(20ms);
        REQUIRE(reports.size() == 1);
        REQUIRE(reports.front() == c.expected);
        REQUIRE_FALSE(popup->is_open());
    }
}

TEST_CASE("destroying the popup or its scope closes it without a report",
          "[platform][popup-window][native]") {
    Editor editor;
    int reports = 0;
    auto scope = std::make_unique<MainThreadScope>();
    auto popup =
        PopupWindow::open(*scope, options_for(editor), [&](PopupDismissReason) { ++reports; });
    REQUIRE(popup != nullptr);
    scope.reset();
    REQUIRE_FALSE(popup->is_open());
    REQUIRE(editor.window.childWindows.count == 0);
    popup.reset();

    MainThreadScope live;
    auto second =
        PopupWindow::open(live, options_for(editor), [&](PopupDismissReason) { ++reports; });
    second.reset();
    dispatch_events(30ms);
    REQUIRE(reports == 0);
    REQUIRE(editor.window.childWindows.count == 0);
}

TEST_CASE("the outside click that closes a popup does not reach the editor",
          "[platform][popup-window][native][outside-click]") {
    Editor editor;
    MainThreadScope scope;

    // Control: with no popup open, the same synthetic click reaches the view.
    g_probe_mouse_downs = 0;
    editor.post_click(NSMakePoint(200, 150));
    REQUIRE(pump_until([&] { return g_probe_mouse_downs == 1; }));
    // The control click starts activating the application, and the window
    // server finishes that asynchronously: on a slow or headless host the key
    // window then moves back to the editor and would close a popup that was
    // opened in between. Let the activation settle before opening the popup.
    pump_until([] { return NSApp.isActive; }, 1000ms);
    dispatch_events(150ms);

    std::optional<PopupDismissReason> reason;
    auto popup =
        PopupWindow::open(scope, options_for(editor), [&](PopupDismissReason r) { reason = r; });
    REQUIRE(popup != nullptr);
    dispatch_events(150ms);
    INFO("popup closed before the click with reason "
         << (reason ? pulp::platform::to_string(*reason) : "none"));
    REQUIRE(popup->is_open());
    g_probe_mouse_downs = 0;
    editor.post_click(NSMakePoint(200, 150));
    REQUIRE(pump_until([&] { return reason.has_value(); }));
    dispatch_events(30ms);
    REQUIRE(*reason == PopupDismissReason::outside_click);
    REQUIRE(g_probe_mouse_downs == 0);
}

TEST_CASE("a popup needs a view in a window", "[platform][popup-window]") {
    [NSApplication sharedApplication];
    MainThreadScope scope;
    NSView* orphan = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 10, 10)];
    PopupWindowOptions options;
    options.view = (__bridge void*)orphan;
    REQUIRE(PopupWindow::open(scope, options, {}) == nullptr);
    options.view = nullptr;
    REQUIRE(PopupWindow::open(scope, options, {}) == nullptr);
    [orphan release];
}
