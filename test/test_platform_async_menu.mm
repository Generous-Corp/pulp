// Anchored menus: placement, the NSMenu built from a model, selection
// reporting (synchronous PopupMenu and show_menu_async), and that tracking a
// real menu keeps MainThreadScope timers running and ends cleanly when the
// scope dies.

#include <pulp/platform/main_thread.hpp>
#include <pulp/platform/popup_menu.hpp>

#include <catch2/catch_test_macros.hpp>

#import <Cocoa/Cocoa.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

using namespace pulp::platform;
using namespace std::chrono_literals;

namespace {

template <typename Pred> bool pump_until(Pred done, std::chrono::milliseconds limit = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    }
    return true;
}

void pump_for(std::chrono::milliseconds span) {
    const auto deadline = std::chrono::steady_clock::now() + span;
    while (std::chrono::steady_clock::now() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
}

struct OffscreenWindow {
    NSWindow* window = nil;
    NSView* view = nil;
    OffscreenWindow() {
        [NSApplication sharedApplication];
        // Out of App Nap, as a host's foreground editor is, so timer counts
        // reflect tracking rather than background throttling.
        static id activity = [[[NSProcessInfo processInfo]
            beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                              reason:@"menu tracking timing"] retain];
        (void)activity;
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 200, 480, 320)
                                             styleMask:NSWindowStyleMaskTitled
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
        window.releasedWhenClosed = NO;
        view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 480, 320)];
        window.contentView = view;
        [window orderFrontRegardless];
    }
    ~OffscreenWindow() {
        [window orderOut:nil];
        [view release];
        [window release];
    }
};

// Runs `action` with the menu once tracking begins, from inside the tracking
// loop (the notification is posted on the main thread as tracking starts).
struct OnTracking {
    id token = nil;
    explicit OnTracking(std::function<void(NSMenu*)> action) {
        auto box = std::make_shared<std::function<void(NSMenu*)>>(std::move(action));
        token = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSMenuDidBeginTrackingNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification* note) {
                      NSMenu* menu = note.object;
                      if (menu.supermenu == nil)
                          (*box)(menu);
                    }];
        [token retain];
    }
    ~OnTracking() {
        [[NSNotificationCenter defaultCenter] removeObserver:token];
        [token release];
    }
};

MenuModel sample_model() {
    MenuModel model;
    MenuItem a;
    a.id = 10;
    a.label = "Init patch";
    MenuItem b;
    b.id = 11;
    b.label = "Copy";
    b.checked = true;
    b.text_colour = MenuColour{1.0f, 0.25f, 0.0f, 1.0f};
    MenuItem c;
    c.id = 12;
    c.label = "Disabled";
    c.enabled = false;
    MenuItem deep;
    deep.id = 31;
    deep.label = "Deep";
    MenuItem inner;
    inner.label = "Inner";
    inner.submenu = {deep};
    MenuItem sub;
    sub.label = "Bank";
    sub.submenu = {MenuItem{20, "A"}, inner};
    model.items = {a, b, MenuItem::make_separator(), c, sub};
    return model;
}

} // namespace

TEST_CASE("menu placement converts a top-left anchor to view coordinates", "[platform][menu]") {
    const MenuAnchor anchor{nullptr, 20, 30, 100, 18};
    auto below = menu_location_in_view(anchor, MenuPlacement::below_anchor, false, 200);
    REQUIRE(below.x == 20);
    REQUIRE(below.y == 200 - 48);
    auto over = menu_location_in_view(anchor, MenuPlacement::over_selected, false, 200);
    REQUIRE(over.y == 200 - 30);
    auto flipped = menu_location_in_view(anchor, MenuPlacement::below_anchor, true, 200);
    REQUIRE(flipped.y == 48);
    auto flipped_over = menu_location_in_view(anchor, MenuPlacement::over_selected, true, 200);
    REQUIRE(flipped_over.y == 30);
}

