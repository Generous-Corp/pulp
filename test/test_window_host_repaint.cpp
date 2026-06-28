// WindowHost::mark_dirty() VBlank-locked safe-repaint routing.
//
// mark_dirty() is the consumer entry point for "something changed, redraw".
// When a RenderLoop is attached it must coalesce every call between two
// vblanks into a single request_frame(); with no loop attached it must
// degrade to a direct repaint() so existing callers keep working.
//
// These tests use a headless WindowHost subclass and the explicit
// timer-backed RenderLoop (RenderLoop::create_timer_loop()), which drives
// its callback from a worker thread and needs no native run loop — so the
// platform-agnostic routing is exercised deterministically without a
// window. (RenderLoop::create() on macOS returns the CVDisplayLink backend
// whose callback dispatches to the main run loop; that never pumps in a
// headless Catch2 process, hence the explicit timer factory here.)

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/window_host.hpp>
#include <pulp/view/view.hpp>
#include <pulp/render/render_loop.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace pulp::view;

namespace {

// Minimal concrete WindowHost — counts direct repaint() calls.
class CountingWindowHost : public WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return false; }
    void repaint() override {
        repaints_.fetch_add(1, std::memory_order_relaxed);
    }
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}

    int repaint_count() const {
        return repaints_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<int> repaints_{0};
};

bool wait_for(std::atomic<int>& v, int target) {
    for (int i = 0; i < 200; ++i) {
        if (v.load(std::memory_order_relaxed) >= target) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return v.load(std::memory_order_relaxed) >= target;
}

} // namespace

TEST_CASE("WindowHost::mark_dirty falls through to repaint with no RenderLoop",
          "[view][window-host][vblank]") {
    CountingWindowHost host;
    REQUIRE(host.render_loop() == nullptr);

    host.mark_dirty();
    host.mark_dirty();
    host.mark_dirty();

    // No vblank loop attached — each mark_dirty() is a direct repaint().
    REQUIRE(host.repaint_count() == 3);
}

TEST_CASE("WindowHost::mark_dirty routes through an attached RenderLoop",
          "[view][window-host][vblank]") {
    CountingWindowHost host;
    auto loop = pulp::render::RenderLoop::create_timer_loop();
    REQUIRE(loop != nullptr);

    std::atomic<int> frames{0};
    loop->start([&]() { frames.fetch_add(1, std::memory_order_relaxed); });

    host.attach_render_loop(loop.get());
    REQUIRE(host.render_loop() == loop.get());

    // Let the initial start() frame land first — otherwise it coalesces
    // with the burst below into a single pending request (which is itself
    // correct coalescing, just not what this case is measuring).
    REQUIRE(wait_for(frames, 1));

    // A burst of mark_dirty() calls in one frame must coalesce to a single
    // vsync-paced repaint — they are NOT direct repaint() calls anymore.
    for (int i = 0; i < 16; ++i) {
        host.mark_dirty();
    }

    REQUIRE(wait_for(frames, 2));  // the 16-call burst → exactly one frame
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(frames.load(std::memory_order_relaxed) == 2);

    // mark_dirty() went through the loop, not the host's direct repaint().
    CHECK(host.repaint_count() == 0);

    loop->stop();
    host.attach_render_loop(nullptr);
}

TEST_CASE("WindowHost::mark_dirty falls through to repaint when the attached loop is not running",
          "[view][window-host][vblank][issue-2580]") {
    // A loop attached but not running has a no-op request_frame(), so
    // routing through it would silently drop the dirty update and freeze the UI.
    // mark_dirty() must guard on is_running().
    CountingWindowHost host;
    auto loop = pulp::render::RenderLoop::create_timer_loop();
    REQUIRE(loop != nullptr);
    REQUIRE_FALSE(loop->is_running());     // attached below, never started

    host.attach_render_loop(loop.get());
    REQUIRE(host.render_loop() == loop.get());

    // Attached but NOT running → must fall through to direct repaint().
    host.mark_dirty();
    host.mark_dirty();
    CHECK(host.repaint_count() == 2);

    // Once started, routing switches to the loop — no new direct repaints.
    std::atomic<int> frames{0};
    loop->start([&]() { frames.fetch_add(1, std::memory_order_relaxed); });
    REQUIRE(wait_for(frames, 1));          // initial start() frame
    host.mark_dirty();
    REQUIRE(wait_for(frames, 2));
    CHECK(host.repaint_count() == 2);      // still 2 — went through the loop

    // Stopping the loop while still attached reverts to direct repaint().
    loop->stop();
    host.mark_dirty();
    CHECK(host.repaint_count() == 3);

    host.attach_render_loop(nullptr);
}

TEST_CASE("WindowHost::mark_dirty reverts to repaint after the loop detaches",
          "[view][window-host][vblank]") {
    CountingWindowHost host;
    auto loop = pulp::render::RenderLoop::create_timer_loop();
    REQUIRE(loop != nullptr);
    loop->start([]() {});

    host.attach_render_loop(loop.get());
    host.mark_dirty();                 // → loop->request_frame()
    CHECK(host.repaint_count() == 0);

    host.attach_render_loop(nullptr);  // detach
    host.mark_dirty();                 // → direct repaint() again
    host.mark_dirty();
    CHECK(host.repaint_count() == 2);

    loop->stop();
}

TEST_CASE("View::request_repaint drives the host's vblank dirty path",
          "[view][window-host][vblank]") {
    // request_repaint() is the in-tree consumer of the safe-repaint
    // pattern: a widget marks itself dirty and the host coalesces to one
    // repaint on the next vsync instead of the legacy poll-on-a-timer.
    CountingWindowHost host;
    auto loop = pulp::render::RenderLoop::create_timer_loop();
    REQUIRE(loop != nullptr);

    std::atomic<int> frames{0};
    loop->start([&]() { frames.fetch_add(1, std::memory_order_relaxed); });
    host.attach_render_loop(loop.get());

    pulp::view::View root;
    root.set_window_host(&host);

    // Drain the initial start() frame before measuring the burst.
    REQUIRE(wait_for(frames, 1));

    for (int i = 0; i < 10; ++i) {
        root.request_repaint();
    }

    REQUIRE(wait_for(frames, 2));  // 10 request_repaint() → one coalesced frame
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(frames.load(std::memory_order_relaxed) == 2);
    CHECK(host.repaint_count() == 0);

    root.set_window_host(nullptr);
    loop->stop();
    host.attach_render_loop(nullptr);
}
