#include <catch2/catch_test_macros.hpp>

#include <pulp/render/render_loop.hpp>

#include "render_loop_state.hpp"

#include <atomic>
#include <chrono>
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
