#include "detail/shared_io_convolution_executor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <limits>
#include <thread>

using pulp::gpu_audio::detail::SharedIoConvolutionExecutor;
using E = SharedIoConvolutionExecutor;

static void prepare(E& executor, std::uint64_t epoch = 7, std::uint64_t first = 0) {
    REQUIRE(executor.prepare(
        {.capacity = 3, .channels = 1, .block = 2, .fft_size = 4, .ir_length = 3}, epoch, first));
}
static std::array<float, 8> frame(float a, float b, float c = 0, float d = 0) {
    return {a, 0, b, 0, c, 0, d, 0};
}

TEST_CASE("shared convolution executor advances OLA in exact sequence",
          "[gpu_audio][shared_io][executor]") {
    E executor;
    prepare(executor);
    auto a = frame(1, 2, 3);
    auto b = frame(4, 5);
    REQUIRE(executor.record_terminal(7, 0, E::Terminal::Success, a));
    REQUIRE(executor.record_terminal(7, 1, E::Terminal::Success, b));
    CHECK(executor.collect() == 2);
    auto out = executor.take_ready(1);
    REQUIRE(out.size() == 2);
    CHECK(out[0] == 7);
    CHECK(out[1] == 5);
    CHECK(executor.release_ready(1));
}

TEST_CASE("late success is suppressed only after it advances shared OLA",
          "[gpu_audio][shared_io][executor]") {
    E executor;
    prepare(executor);
    executor.advance_callback_watermark(0);
    auto a = frame(1, 2, 3);
    auto b = frame(4, 5);
    REQUIRE(executor.record_terminal(7, 0, E::Terminal::Success, a));
    CHECK(executor.collect() == 1);
    CHECK(executor.take_ready(0).empty());
    REQUIRE(executor.record_terminal(7, 1, E::Terminal::Success, b));
    CHECK(executor.collect() == 1);
    auto out = executor.take_ready(1);
    REQUIRE(out.size() == 2);
    CHECK(out[0] == 7);
    CHECK(out[1] == 5);
    CHECK(executor.release_ready(1));
}

TEST_CASE("delayed head prevents successor OLA and bounded saturation rejects overflow",
          "[gpu_audio][shared_io][executor]") {
    E executor;
    prepare(executor);
    auto x = frame(1, 1);
    REQUIRE(executor.record_terminal(7, 1, E::Terminal::Success, x));
    CHECK(executor.collect() == 0);
    CHECK_FALSE(executor.record_terminal(7, 3, E::Terminal::Success, x));
    REQUIRE(executor.record_terminal(7, 0, E::Terminal::Success, x));
    CHECK(executor.collect() == 2);
}

TEST_CASE("failure loss gaps and stale epoch fence then reprime exactly",
          "[gpu_audio][shared_io][executor]") {
    E executor;
    prepare(executor);
    auto x = frame(1, 1);
    CHECK_FALSE(executor.record_terminal(6, 0, E::Terminal::Success, x));
    REQUIRE(executor.record_terminal(7, 0, E::Terminal::Failed, {}));
    CHECK(executor.collect() == 1);
    CHECK(executor.fenced());
    CHECK_FALSE(executor.record_terminal(7, 1, E::Terminal::Success, x));
    REQUIRE(executor.fence_and_reprime(8, 10));
    CHECK_FALSE(executor.fenced());
    CHECK(executor.valid_from_sequence() == 11);
    CHECK_FALSE(executor.record_terminal(7, 10, E::Terminal::Success, x));
    REQUIRE(executor.record_terminal(8, 10, E::Terminal::Success, x));
    REQUIRE(executor.record_terminal(8, 11, E::Terminal::Success, x));
    CHECK(executor.collect() == 2);
    CHECK(executor.take_ready(10).empty());
    CHECK(executor.take_ready(11).size() == 2);
    CHECK(executor.release_ready(11));
}

TEST_CASE("nonfinite terminal history fails closed before OLA carry mutation",
          "[gpu_audio][shared_io][executor]") {
    for (const float nonfinite :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        E executor;
        prepare(executor);
        auto first = frame(1, 2, 3);
        REQUIRE(executor.record_terminal(7, 0, E::Terminal::Success, first));
        REQUIRE(executor.collect() == 1);

        auto poisoned = frame(4, 5);
        poisoned[0] = nonfinite;
        REQUIRE(executor.record_terminal(7, 1, E::Terminal::Success, poisoned));
        CHECK(executor.collect() == 1);
        CHECK(executor.fenced());
        CHECK(executor.next_sequence() == 2);
        CHECK(executor.take_ready(1).empty());

        REQUIRE(executor.fence_and_reprime(8, 10));
        auto clean = frame(4, 5);
        REQUIRE(executor.record_terminal(8, 10, E::Terminal::Success, first));
        REQUIRE(executor.record_terminal(8, 11, E::Terminal::Success, clean));
        REQUIRE(executor.collect() == 2);
        const auto ready = executor.take_ready(11);
        REQUIRE(ready.size() == 2);
        CHECK(ready[0] == 7);
        CHECK(ready[1] == 5);
        CHECK(executor.release_ready(11));
    }
}

