// An existing editor's idle timer and post_to_main_thread keep running while
// the host's main thread sits in a nested run loop (menu tracking, a modal
// panel), including a nested loop entered from a GCD main-queue block, where
// a main-queue dispatch timer cannot fire. The dispatch timer read alongside
// is the control: the same nested loop gives it no ticks.

#include <pulp/format/detail/main_thread_timer.hpp>

#include <catch2/catch_test_macros.hpp>

#import <AppKit/AppKit.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>

#include <chrono>
#include <functional>
#include <memory>

using pulp::format::detail::MainThreadTimer;
using namespace std::chrono_literals;

namespace {

template <typename Pred> bool pump_until(Pred done, std::chrono::milliseconds limit = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
    }
    return true;
}

struct NestedResult {
    int timer_ticks = -1;
    int dispatch_ticks = -1;
    bool post_ran = false;
};

// Inside a main-queue block, run `mode` for 250 ms with the editor timer at
// 10 ms, a dispatch timer at 10 ms, and one post_to_main_thread made from
// inside the nested loop.
NestedResult run_nested(CFStringRef mode) {
    [NSApplication sharedApplication]; // registers AppKit's common modes
    struct Probe {
        MainThreadTimer timer;
        int timer_ticks = 0;
        int dispatch_ticks = 0;
        bool post_ran = false;
        bool done = false;
        NestedResult result;
    };
    auto probe = std::make_unique<Probe>();
    Probe* p = probe.get();

    dispatch_source_t dispatch_timer =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(dispatch_timer, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC),
                              10 * NSEC_PER_MSEC, 0);
    dispatch_source_set_event_handler(dispatch_timer, ^{
      ++p->dispatch_ticks;
    });

    dispatch_async(dispatch_get_main_queue(), ^{
      p->timer.start(10, [p] { ++p->timer_ticks; });
      dispatch_resume(dispatch_timer);
      const int t0 = p->timer_ticks;
      const int d0 = p->dispatch_ticks;
      std::function<void()> post = [p] { p->post_ran = true; };
      pulp::format::detail::post_to_main_thread(post);
      const auto deadline = std::chrono::steady_clock::now() + 250ms;
      while (std::chrono::steady_clock::now() < deadline)
          CFRunLoopRunInMode(mode, 0.01, false);
      p->result.timer_ticks = p->timer_ticks - t0;
      p->result.dispatch_ticks = p->dispatch_ticks - d0;
      p->result.post_ran = p->post_ran;
      p->timer.stop();
      p->done = true;
    });
    pump_until([p] { return p->done; });
    dispatch_source_cancel(dispatch_timer);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
    return p->result;
}

} // namespace

TEST_CASE("an editor timer and posts keep running during menu tracking",
          "[editor-host][main-thread-timer][nested-run-loop]") {
    const auto r = run_nested((__bridge CFStringRef)NSEventTrackingRunLoopMode);
    CHECK(r.dispatch_ticks == 0); // control: the dispatch path is held back
    REQUIRE(r.timer_ticks >= 12);
    REQUIRE(r.post_ran);
}

TEST_CASE("an editor timer and posts keep running inside a modal panel loop",
          "[editor-host][main-thread-timer][nested-run-loop]") {
    const auto r = run_nested((__bridge CFStringRef)NSModalPanelRunLoopMode);
    CHECK(r.dispatch_ticks == 0);
    REQUIRE(r.timer_ticks >= 12);
    REQUIRE(r.post_ran);
}

TEST_CASE("a stopped editor timer leaves nothing on the run loop",
          "[editor-host][main-thread-timer]") {
    int ticks = 0;
    MainThreadTimer timer;
    REQUIRE(timer.start(2, [&] { ++ticks; }));
    REQUIRE(pump_until([&] { return ticks >= 3; }));
    timer.stop();
    REQUIRE_FALSE(timer.running());
    const int at_stop = ticks;
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    REQUIRE(ticks == at_stop);
}