TEST_CASE("the native menu mirrors the model", "[platform][menu][builder]") {
    @autoreleasepool {
        [NSApplication sharedApplication];
        auto* menu = static_cast<NSMenu*>(build_native_menu(sample_model(), 140));
        REQUIRE(menu.numberOfItems == 5);
        REQUIRE(menu.minimumWidth == 140);
        REQUIRE_FALSE(menu.autoenablesItems);
        NSMenuItem* a = [menu itemAtIndex:0];
        REQUIRE(a.tag == 10);
        REQUIRE([a.title isEqualToString:@"Init patch"]);
        REQUIRE(a.state == NSControlStateValueOff);
        REQUIRE(a.attributedTitle == nil);
        NSMenuItem* b = [menu itemAtIndex:1];
        REQUIRE(b.state == NSControlStateValueOn);
        REQUIRE(b.attributedTitle != nil);
        NSColor* colour = [b.attributedTitle attribute:NSForegroundColorAttributeName
                                               atIndex:0
                                        effectiveRange:nullptr];
        REQUIRE(colour != nil);
        REQUIRE(std::abs([colour colorUsingColorSpace:NSColorSpace.sRGBColorSpace].redComponent -
                         1.0) < 0.01);
        REQUIRE([menu itemAtIndex:2].isSeparatorItem);
        REQUIRE_FALSE([menu itemAtIndex:3].enabled);
        NSMenuItem* bank = [menu itemAtIndex:4];
        REQUIRE(bank.submenu != nil);
        REQUIRE(bank.submenu.numberOfItems == 2);
        REQUIRE([bank.submenu itemAtIndex:1].submenu.numberOfItems == 1);
        REQUIRE([[bank.submenu itemAtIndex:1].submenu itemAtIndex:0].tag == 31);
    }
}

TEST_CASE("choosing a row, at any depth, is recorded on the root menu",
          "[platform][menu][builder]") {
    [NSApplication sharedApplication]; // menu actions are sent through NSApp
    @autoreleasepool {
        auto* menu = static_cast<NSMenu*>(build_native_menu(sample_model(), 0));
        REQUIRE_FALSE(native_menu_chosen_id((__bridge void*)menu).has_value());
        [menu performActionForItemAtIndex:1];
        REQUIRE(native_menu_chosen_id((__bridge void*)menu) == 11);
        NSMenu* inner = [[menu itemAtIndex:4].submenu itemAtIndex:1].submenu;
        [inner performActionForItemAtIndex:0];
        REQUIRE(native_menu_chosen_id((__bridge void*)menu) == 31);
    }
}

TEST_CASE("an async menu's choice arrives once, after the call returns",
          "[platform][menu][presenter]") {
    std::function<void(std::optional<int>)> respond;
    int cancels = 0;
    set_menu_presenter({[&](const MenuAnchor&, const MenuModel&, MenuPlacement,
                            std::function<void(std::optional<int>)> r) { respond = std::move(r); },
                        [&] { ++cancels; }});
    MainThreadScope scope;
    int calls = 0;
    std::optional<int> got;
    show_menu_async(scope, {}, sample_model(), MenuPlacement::below_anchor,
                    [&](std::optional<int> id) {
                        ++calls;
                        got = id;
                    });
    respond(12);
    REQUIRE(calls == 0);
    respond(10); // ignored
    REQUIRE(pump_until([&] { return calls > 0; }));
    pump_for(20ms);
    REQUIRE(calls == 1);
    REQUIRE(got == 12);

    int dropped_calls = 0;
    {
        MainThreadScope doomed;
        show_menu_async(doomed, {}, sample_model(), MenuPlacement::below_anchor,
                        [&](std::optional<int>) { ++dropped_calls; });
    }
    pump_for(30ms);
    set_menu_presenter({});
    REQUIRE(cancels == 1);
    REQUIRE(dropped_calls == 0);
}