TEST_CASE("ready output ownership publishes safely to callback",
          "[gpu_audio][shared_io][executor]") {
    E executor;
    prepare(executor, 9, 100);
    auto x = frame(1, 2);
    std::atomic<bool> published{false}, producer_ok{true}, callback_ok{true};
    std::thread producer([&] {
        producer_ok.store(executor.record_terminal(9, 100, E::Terminal::Success, x) &&
                              executor.record_terminal(9, 101, E::Terminal::Success, x) &&
                              executor.collect() == 2,
                          std::memory_order_release);
        published.store(true, std::memory_order_release);
    });
    std::thread callback([&] {
        while (!published.load(std::memory_order_acquire)) {
        }
        auto out = executor.take_ready(101);
        callback_ok.store(out.size() == 2 && out[0] == 1 && out[1] == 2 &&
                              executor.release_ready(101) && !executor.release_ready(101),
                          std::memory_order_release);
    });
    producer.join();
    callback.join();
    CHECK(producer_ok.load(std::memory_order_acquire));
    CHECK(callback_ok.load(std::memory_order_acquire));
}

TEST_CASE("reprime refuses a callback-held ready lease", "[gpu_audio][shared_io][executor]") {
    E executor;
    prepare(executor);
    auto x = frame(1, 2);
    REQUIRE(executor.record_terminal(7, 0, E::Terminal::Success, x));
    REQUIRE(executor.record_terminal(7, 1, E::Terminal::Success, x));
    REQUIRE(executor.collect() == 2);
    auto held = executor.take_ready(1);
    REQUIRE(held.size() == 2);
    CHECK_FALSE(executor.fence_and_reprime(8, 10));
    CHECK(executor.fenced());
    CHECK(held[0] == 1);
    REQUIRE(executor.release_ready(1));
    REQUIRE(executor.fence_and_reprime(8, 10));
    CHECK_FALSE(executor.fenced());
}

TEST_CASE("prepare is one-shot and cannot invalidate a callback-held ready lease",
          "[gpu_audio][shared_io][executor]") {
    E executor;
    prepare(executor);
    auto x = frame(1, 2);
    REQUIRE(executor.record_terminal(7, 0, E::Terminal::Success, x));
    REQUIRE(executor.record_terminal(7, 1, E::Terminal::Success, x));
    REQUIRE(executor.collect() == 2);
    auto held = executor.take_ready(1);
    REQUIRE(held.size() == 2);
    CHECK_FALSE(executor.prepare(
        {.capacity = 4, .channels = 1, .block = 2, .fft_size = 4, .ir_length = 3}, 8, 10));
    CHECK(held[0] == 1);
    CHECK(held[1] == 2);
    CHECK(executor.release_ready(1));
}

TEST_CASE("prepare rejects convolution geometry that would truncate overlap",
          "[gpu_audio][shared_io][executor]") {
    E executor;
    CHECK_FALSE(executor.prepare(
        {.capacity = 2, .channels = 1, .block = 4, .fft_size = 6, .ir_length = 4}, 7, 0));
    REQUIRE(executor.prepare(
        {.capacity = 2, .channels = 1, .block = 4, .fft_size = 7, .ir_length = 4}, 7, 0));
}

TEST_CASE("reprime after callback stop rejects every prior-epoch delivery",
          "[gpu_audio][shared_io][executor]") {
    E executor;
    prepare(executor);
    auto x = frame(1, 2);
    REQUIRE(executor.record_terminal(7, 0, E::Terminal::Success, x));
    REQUIRE(executor.record_terminal(7, 1, E::Terminal::Success, x));
    REQUIRE(executor.collect() == 2);
    auto ready = executor.take_ready(1);
    REQUIRE(ready.size() == 2);
    REQUIRE(executor.release_ready(1));

    // The owner has stopped and joined callback invocation before this call;
    // no callback may already be between the fence load and ready-state CAS.
    REQUIRE(executor.fence_and_reprime(8, 10));
    CHECK(executor.take_ready(1).empty());
    CHECK_FALSE(executor.record_terminal(7, 10, E::Terminal::Success, x));
}
