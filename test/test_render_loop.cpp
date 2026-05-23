#include <catch2/catch_test_macros.hpp>

#include <pulp/render/render_loop.hpp>

#include "render_loop_state.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace pulp::render;

namespace {

bool wait_for_count(std::atomic<int>& count, int target) {
    for (int i = 0; i < 200; ++i) {
        if (count.load(std::memory_order_relaxed) >= target) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return count.load(std::memory_order_relaxed) >= target;
}

} // namespace

TEST_CASE("RenderLoopState starts stopped and schedules the first frame",
          "[render][loop][issue-646]") {
    RenderLoopState state;

    REQUIRE_FALSE(state.is_running());
    REQUIRE_FALSE(state.consume_frame_request());

    REQUIRE(state.start());
    REQUIRE(state.is_running());
    REQUIRE(state.consume_frame_request());
    REQUIRE_FALSE(state.consume_frame_request());
}

TEST_CASE("RenderLoopState coalesces repeated frame requests",
          "[render][loop][issue-646]") {
    RenderLoopState state;
    REQUIRE(state.start());
    REQUIRE(state.consume_frame_request());

    state.request_frame();
    state.request_frame();

    REQUIRE(state.consume_frame_request());
    REQUIRE_FALSE(state.consume_frame_request());
}

TEST_CASE("RenderLoopState ignores requests while stopped",
          "[render][loop][issue-646]") {
    RenderLoopState state;

    state.request_frame();
    REQUIRE_FALSE(state.consume_frame_request());

    REQUIRE(state.start());
    REQUIRE(state.consume_frame_request());
    REQUIRE(state.stop());

    state.request_frame();
    REQUIRE_FALSE(state.consume_frame_request());
}

TEST_CASE("RenderLoopState stop clears pending work and is idempotent",
          "[render][loop][issue-646]") {
    RenderLoopState state;
    REQUIRE(state.start());

    state.request_frame();
    REQUIRE(state.stop());
    REQUIRE_FALSE(state.is_running());
    REQUIRE_FALSE(state.consume_frame_request());

    REQUIRE_FALSE(state.stop());
    REQUIRE_FALSE(state.is_running());
}

TEST_CASE("RenderLoopState repeated start only requests another frame",
          "[render][loop][issue-646]") {
    RenderLoopState state;
    REQUIRE(state.start());
    REQUIRE(state.consume_frame_request());

    REQUIRE_FALSE(state.start());
    REQUIRE(state.is_running());
    REQUIRE(state.consume_frame_request());
}

TEST_CASE("RenderLoop timer backend tolerates stop before start",
          "[render][loop][issue-646]") {
    auto loop = RenderLoop::create();
    REQUIRE(loop != nullptr);
    REQUIRE_FALSE(loop->is_running());

    loop->stop();
    REQUIRE_FALSE(loop->is_running());
}

TEST_CASE("RenderLoop timer backend handles an empty callback",
          "[render][loop][issue-646]") {
    auto loop = RenderLoop::create();
    REQUIRE(loop != nullptr);

    loop->start(FrameCallback{});
    REQUIRE(loop->is_running());
    loop->request_frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    loop->stop();

    REQUIRE_FALSE(loop->is_running());
}

TEST_CASE("RenderLoop timer backend repeated start keeps the active callback",
          "[render][loop][issue-646]") {
    auto loop = RenderLoop::create();
    REQUIRE(loop != nullptr);

    std::atomic<int> first{0};
    std::atomic<int> second{0};

    loop->start([&]() { first.fetch_add(1, std::memory_order_relaxed); });
    REQUIRE(wait_for_count(first, 1));

    loop->start([&]() { second.fetch_add(1, std::memory_order_relaxed); });
    REQUIRE(loop->is_running());
    REQUIRE(wait_for_count(first, 2));

    loop->stop();

    REQUIRE_FALSE(loop->is_running());
    REQUIRE(second.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("RenderLoop timer backend can restart with a new callback",
          "[render][loop][issue-646]") {
    auto loop = RenderLoop::create();
    REQUIRE(loop != nullptr);

    std::atomic<int> first{0};
    std::atomic<int> second{0};

    loop->start([&]() { first.fetch_add(1, std::memory_order_relaxed); });
    REQUIRE(wait_for_count(first, 1));
    loop->stop();

    loop->start([&]() { second.fetch_add(1, std::memory_order_relaxed); });
    REQUIRE(wait_for_count(second, 1));
    loop->stop();

    REQUIRE(first.load(std::memory_order_relaxed) == 1);
    REQUIRE(second.load(std::memory_order_relaxed) == 1);
}

// ── Slice 16 — VBlank-locked safe-repaint additions ─────────────────────

TEST_CASE("RenderLoop factory selects the timer backend under force-timer",
          "[render][loop][vblank][slice-16]") {
    // This test target is compiled with PULP_RENDER_LOOP_FORCE_TIMER=1, so
    // the factory must select the conscious timer fallback on every host.
    auto loop = RenderLoop::create();
    REQUIRE(loop != nullptr);
    REQUIRE(loop->backend() == RenderLoopBackend::timer);
}

TEST_CASE("RenderLoop explicit timer factory is deterministic",
          "[render][loop][coverage][phase3-large]") {
    auto loop = RenderLoop::create_timer_loop();
    REQUIRE(loop != nullptr);
    REQUIRE_FALSE(loop->is_running());
    REQUIRE(loop->backend() == RenderLoopBackend::timer);

    std::atomic<int> frames{0};
    loop->start([&]() { frames.fetch_add(1, std::memory_order_relaxed); });
    REQUIRE(wait_for_count(frames, 1));
    loop->stop();
    REQUIRE_FALSE(loop->is_running());
}

TEST_CASE("render_loop_backend_is_vsync distinguishes real vblank sources",
          "[render][loop][vblank][slice-16]") {
    // The four native backends are real vblank sources; only the timer
    // fallback is not. constexpr so the classification is compile-checked.
    static_assert(render_loop_backend_is_vsync(RenderLoopBackend::cv_display_link));
    static_assert(render_loop_backend_is_vsync(RenderLoopBackend::ca_display_link));
    static_assert(render_loop_backend_is_vsync(RenderLoopBackend::choreographer));
    static_assert(render_loop_backend_is_vsync(RenderLoopBackend::dwm_flush));
    static_assert(!render_loop_backend_is_vsync(RenderLoopBackend::timer));

    REQUIRE(render_loop_backend_is_vsync(RenderLoopBackend::dwm_flush));
    REQUIRE_FALSE(render_loop_backend_is_vsync(RenderLoopBackend::timer));
}

TEST_CASE("render_loop_backend_name covers every backend",
          "[render][loop][vblank][slice-16]") {
    CHECK(std::string(render_loop_backend_name(RenderLoopBackend::cv_display_link))
          == "CVDisplayLink");
    CHECK(std::string(render_loop_backend_name(RenderLoopBackend::ca_display_link))
          == "CADisplayLink");
    CHECK(std::string(render_loop_backend_name(RenderLoopBackend::choreographer))
          == "AChoreographer");
    CHECK(std::string(render_loop_backend_name(RenderLoopBackend::dwm_flush))
          == "DwmFlush");
    CHECK(std::string(render_loop_backend_name(RenderLoopBackend::timer))
          == "timer");
}

TEST_CASE("DwmBackendTracker latches to the timer fallback once a vblank wait fails",
          "[render][loop][vblank][slice-16][issue-2580]") {
    // Codex P2 #2580: the Windows loop sleeps on a ~60 Hz timer when
    // DwmFlush() fails, but backend() must then stop claiming dwm_flush so
    // render_loop_backend_is_vsync() callers can detect the fallback.
    DwmBackendTracker tracker;

    // Fresh tracker is optimistic: the real vblank backend.
    REQUIRE(tracker.effective_backend() == RenderLoopBackend::dwm_flush);
    REQUIRE(render_loop_backend_is_vsync(tracker.effective_backend()));

    // Successful waits keep it on the vblank backend.
    tracker.note_wait_result(true);
    tracker.note_wait_result(true);
    REQUIRE(tracker.effective_backend() == RenderLoopBackend::dwm_flush);

    // A single failure latches the timer fallback.
    tracker.note_wait_result(false);
    REQUIRE(tracker.effective_backend() == RenderLoopBackend::timer);
    REQUIRE_FALSE(render_loop_backend_is_vsync(tracker.effective_backend()));

    // The latch is sticky: a later successful wait does not flip it back.
    tracker.note_wait_result(true);
    REQUIRE(tracker.effective_backend() == RenderLoopBackend::timer);
}

TEST_CASE("RenderLoop coalesces a burst of frame requests into one callback",
          "[render][loop][vblank][slice-16]") {
    // The canonical safe-repaint contract: any number of request_frame()
    // calls between two vblanks fire the callback exactly once. This is the
    // platform-agnostic coalescing the native vsync subclasses inherit via
    // RenderLoopState — exercised here through the timer backend.
    auto loop = RenderLoop::create();
    REQUIRE(loop != nullptr);

    std::atomic<int> frames{0};
    loop->start([&]() { frames.fetch_add(1, std::memory_order_relaxed); });

    // start() arms the initial frame.
    REQUIRE(wait_for_count(frames, 1));

    // Burst of dirty marks before the next frame interval elapses.
    for (int i = 0; i < 32; ++i) {
        loop->request_frame();
    }

    // The burst coalesces: exactly one more callback, not 32.
    REQUIRE(wait_for_count(frames, 2));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    REQUIRE(frames.load(std::memory_order_relaxed) == 2);

    loop->stop();
}

TEST_CASE("RenderLoop stays idle with no frame request",
          "[render][loop][vblank][slice-16]") {
    // Demand-driven: after the initial start() frame, an untouched loop
    // must not free-run. This is what makes vblank-locked repaint cheap
    // for an idle UI versus the legacy periodic-poll pattern.
    auto loop = RenderLoop::create();
    REQUIRE(loop != nullptr);

    std::atomic<int> frames{0};
    loop->start([&]() { frames.fetch_add(1, std::memory_order_relaxed); });

    REQUIRE(wait_for_count(frames, 1));      // initial frame only
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    REQUIRE(frames.load(std::memory_order_relaxed) == 1);

    loop->stop();
}

TEST_CASE("RenderLoop request_frame from inside the callback arms the next frame",
          "[render][loop][vblank][slice-16]") {
    // A callback that re-arms (e.g. an animating widget) drives a steady
    // one-callback-per-vblank cadence — the next-frame scheduling path —
    // without ever recursing within the same frame.
    auto loop = RenderLoop::create();
    REQUIRE(loop != nullptr);

    std::atomic<int> frames{0};
    loop->start([&]() {
        if (frames.fetch_add(1, std::memory_order_relaxed) < 4) {
            loop->request_frame();  // schedule the next frame, not a recursion
        }
    });

    REQUIRE(wait_for_count(frames, 5));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    // The callback stopped re-arming after frame 5 — the loop goes idle.
    REQUIRE(frames.load(std::memory_order_relaxed) == 5);

    loop->stop();
}

TEST_CASE("RenderLoop stop suppresses callbacks for a pending request",
          "[render][loop][vblank][slice-16]") {
    auto loop = RenderLoop::create();
    REQUIRE(loop != nullptr);

    std::atomic<int> frames{0};
    loop->start([&]() { frames.fetch_add(1, std::memory_order_relaxed); });
    REQUIRE(wait_for_count(frames, 1));

    // Arm a request and immediately stop — the pending dirty flag must be
    // cleared by stop() so no further callback fires.
    loop->request_frame();
    loop->stop();
    REQUIRE_FALSE(loop->is_running());

    const int after_stop = frames.load(std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    REQUIRE(frames.load(std::memory_order_relaxed) == after_stop);
}
