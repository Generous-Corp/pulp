#include <TargetConditionals.h>

#if TARGET_OS_OSX

#import <Cocoa/Cocoa.h>

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/view.hpp>
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

#endif
