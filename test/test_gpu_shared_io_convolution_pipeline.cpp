#include "detail/shared_io_convolution_pipeline.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>

namespace {
using P = pulp::gpu_audio::detail::SharedIoConvolutionPipeline;

constexpr std::array<float, 2> input{1.f, 2.f};
constexpr std::array<float, 2> zero{};

std::array<float, 16> terminal(float a, float b, float c = 0.f, float d = 0.f) {
    return {a, 0.f, b, 0.f, c, 0.f, d, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
}

void prepare(P& pipeline, std::uint32_t ir_length = 7) {
    REQUIRE(pipeline.prepare(
        {.capacity = 8, .channels = 1, .block_size = 2, .fft_size = 8, .ir_length = ir_length}, 7));
}

P::Callback callback(P& pipeline, std::array<float, 2>& output) {
    const auto record = pipeline.begin_callback(input);
    REQUIRE(record.valid());
    pipeline.consume_output(record, output);
    return record;
}
} // namespace

TEST_CASE("shared convolution pipeline shares stamped sequence through callback and OLA",
          "[gpu_audio][shared_io][pipeline]") {
    P pipeline;
    prepare(pipeline);
    // q = ceil((7 - 1) / 2) = 3. The bridge lead only delays consumption;
    // full-tail recovery still needs three CPU-fallback blocks after reset.
    CHECK(pipeline.valid_from_sequence() == 3);

    std::array<float, 2> output;
    const auto first_callback = callback(pipeline, output);
    CHECK(first_callback.stamp.sequence == 0);
    CHECK(output == zero);
    auto ingress = pipeline.acquire_input();
    REQUIRE(ingress);
    CHECK(ingress->stamp() == P::Stamp{7, 0});
    CHECK(std::equal(ingress->samples().begin(), ingress->samples().end(), input.begin()));
    REQUIRE(pipeline.release_input(*ingress));

    const auto first = terminal(1, 2, 3);
    REQUIRE(pipeline.record_terminal({7, 0}, P::Terminal::Success, first));
    REQUIRE(pipeline.drain_terminals() == 1);

    CHECK(callback(pipeline, output).stamp.sequence == 1);
    CHECK(output == zero);
    const auto missed = callback(pipeline, output);
    CHECK(missed.stamp.sequence == 2);
    CHECK(output == zero); // sequence zero was intentionally CPU-fallback territory.

    REQUIRE(pipeline.record_terminal({7, 1}, P::Terminal::Success, terminal(4, 5)));
    REQUIRE(pipeline.drain_terminals() == 1);
    REQUIRE(pipeline.record_terminal({7, 2}, P::Terminal::Success, terminal(6, 7)));
    REQUIRE(pipeline.drain_terminals() == 1);
    REQUIRE(pipeline.record_terminal({7, 3}, P::Terminal::Success, terminal(8, 9)));
    REQUIRE(pipeline.drain_terminals() == 1);
    CHECK(callback(pipeline, output).stamp.sequence == 3);
    CHECK(output == zero); // sequence one was also reset-recovery territory.
    CHECK(callback(pipeline, output).stamp.sequence == 4);
    CHECK(output == zero); // sequence two remains reset-recovery territory.
    const auto delivered = callback(pipeline, output);
    CHECK(delivered.stamp.sequence == 5);
    CHECK(output == std::array<float, 2>{8.f, 9.f});
}

TEST_CASE("shared convolution pipeline mutation control catches a missing recovery block",
          "[gpu_audio][shared_io][pipeline][mutation-control]") {
    P pipeline;
    prepare(pipeline);
    REQUIRE(pipeline.record_terminal({7, 0}, P::Terminal::Success, terminal(1, 2, 3)));
    REQUIRE(pipeline.drain_terminals() == 1);
    REQUIRE(pipeline.record_terminal({7, 1}, P::Terminal::Success, terminal(4, 5)));
    REQUIRE(pipeline.drain_terminals() == 1);
    REQUIRE(pipeline.record_terminal({7, 2}, P::Terminal::Success, terminal(6, 7)));
    REQUIRE(pipeline.drain_terminals() == 1);
    REQUIRE(pipeline.record_terminal({7, 3}, P::Terminal::Success, terminal(8, 9)));
    REQUIRE(pipeline.drain_terminals() == 1);

    std::array<float, 2> output;
    callback(pipeline, output); // bridge priming block zero
    callback(pipeline, output); // bridge priming block one
    callback(pipeline, output); // sequence zero: reset recovery
    callback(pipeline, output); // sequence one: reset recovery
    callback(pipeline, output); // sequence two: reset recovery
    const auto record = pipeline.begin_callback(input);
    REQUIRE(record.stamp.sequence == 5);
    REQUIRE(pipeline.consume_output(record, output) == P::Delivery::Ready);
    REQUIRE(output == std::array<float, 2>{8.f, 9.f});
    // Changing q back to q - lead publishes a reset-corrupted record at
    // callback sequence three and makes this control red.
}

TEST_CASE("shared convolution pipeline fences failed history into CPU-only recovery",
          "[gpu_audio][shared_io][pipeline]") {
    P pipeline;
    prepare(pipeline, 1);
    REQUIRE(pipeline.record_terminal({7, 0}, P::Terminal::Failed, {}));
    REQUIRE(pipeline.drain_terminals() == 1);
    CHECK(pipeline.fenced());

    std::array<float, 2> output;
    const auto cpu_only = pipeline.begin_callback(input);
    CHECK(cpu_only.stamp == P::Stamp{0, 0});
    CHECK(pipeline.consume_output(cpu_only, output) == P::Delivery::Priming);
    CHECK(output == zero);
}

TEST_CASE("shared convolution pipeline reprimes both sides at the same absolute sequence",
          "[gpu_audio][shared_io][pipeline]") {
    P pipeline;
    prepare(pipeline, 1);
    std::array<float, 2> output;
    CHECK(callback(pipeline, output).stamp == P::Stamp{7, 0});
    CHECK(callback(pipeline, output).stamp == P::Stamp{7, 1});
    REQUIRE(pipeline.fence_and_reprime(8));
    CHECK(pipeline.epoch() == 8);
    CHECK(pipeline.next_sequence() == 2);
    CHECK(pipeline.valid_from_sequence() == 2);
    CHECK_FALSE(pipeline.record_terminal({7, 2}, P::Terminal::Success, terminal(1, 2)));
    REQUIRE(pipeline.record_terminal({8, 2}, P::Terminal::Success, terminal(1, 2)));
    REQUIRE(pipeline.drain_terminals() == 1);
    const auto fresh = callback(pipeline, output);
    CHECK(fresh.stamp == P::Stamp{8, 2});
}

TEST_CASE("shared convolution pipeline callback path allocates nothing after preparation",
          "[gpu_audio][shared_io][pipeline][rt-safety]") {
    P pipeline;
    prepare(pipeline, 1);
    std::array<float, 2> output;
    std::size_t allocations = 1;
    {
        pulp::test::RtAllocationProbe probe;
        for (std::uint64_t sequence = 0; sequence < 16; ++sequence) {
            const auto record = pipeline.begin_callback(input);
            REQUIRE(record.valid());
            pipeline.consume_output(record, output);
        }
        allocations = probe.allocation_count();
    }
    CHECK(allocations == 0);
}

TEST_CASE("shared convolution pipeline separates callback watermark from worker OLA",
          "[gpu_audio][shared_io][pipeline][thread]") {
    P pipeline;
    // Capacity is deliberately larger than the finite concurrent run so this
    // exercises the callback/worker ownership boundary rather than saturation.
    constexpr std::uint64_t count = 1024;
    REQUIRE(pipeline.prepare({.capacity = static_cast<std::uint32_t>(count + 2u),
                              .channels = 1,
                              .block_size = 2,
                              .fft_size = 8,
                              .ir_length = 1},
                             7));
    std::atomic<bool> callback_done{false}, worker_ok{true};
    std::thread worker([&] {
        for (;;) {
            auto ingress = pipeline.acquire_input();
            if (!ingress) {
                if (callback_done.load(std::memory_order_acquire))
                    break;
                std::this_thread::yield();
                continue;
            }
            const auto stamp = ingress->stamp();
            const auto sample = static_cast<float>(stamp.sequence);
            worker_ok.store(pipeline.release_input(*ingress) &&
                                pipeline.record_terminal(stamp, P::Terminal::Success,
                                                         terminal(sample, sample + 0.25f)) &&
                                pipeline.drain_terminals() == 1,
                            std::memory_order_release);
            if (!worker_ok.load(std::memory_order_acquire))
                return;
        }
    });

    std::array<float, 2> output;
    bool callback_ok = true;
    for (std::uint64_t sequence = 0; sequence < count; ++sequence) {
        const auto record = pipeline.begin_callback(input);
        callback_ok = callback_ok && record.valid() && record.stamp.sequence == sequence;
        pipeline.consume_output(record, output);
    }
    callback_done.store(true, std::memory_order_release);
    worker.join();
    CHECK(callback_ok);
    CHECK(worker_ok.load(std::memory_order_acquire));
}
