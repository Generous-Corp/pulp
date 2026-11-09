// MainThreadScope / MainThreadTimer: posting from any thread, dropping work
// whose owner is gone, and firing inside nested run loops (menu tracking and
// modal panels) where the main dispatch queue cannot.

#include <pulp/platform/main_thread.hpp>

#include <catch2/catch_test_macros.hpp>

#import <AppKit/AppKit.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using pulp::platform::MainThreadScope;
using pulp::platform::MainThreadTimer;
using namespace std::chrono_literals;

namespace {

// AppKit registers its tracking and modal-panel modes as common modes when the
// shared application is created, as it always is inside a host. A test
// process is a background app, so App Nap may also coalesce its timers; the
// activity keeps it at the latency a host's foreground editor gets. Counts
// below still only require the timer to fire repeatedly, not at full rate,
// because shared CI machines (and sanitizers) slow it further.
void ensure_app() {
    [NSApplication sharedApplication];
    static id activity = [[[NSProcessInfo processInfo]
        beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                          reason:@"main-thread timer timing"] retain];
    (void)activity;
}

// Pump the default mode until `done` or the deadline passes.
template <typename Pred> bool pump_until(Pred done, std::chrono::milliseconds limit = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
    }
    return true;
}

void pump_for(std::chrono::milliseconds span) {
    const auto deadline = std::chrono::steady_clock::now() + span;
    while (std::chrono::steady_clock::now() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
}

const CFStringRef kTrackingMode = (__bridge CFStringRef)NSEventTrackingRunLoopMode;
const CFStringRef kModalMode = (__bridge CFStringRef)NSModalPanelRunLoopMode;

} // namespace

TEST_CASE("main thread backend exists on macOS", "[platform][main-thread]") {
    REQUIRE(pulp::platform::main_thread_supported());
    REQUIRE(pulp::platform::is_main_thread());
    bool worker_is_main = true;
    std::thread([&] { worker_is_main = pulp::platform::is_main_thread(); }).join();
    REQUIRE_FALSE(worker_is_main);
}

TEST_CASE("a post from a worker thread runs once on the main thread", "[platform][main-thread]") {
    ensure_app();
    MainThreadScope scope;
    std::atomic<int> runs{0};
    std::atomic<bool> on_main{false};
    std::atomic<bool> accepted{false};
    std::thread worker([&, handle = scope.handle()] {
        accepted = handle.post([&] {
            on_main = pulp::platform::is_main_thread();
            ++runs;
        });
    });
    worker.join();
    REQUIRE(accepted.load());
    REQUIRE(pump_until([&] { return runs.load() > 0; }));
    pump_for(20ms);
    REQUIRE(runs.load() == 1);
    REQUIRE(on_main.load());
}

TEST_CASE("posted tasks run in order", "[platform][main-thread]") {
    MainThreadScope scope;
    std::vector<int> order;
    for (int i = 0; i < 5; ++i)
        REQUIRE(scope.post([&order, i] { order.push_back(i); }));
    REQUIRE(pump_until([&] { return order.size() == 5; }));
    REQUIRE(order == std::vector<int>{0, 1, 2, 3, 4});
}

TEST_CASE("a handle refuses to post once its scope is gone", "[platform][main-thread]") {
    bool ran = false;
    MainThreadScope::Handle handle;
    {
        MainThreadScope scope;
        handle = scope.handle();
        REQUIRE(handle.alive());
    }
    REQUIRE_FALSE(handle.alive());
    REQUIRE_FALSE(handle.post([&] { ran = true; }));
    pump_for(20ms);
    REQUIRE_FALSE(ran);
}

TEST_CASE("a task still queued when its scope dies never runs", "[platform][main-thread][drop]") {
    bool ran = false;
    {
        MainThreadScope scope;
        REQUIRE(scope.post([&] { ran = true; }));
        // Destroyed before the run loop gets a chance to service the source.
    }
    pump_for(30ms);
    REQUIRE_FALSE(ran);
}

TEST_CASE("a task may destroy its scope; the rest of the batch is dropped",
          "[platform][main-thread][batch-drop]") {
    auto scope = std::make_unique<MainThreadScope>();
    int later = 0;
    bool first = false;
    REQUIRE(scope->post([&] {
        first = true;
        scope.reset();
    }));
    REQUIRE(scope->post([&] { ++later; }));
    REQUIRE(pump_until([&] { return first; }));
    pump_for(20ms);
    REQUIRE(scope == nullptr);
    REQUIRE(later == 0);
}

TEST_CASE("a throwing task is contained and later tasks still run", "[platform][main-thread]") {
    MainThreadScope scope;
    bool after = false;
    REQUIRE(scope.post([] { throw 42; }));
    REQUIRE(scope.post([&] { after = true; }));
    REQUIRE(pump_until([&] { return after; }));
}

