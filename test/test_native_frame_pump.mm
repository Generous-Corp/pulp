// test_native_frame_pump.mm — pulp::native::FramePump, the optional
// display-paced frame driver for native-surface editors.
//
// The cases run on whatever tick source the host has: the display link, or the
// 60 Hz fallback timer on a host with no display. One case checks the display
// link itself and skips, saying so, without a display. Main-thread delivery
// needs the main run loop pumped, so each case runs it for a bounded time.
// Compiled with ARC.

#include <catch2/catch_test_macros.hpp>

#include <pulp/native/frame_pump.hpp>

#include "support/thread_progress.hpp"

#import <Foundation/Foundation.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace pulp::native;

namespace {

void pump_main(double seconds) {
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}

struct Counter {
    std::atomic<int> frames{0};
    std::atomic<int> off_main{0};
    std::atomic<int> on_main{0};
    std::function<void(const FrameTick&)> callback() {
        return [this](const FrameTick&) {
            frames.fetch_add(1);
            if ([NSThread isMainThread])
                on_main.fetch_add(1);
            else
                off_main.fetch_add(1);
        };
    }
};

#define REQUIRE_STARTED(pump)                                                                      \
    do {                                                                                           \
        if (!(pump).start())                                                                       \
            FAIL("the frame pump did not start");                                                  \
    } while (0)

} // namespace

TEST_CASE("a frame pump that is never started creates no tick source",
          "[native-surface][frame-pump]") {
    const auto before = FramePump::live_tick_sources();
    Counter counter;
    {
        FramePump pump(counter.callback(), {60.0, true, FrameThread::main});
        pump.request_frame();
        pump_main(0.3);
        REQUIRE_FALSE(pump.running());
        REQUIRE(FramePump::live_tick_sources() == before);
        REQUIRE(counter.frames.load() == 0);

        // Control: the same pump, started, does tick and does hold a source.
        REQUIRE_STARTED(pump);
        REQUIRE(FramePump::live_tick_sources() == before + 1);
        pump_main(0.3);
        REQUIRE(counter.frames.load() > 0);
    }
    REQUIRE(FramePump::live_tick_sources() == before);
}

TEST_CASE("with a display the pump follows its display link", "[native-surface][frame-pump]") {
    const auto before = FramePump::live_display_links();
    Counter counter;
    FramePump pump(counter.callback(), {60.0, true, FrameThread::render});
    REQUIRE_STARTED(pump);
    if (!pump.follows_display()) {
        REQUIRE(FramePump::live_display_links() == before);
        SKIP("no active display on this host; the fallback timer drove the other cases");
    }
    REQUIRE(FramePump::live_display_links() == before + 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(counter.frames.load() > 0);
    pump.stop();
    REQUIRE(FramePump::live_display_links() == before);
}

TEST_CASE("the rate cap holds within ten percent", "[native-surface][frame-pump]") {
    for (const double cap : {30.0, 20.0}) {
        Counter counter;
        FramePump pump(counter.callback(), {cap, true, FrameThread::render});
        REQUIRE_STARTED(pump);
        // Settle, then count over a measured window.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const int start = counter.frames.load();
        const auto t0 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::seconds(2));
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double rate = (counter.frames.load() - start) / seconds;
        pump.stop();
        INFO("cap " << cap << " Hz delivered " << rate << " Hz");
        REQUIRE(std::abs(rate - cap) <= cap * 0.10);
    }
}

TEST_CASE("the rate cap changes at run time and is clamped", "[native-surface][frame-pump]") {
    Counter counter;
    FramePump pump(counter.callback(), {60.0, true, FrameThread::render});
    pump.set_rate_cap(0.0);
    REQUIRE(pump.rate_cap() == 1.0);
    pump.set_rate_cap(1000.0);
    REQUIRE(pump.rate_cap() == 300.0);
    pump.set_rate_cap(10.0);
    REQUIRE(pump.rate_cap() == 10.0);
    REQUIRE_STARTED(pump);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    const int at_ten = counter.frames.load();
    pump.stop();
    INFO("frames in 1.1 s at 10 Hz: " << at_ten);
    REQUIRE(at_ten >= 9);
    REQUIRE(at_ten <= 13);
}

TEST_CASE("a one-shot request delivers exactly one frame", "[native-surface][frame-pump]") {
    Counter counter;
    FramePump pump(counter.callback(), {60.0, false, FrameThread::main});
    REQUIRE_STARTED(pump);
    pump_main(0.2);
    REQUIRE(counter.frames.load() == 0);
    pump.request_frame();
    pump.request_frame(); // two requests before the vsync are one frame
    pump_main(0.3);
    REQUIRE(counter.frames.load() == 1);
    REQUIRE(counter.on_main.load() == 1);
    pump_main(0.2);
    REQUIRE(counter.frames.load() == 1);
}

TEST_CASE("nothing is delivered while the surface is off screen", "[native-surface][frame-pump]") {
    Counter counter;
    FramePump pump(counter.callback(), {60.0, true, FrameThread::render});
    pump.set_on_screen(false);
    REQUIRE_STARTED(pump);
    pump.request_frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(counter.frames.load() == 0);
    // Control: the same pump on screen delivers, including the held one-shot.
    pump.set_on_screen(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(counter.frames.load() > 0);
}

TEST_CASE("frames arrive on the thread the pump was asked for", "[native-surface][frame-pump]") {
    Counter on_main;
    FramePump main_pump(on_main.callback(), {60.0, true, FrameThread::main});
    REQUIRE_STARTED(main_pump);
    pump_main(0.3);
    main_pump.stop();
    REQUIRE(on_main.frames.load() > 0);
    REQUIRE(on_main.off_main.load() == 0);

    Counter on_render;
    FramePump render_pump(on_render.callback(), {60.0, true, FrameThread::render});
    REQUIRE_STARTED(render_pump);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    render_pump.stop();
    REQUIRE(on_render.frames.load() > 0);
    REQUIRE(on_render.on_main.load() == 0);
}

namespace {

/// Holds a frame callback until the test releases it, so "a frame is still
/// running" is a state the test controls rather than a duration. The wait is
/// bounded; it runs on the pump's thread, where a timeout simply ends the
/// frame and the test's own bounded waits then fail by name.
struct FrameGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool open = false;
    void hold_until_released() {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, pulp::test::kProgressDeadline, [this] { return open; });
    }
    void release() {
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        cv.notify_all();
    }
};

