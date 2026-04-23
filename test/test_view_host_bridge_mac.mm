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

#endif