// The property the dispatch-based helpers cannot offer: work keeps running
// while a main-queue block runs a nested loop in the tracking or modal mode.
// The outer task here is a GCD main-queue block on purpose, because that is
// where a host or an older Pulp helper may be sitting when a menu opens.
static void check_ticks_inside_nested_mode(CFStringRef mode) {
    ensure_app();
    struct Probe {
        MainThreadScope scope;
        int timer_ticks = 0;
        int dispatch_ticks = 0;
        bool nested_post_ran = false;
        int ticks_in_nested = -1;
        int dispatch_ticks_in_nested = -1;
        bool outer_done = false;
    };
    auto probe = std::make_unique<Probe>();
    Probe* p = probe.get();
    MainThreadTimer timer(p->scope, [p] { ++p->timer_ticks; });
    MainThreadTimer* t = &timer;

    dispatch_source_t dispatch_timer =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(dispatch_timer, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC),
                              10 * NSEC_PER_MSEC, 0);
    dispatch_source_set_event_handler(dispatch_timer, ^{
      ++p->dispatch_ticks;
    });

    dispatch_async(dispatch_get_main_queue(), ^{
      t->fire_every(10ms);
      dispatch_resume(dispatch_timer);
      const int t0 = p->timer_ticks;
      const int d0 = p->dispatch_ticks;
      p->scope.post([p] { p->nested_post_ran = true; });
      const auto deadline = std::chrono::steady_clock::now() + 250ms;
      while (std::chrono::steady_clock::now() < deadline)
          CFRunLoopRunInMode(mode, 0.01, false);
      p->ticks_in_nested = p->timer_ticks - t0;
      p->dispatch_ticks_in_nested = p->dispatch_ticks - d0;
      p->outer_done = true;
    });
    REQUIRE(pump_until([p] { return p->outer_done; }, 3000ms));
    timer.cancel();
    dispatch_source_cancel(dispatch_timer);
    pump_for(20ms);

    // Comparator, not a gate on GCD: it shows why the dispatch path is unfit.
    CHECK(p->dispatch_ticks_in_nested == 0);
    REQUIRE(p->ticks_in_nested >= 3);
    REQUIRE(p->nested_post_ran);
}

TEST_CASE("timers and posts run inside menu tracking under a main-queue block",
          "[platform][main-thread][nested-run-loop]") {
    check_ticks_inside_nested_mode(kTrackingMode);
}

TEST_CASE("timers and posts run inside a modal panel loop under a main-queue block",
          "[platform][main-thread][nested-run-loop]") {
    check_ticks_inside_nested_mode(kModalMode);
}

TEST_CASE("fire_after is one-shot and replaces a pending fire with zero tolerance",
          "[platform][main-thread][timer][one-shot]") {
    MainThreadScope scope;
    int fired = 0;
    MainThreadTimer timer(scope, [&] { ++fired; });
    REQUIRE_FALSE(timer.pending());
    REQUIRE_FALSE(timer.next_fire().has_value());

    timer.fire_after(500ms);
    timer.fire_after(5ms); // replaces the 500 ms fire
    REQUIRE(timer.pending());
    REQUIRE(timer.tolerance() == std::chrono::microseconds(0));
    const auto expected = std::chrono::steady_clock::now() + 5ms;
    const auto next = timer.next_fire();
    REQUIRE(next.has_value());
    const auto error = *next > expected ? *next - expected : expected - *next;
    REQUIRE(error < 1ms);

    REQUIRE(pump_until([&] { return fired > 0; }));
    pump_for(40ms);
    REQUIRE(fired == 1);
    REQUIRE_FALSE(timer.pending());

    // Reschedulable after it fired.
    timer.fire_after(1ms);
    REQUIRE(pump_until([&] { return fired == 2; }));
}

TEST_CASE("cancel stops a repeating timer", "[platform][main-thread][timer]") {
    MainThreadScope scope;
    int fired = 0;
    MainThreadTimer timer(scope, [&] { ++fired; });
    timer.fire_every(2ms);
    REQUIRE(pump_until([&] { return fired >= 3; }));
    timer.cancel();
    const int at_cancel = fired;
    pump_for(30ms);
    REQUIRE(fired == at_cancel);
    REQUIRE_FALSE(timer.pending());
}

TEST_CASE("a timer may destroy itself and its scope from its own callback",
          "[platform][main-thread][timer]") {
    auto scope = std::make_unique<MainThreadScope>();
    std::unique_ptr<MainThreadTimer> timer;
    int fired = 0;
    timer = std::make_unique<MainThreadTimer>(*scope, [&] {
        ++fired;
        timer.reset();
        scope.reset();
    });
    timer->fire_every(1ms);
    REQUIRE(pump_until([&] { return fired > 0; }));
    pump_for(20ms);
    REQUIRE(fired == 1);
    REQUIRE(timer == nullptr);
    REQUIRE(scope == nullptr);
}

TEST_CASE("destroying the scope stops its timers for good", "[platform][main-thread][timer]") {
    auto scope = std::make_unique<MainThreadScope>();
    int fired = 0;
    MainThreadTimer timer(*scope, [&] { ++fired; });
    timer.fire_every(1ms);
    scope.reset();
    REQUIRE_FALSE(timer.pending());
    timer.fire_after(1ms); // no effect once the scope is gone
    REQUIRE_FALSE(timer.pending());
    pump_for(30ms);
    REQUIRE(fired == 0);
}

// An idle scope must cost the main thread nothing: the run loop blocks and
// never calls into this layer. Counting run-loop wake-ups directly is not
// attributable (the loop also wakes for its own timeout and for system
// sources), so the instrument is the layer's own callout counter. Its control
// is an armed timer on the same scope over the same span.
TEST_CASE("an idle scope causes no main-thread callouts", "[platform][main-thread][idle]") {
    ensure_app();
    pump_for(20ms); // drain anything left by earlier cases
    MainThreadScope scope;
    MainThreadTimer timer(scope, [] {});
    timer.fire_after(5ms);
    timer.cancel(); // parked, as an idle editor's timer is

    const auto before = pulp::platform::detail::main_thread_callouts();
    const CFRunLoopRunResult idle_result = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);
    REQUIRE(idle_result == kCFRunLoopRunTimedOut); // it really blocked
    REQUIRE(pulp::platform::detail::main_thread_callouts() == before);

    timer.fire_every(10ms);
    const auto armed_before = pulp::platform::detail::main_thread_callouts();
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);
    timer.cancel();
    REQUIRE(pulp::platform::detail::main_thread_callouts() - armed_before >= 3);
}