void run_main_loop_once() {
    [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                             beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
}

} // namespace

TEST_CASE("a slow frame is skipped, never queued", "[native-surface][frame-pump]") {
    // Vsyncs come from the test through a manual tick source, and the frame
    // runs until the test releases it, so nothing here depends on wall-clock
    // time or host load.
    FrameGate gate;
    std::atomic<int> running{0};
    std::atomic<int> overlaps{0};
    std::atomic<int> started{0};
    FramePump pump(
        [&](const FrameTick&) {
            if (running.fetch_add(1) != 0)
                overlaps.fetch_add(1);
            started.fetch_add(1);
            gate.hold_until_released();
            running.fetch_sub(1);
        },
        {300.0, true, FrameThread::render, TickSource::manual});
    const auto sources = FramePump::live_tick_sources();
    REQUIRE(pump.start());
    REQUIRE_FALSE(pump.follows_display());
    REQUIRE(FramePump::live_tick_sources() == sources);

    const double vsync = 1.0 / 120.0;
    double t = 1.0;
    REQUIRE(pump.tick(t, vsync));
    REQUIRE(pulp::test::wait_for_condition([&] { return started.load() == 1; }));

    // Every vsync while the frame still runs is skipped, not stored up.
    for (int i = 0; i < 50; ++i) {
        t += vsync;
        REQUIRE_FALSE(pump.tick(t, vsync));
    }
    gate.release();

    // Once it returns, the next vsync delivers exactly one frame, not the 50
    // that were due meanwhile.
    REQUIRE(pulp::test::wait_for_condition([&] {
        t += vsync;
        return pump.tick(t, vsync);
    }));
    REQUIRE(pulp::test::wait_for_condition([&] { return started.load() == 2; }));
    REQUIRE(pulp::test::wait_for_condition([&] { return running.load() == 0; }));
    pump.stop();
    REQUIRE(started.load() == 2);
    REQUIRE(overlaps.load() == 0);
}

TEST_CASE("a slow main-thread frame never builds a backlog", "[native-surface][frame-pump]") {
    // Main-thread delivery: a frame is posted to the main run loop. Vsyncs
    // that arrive before it has run are skipped, so running the loop once
    // delivers one frame however many vsyncs passed.
    int frames = 0;
    FramePump pump([&](const FrameTick&) { ++frames; },
                   {300.0, true, FrameThread::main, TickSource::manual});
    REQUIRE(pump.start());

    const double vsync = 1.0 / 120.0;
    double t = 1.0;
    REQUIRE(pump.tick(t, vsync));
    for (int i = 0; i < 50; ++i) {
        t += vsync;
        REQUIRE_FALSE(pump.tick(t, vsync));
    }
    REQUIRE(pulp::test::pump_until([&] { return frames > 0; }, run_main_loop_once));
    REQUIRE(frames == 1);
    t += vsync;
    REQUIRE(pump.tick(t, vsync));
    REQUIRE(pulp::test::pump_until([&] { return frames > 1; }, run_main_loop_once));
    REQUIRE(frames == 2);
    pump.stop();
}

TEST_CASE("a manual pump delivers only when ticked and never after stop",
          "[native-surface][frame-pump]") {
    std::atomic<int> frames{0};
    FramePump pump([&](const FrameTick&) { frames.fetch_add(1); },
                   {60.0, false, FrameThread::render, TickSource::manual});
    REQUIRE_FALSE(pump.tick(1.0, 1.0 / 60.0)); // not started
    REQUIRE(pump.start());
    REQUIRE_FALSE(pump.tick(1.0, 1.0 / 60.0)); // nothing requested
    pump.request_frame();
    REQUIRE(pump.tick(1.0, 1.0 / 60.0));
    REQUIRE(pulp::test::wait_for_condition([&] { return frames.load() == 1; }));
    pump.stop();
    pump.request_frame();
    REQUIRE_FALSE(pump.tick(2.0, 1.0 / 60.0));
    REQUIRE(frames.load() == 1);
}

TEST_CASE("start and stop a hundred times leaves no tick source and no late frame",
          "[native-surface][frame-pump]") {
    const auto before = FramePump::live_tick_sources();
    Counter counter;
    FramePump pump(counter.callback(), {300.0, true, FrameThread::render});
    for (int i = 0; i < 100; ++i) {
        REQUIRE_STARTED(pump);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        pump.stop();
        REQUIRE_FALSE(pump.running());
    }
    REQUIRE(FramePump::live_tick_sources() == before);
    const int after_stop = counter.frames.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(counter.frames.load() == after_stop);
}

TEST_CASE("a main-thread frame may destroy its own pump", "[native-surface][frame-pump]") {
    std::unique_ptr<FramePump> pump;
    int frames = 0;
    pump = std::make_unique<FramePump>(
        [&](const FrameTick&) {
            ++frames;
            pump.reset();
        },
        FramePumpOptions{60.0, true, FrameThread::main});
    REQUIRE(pump->start());
    pump_main(0.3);
    REQUIRE(frames == 1);
    REQUIRE(pump == nullptr);
}