TEST_CASE("an async menu with no window reports nothing chosen", "[platform][menu]") {
    [NSApplication sharedApplication];
    MainThreadScope scope;
    NSView* orphan = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 10, 10)];
    bool called = false;
    std::optional<int> got = 99;
    show_menu_async(scope, {(__bridge void*)orphan, 0, 0, 10, 10}, sample_model(),
                    MenuPlacement::below_anchor, [&](std::optional<int> id) {
                        called = true;
                        got = id;
                    });
    REQUIRE_FALSE(called);
    REQUIRE(pump_until([&] { return called; }));
    REQUIRE_FALSE(got.has_value());
    [orphan release];
}

TEST_CASE("the synchronous PopupMenu reports the chosen item", "[platform][menu][native][sync]") {
    OffscreenWindow host;
    MainThreadScope scope;
    NSMenu* open_menu = nil;
    MainThreadTimer closer(scope, [&] { [open_menu cancelTracking]; });
    bool tracked = false;
    // Choose as tracking begins, then close on the next pass of the tracking
    // loop. Tracking can also end at any moment from outside (a menu another
    // process opens ends this one's tracking), so the choice is made with no
    // delay for that to race against.
    OnTracking on_tracking([&](NSMenu* menu) {
        tracked = true;
        open_menu = menu;
        [menu performActionForItemAtIndex:1];
        closer.fire_after(0ms);
    });
    PopupMenu menu;
    menu.add_item(1, "One");
    menu.add_item(2, "Two");
    const auto chosen = menu.show_at_view((__bridge void*)host.view);
    closer.cancel();
    REQUIRE(tracked);
    REQUIRE(chosen == 2);
}

TEST_CASE("an anchored native menu tracks, keeps scope timers running, and reports the choice",
          "[platform][menu][native]") {
    OffscreenWindow host;
    MainThreadScope scope;
    int ticks_while_tracking = 0;
    bool tracking = false;
    MainThreadTimer ticker(scope, [&] {
        if (tracking)
            ++ticks_while_tracking;
    });
    NSMenu* tracked = nil;
    MainThreadTimer chooser(scope, [&] {
        [tracked performActionForItemAtIndex:1]; // "Copy"
        [tracked cancelTracking];
    });
    float tracked_min_width = 0;
    OnTracking on_tracking([&](NSMenu* menu) {
        // Retained: if tracking ends before the chooser fires, the chooser
        // still runs against a live menu and the test fails on the choice
        // instead of crashing.
        tracked = [menu retain];
        tracked_min_width = static_cast<float>(menu.minimumWidth);
        tracking = true;
        chooser.fire_after(150ms);
    });
    ticker.fire_every(10ms);
    std::optional<std::optional<int>> got;
    show_menu_async(scope, {(__bridge void*)host.view, 40, 40, 160, 20}, sample_model(),
                    MenuPlacement::below_anchor, [&](std::optional<int> id) { got = id; });
    REQUIRE(pump_until([&] { return got.has_value(); }));
    tracking = false;
    ticker.cancel();
    chooser.cancel();
    REQUIRE(tracked != nil);
    [tracked release];
    REQUIRE(tracked_min_width >= 160);
    REQUIRE(*got == 11);
    REQUIRE(ticks_while_tracking >= 2); // repeated firing, not a full rate
}

TEST_CASE("destroying the scope while a menu tracks closes it with no completion",
          "[platform][menu][native]") {
    OffscreenWindow host;
    auto scope = std::make_unique<MainThreadScope>();
    MainThreadScope killer_scope;
    int calls = 0;
    bool tracked = false;
    MainThreadTimer killer(killer_scope, [&] { scope.reset(); });
    OnTracking on_tracking([&](NSMenu*) {
        tracked = true;
        killer.fire_after(50ms);
    });
    show_menu_async(*scope, {(__bridge void*)host.view, 10, 10, 80, 20}, sample_model(),
                    MenuPlacement::below_anchor, [&](std::optional<int>) { ++calls; });
    REQUIRE(pump_until([&] { return scope == nullptr; }));
    pump_for(50ms);
    REQUIRE(tracked);
    REQUIRE(calls == 0);
}
