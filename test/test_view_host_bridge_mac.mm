#include <TargetConditionals.h>

#if TARGET_OS_OSX

#import <Cocoa/Cocoa.h>

#include <catch2/catch_test_macros.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/window_host.hpp>

#include <chrono>
#include <thread>
#include <vector>

using namespace pulp::view;

TEST_CASE("macOS WindowHost reports content size and fires resize callbacks",
          "[view][hosts][issue-661]") {
    @autoreleasepool {
        [NSApplication sharedApplication];

        View root;
        WindowOptions opts;
        opts.title = "WindowHost issue-661";
        opts.width = 320.0f;
        opts.height = 240.0f;
        opts.resizable = true;
        opts.use_gpu = false;

        auto host = WindowHost::create(root, opts);
        REQUIRE(host != nullptr);
        REQUIRE(root.window_host() == host.get());
        REQUIRE(host->native_window_handle() != nullptr);
        REQUIRE(host->native_content_view_handle() != nullptr);

        const auto initial = host->get_content_size();
        REQUIRE(initial.width == 320);
        REQUIRE(initial.height == 240);

        std::vector<WindowHost::ContentSize> seen_sizes;
        host->set_resize_callback([&](uint32_t width, uint32_t height) {
            seen_sizes.push_back({width, height});
        });

        host->show();
        auto* nswindow = (__bridge NSWindow*)host->native_window_handle();
        REQUIRE(nswindow != nil);
        [nswindow setContentSize:NSMakeSize(360.0, 280.0)];

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (seen_sizes.empty() && std::chrono::steady_clock::now() < deadline) {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        REQUIRE_FALSE(seen_sizes.empty());
        REQUIRE(seen_sizes.back().width == 360);
        REQUIRE(seen_sizes.back().height == 280);

        const auto resized = host->get_content_size();
        REQUIRE(resized.width == 360);
        REQUIRE(resized.height == 280);

        host->hide();
    }
}

// pulp #992 — `-[PulpView mouseUp:]` SIGSEGV regression test.
//
// Repro: a click on a child View triggers a state change (in real apps,
// React unmounts the clicked widget on its own click handler), so by the
// time `mouseUp` fires the captured `_dragTarget` is a freed pointer.
// Without the fix in `view_is_in_tree`, the next deref crashes the app.
//
// We stage this directly on the macOS host: post a real `NSLeftMouseDown`
// to the PulpView's window, remove the child during the down handler,
// then post `NSLeftMouseUp` and verify the up event drops cleanly with
// no exception. The `view_is_in_tree` check must catch the unmount
// before any field of `_dragTarget` is touched.
TEST_CASE("PulpView mouseUp survives mouseDown handler that unmounts the target",
          "[view][hosts][issue-992]") {
    @autoreleasepool {
        [NSApplication sharedApplication];

        View root;
        WindowOptions opts;
        opts.title = "PulpView issue-992";
        opts.width = 200.0f;
        opts.height = 100.0f;
        opts.resizable = false;
        opts.use_gpu = false;

        auto host = WindowHost::create(root, opts);
        REQUIRE(host != nullptr);

        auto child = std::make_unique<View>();
        child->set_bounds({0, 0, 200, 100});
        auto* child_ptr = child.get();
        root.add_child(std::move(child));

        host->show();
        auto* nswindow = (__bridge NSWindow*)host->native_window_handle();
        REQUIRE(nswindow != nil);
        auto* contentView = nswindow.contentView;
        REQUIRE(contentView != nil);

        // Pump the run loop so the window finishes coming up.
        for (int i = 0; i < 20; ++i) {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
        }

        const NSPoint local = NSMakePoint(40.0, 50.0);   // arbitrary, inside child
        const NSPoint window_pt = [contentView convertPoint:local toView:nil];

        NSEvent* down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                           location:window_pt
                                      modifierFlags:0
                                          timestamp:0
                                       windowNumber:nswindow.windowNumber
                                            context:nil
                                        eventNumber:0
                                         clickCount:1
                                           pressure:1.0];
        NSEvent* up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                         location:window_pt
                                    modifierFlags:0
                                        timestamp:0
                                     windowNumber:nswindow.windowNumber
                                          context:nil
                                      eventNumber:0
                                       clickCount:1
                                         pressure:0.0];

        // Dispatch directly to the view — bypasses the AppKit responder
        // chain so the test stays deterministic on a headless CI runner.
        REQUIRE_NOTHROW([contentView mouseDown:down]);

        // Mirror the real-world race: between mouseDown and mouseUp, the
        // app's React-style state machine unmounts and frees the clicked
        // child. After this, PulpView's private `_dragTarget` ivar points
        // at freed memory.
        auto removed = root.remove_child(child_ptr);
        REQUIRE(removed != nullptr);
        removed.reset();   // free the heap object so any later deref by
                           // PulpView is a true use-after-free.

        // Without the fix this call SIGSEGVs deterministically (loads
        // `_dragTarget->on_click` from freed memory). With the fix the
        // `view_is_in_tree` check rejects the dangling pointer and the
        // up event drops cleanly.
        REQUIRE_NOTHROW([contentView mouseUp:up]);

        host->hide();
    }
}

// pulp #1006 — full integration: a JSX `onClick={fn}` flows through
// @pulp/react's prop-applier into a bare `on(id, 'click', fn)` bridge
// call (no addEventListener, no explicit registerClick). A real
// NSLeftMouseDown + NSLeftMouseUp pair on the contentView must reach
// the JS handler. Pre-#1006 the bridge stored the callback in
// __callbacks__ but never wired View::on_click, so post-#992 mouseUp
// found `_dragTarget->on_click == nullptr` and silently dropped the
// click — the user-facing symptom that every <RailBtn onClick=...>
// stayed dead in Spectr.
TEST_CASE("PulpView NSEvent click dispatches JS on(id,'click') subscriber",
          "[view][hosts][bridge][issue-1006]") {
    @autoreleasepool {
        [NSApplication sharedApplication];

        View root;
        WindowOptions opts;
        opts.title = "PulpView issue-1006";
        opts.width = 200.0f;
        opts.height = 100.0f;
        opts.resizable = false;
        opts.use_gpu = false;

        auto host = WindowHost::create(root, opts);
        REQUIRE(host != nullptr);
        // hit_test walks the live bounds; the host only resizes the
        // root during paint, so set bounds up front so the synthetic
        // event lands on the child without depending on a paint cycle.
        root.set_bounds({0, 0, 200, 100});

        pulp::state::StateStore store;
        pulp::view::ScriptEngine engine;
        pulp::view::WidgetBridge bridge(engine, root, store);

        // createCol → child View added to root with the given id; the
        // subsequent `on('clear', 'click', fn)` is exactly the call the
        // @pulp/react prop-applier emits for `<RailBtn onClick={fn}>`.
        // Pin a preferred size so Yoga doesn't shrink the child to 0
        // during paint-driven layout (test stays deterministic).
        bridge.load_script(R"(
            var clicks = 0;
            createCol('clear', '');
            setFlex('clear', 'width', 200);
            setFlex('clear', 'height', 100);
            on('clear', 'click', function() { clicks += 1; });
        )");

        auto* child_ptr = bridge.widget("clear");
        REQUIRE(child_ptr != nullptr);

        host->show();
        auto* nswindow = (__bridge NSWindow*)host->native_window_handle();
        REQUIRE(nswindow != nil);
        auto* contentView = nswindow.contentView;
        REQUIRE(contentView != nil);

        for (int i = 0; i < 20; ++i) {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
        }

        const NSPoint local = NSMakePoint(40.0, 50.0);
        const NSPoint window_pt = [contentView convertPoint:local toView:nil];

        NSEvent* down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                           location:window_pt
                                      modifierFlags:0
                                          timestamp:0
                                       windowNumber:nswindow.windowNumber
                                            context:nil
                                        eventNumber:0
                                         clickCount:1
                                           pressure:1.0];
        NSEvent* up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                         location:window_pt
                                    modifierFlags:0
                                        timestamp:0
                                     windowNumber:nswindow.windowNumber
                                          context:nil
                                      eventNumber:0
                                       clickCount:1
                                         pressure:0.0];

        // Sanity: hit_test must find the child under the click point;
        // otherwise mouseDown sets _dragTarget=nullptr and mouseUp has
        // no view to dispatch the click to.
        {
            auto* target = root.hit_test({static_cast<float>(local.x),
                                          static_cast<float>(local.y)});
            REQUIRE(target == child_ptr);
        }

        [contentView mouseDown:down];
        [contentView mouseUp:up];

        // mouseUp queues the click handler via dispatch_async on the
        // main queue (post-#992). Drain the run loop until it fires.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        int clicks = 0;
        while (clicks == 0 && std::chrono::steady_clock::now() < deadline) {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
            clicks = engine.evaluate("clicks").getWithDefault<int>(0);
        }

        REQUIRE(clicks == 1);

        host->hide();
    }
}

// pulp #1067 — #1006/#1008 wired `on(id, 'click', fn)` to the native
// `View::on_click`, but the click still doesn't fire when the visible
// hit target is a child of the registered widget. The classic shape:
// `<button onClick=...>Clear</button>` lowers (via Spectr's dom-adapter
// + @pulp/react host config) into a `<View onClick=...>` with a child
// `<Label>Clear</Label>`. `hit_test` walks topmost-child-first and
// returns the Label (the visible text covers the parent's bounds), but
// `on_click` is registered on the parent View. Pre-#1067 the mac
// mouseUp path captured `_dragTarget = label`, found `on_click ==
// nullptr`, and silently dropped the click. Fix: walk up the parent
// chain to find the nearest ancestor with a registered handler — DOM
// click bubbling.
TEST_CASE("PulpView NSEvent click bubbles up to ancestor on_click handler",
          "[view][hosts][bridge][issue-1067]") {
    @autoreleasepool {
        [NSApplication sharedApplication];

        View root;
        WindowOptions opts;
        opts.title = "PulpView issue-1067";
        opts.width = 200.0f;
        opts.height = 100.0f;
        opts.resizable = false;
        opts.use_gpu = false;

        auto host = WindowHost::create(root, opts);
        REQUIRE(host != nullptr);
        root.set_bounds({0, 0, 200, 100});

        pulp::state::StateStore store;
        pulp::view::ScriptEngine engine;
        pulp::view::WidgetBridge bridge(engine, root, store);

        // Mirrors what @pulp/react + Spectr's dom-adapter emit for
        // `<button onClick={fn}>Clear</button>`:
        //   * a Panel (Button) with the click handler attached
        //   * a Label child whose bounds cover the visible button area
        // Pin the parent's bounds so hit_test lands on the Label child
        // deterministically without depending on a paint pass.
        bridge.load_script(R"(
            var clicks = 0;
            createPanel('btn', '');
            setFlex('btn', 'width', 200);
            setFlex('btn', 'height', 100);
            createLabel('btn_l', 'Clear', 'btn');
            setFlex('btn_l', 'width', 200);
            setFlex('btn_l', 'height', 100);
            on('btn', 'click', function() { clicks += 1; });
        )");

        auto* button = bridge.widget("btn");
        auto* label  = bridge.widget("btn_l");
        REQUIRE(button != nullptr);
        REQUIRE(label  != nullptr);
        REQUIRE(label->parent() == button);
        // The handler is on the parent button.
        REQUIRE(static_cast<bool>(button->on_click));
        // The label has no handler of its own — that's the point.
        REQUIRE_FALSE(static_cast<bool>(label->on_click));

        host->show();
        auto* nswindow = (__bridge NSWindow*)host->native_window_handle();
        REQUIRE(nswindow != nil);
        auto* contentView = nswindow.contentView;
        REQUIRE(contentView != nil);

        for (int i = 0; i < 20; ++i) {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
        }

        // Sanity: hit_test must find the Label child, not the Button
        // parent (otherwise the test wouldn't be exercising the bubble
        // path — it would be the trivial #1006 case).
        {
            auto* target = root.hit_test({40.0f, 50.0f});
            REQUIRE(target == label);
        }

        const NSPoint local = NSMakePoint(40.0, 50.0);
        const NSPoint window_pt = [contentView convertPoint:local toView:nil];

        NSEvent* down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                           location:window_pt
                                      modifierFlags:0
                                          timestamp:0
                                       windowNumber:nswindow.windowNumber
                                            context:nil
                                        eventNumber:0
                                         clickCount:1
                                           pressure:1.0];
        NSEvent* up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                         location:window_pt
                                    modifierFlags:0
                                        timestamp:0
                                     windowNumber:nswindow.windowNumber
                                          context:nil
                                      eventNumber:0
                                       clickCount:1
                                         pressure:0.0];

        [contentView mouseDown:down];
        [contentView mouseUp:up];

        // mouseUp queues the click handler via dispatch_async on the
        // main queue; drain the run loop until it fires (or we hit a
        // generous timeout, in which case the bubble path is broken).
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        int clicks = 0;
        while (clicks == 0 && std::chrono::steady_clock::now() < deadline) {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
            clicks = engine.evaluate("clicks").getWithDefault<int>(0);
        }

        REQUIRE(clicks == 1);

        host->hide();
    }
}

#endif
