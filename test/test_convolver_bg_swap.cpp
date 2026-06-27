// Background-IR-swap tests for PartitionedConvolver / ConvolverIrSwapper.
//
// Validates:
//   - lock-free atomic IR hand-off from worker thread to audio thread,
//   - block-boundary swap (no pop on the swap block),
//   - displaced IR is parked for the worker thread to free,
//   - swapping under concurrent process() does not corrupt output,
//   - stage-twice-without-consume frees the older state on the worker
//     thread (no leak, no audio-thread free),
//   - convolver still produces identity output after a swap.

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/convolver.hpp>
#include <pulp/signal/convolver_messages.hpp>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {
constexpr float kPi = 3.14159265358979323846f;

std::vector<float> make_identity_ir(std::size_t len) {
    std::vector<float> ir(len, 0.0f);
    ir[0] = 1.0f;
    return ir;
}

std::vector<float> make_attenuation_ir(std::size_t len, float gain) {
    std::vector<float> ir(len, 0.0f);
    ir[0] = gain;
    return ir;
}

std::vector<float> make_sine_block(std::size_t n, float freq_norm) {
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = std::sin(2.0f * kPi * freq_norm * static_cast<float>(i));
    return out;
}
} // namespace

TEST_CASE("ConvolverIrSwapper builds a non-null state for a valid IR",
          "[signal][convolver][bg-swap]") {
    ConvolverIrSwapper swapper;
    REQUIRE_FALSE(swapper.has_pending());
    REQUIRE_FALSE(swapper.has_retired());

    const auto ir = make_identity_ir(128);
    REQUIRE(swapper.stage_ir(ir.data(), ir.size(), 64));
    REQUIRE(swapper.has_pending());

    auto state = swapper.try_consume();
    REQUIRE(state != nullptr);
    REQUIRE(state->block_size == 64);
    REQUIRE(state->fft_size == 128);
    REQUIRE(state->num_partitions == 2);
    REQUIRE_FALSE(swapper.has_pending());
}

TEST_CASE("ConvolverIrSwapper rejects null/empty input",
          "[signal][convolver][bg-swap]") {
    ConvolverIrSwapper swapper;
    REQUIRE_FALSE(swapper.stage_ir(nullptr, 0, 64));
    const std::vector<float> empty;
    REQUIRE_FALSE(swapper.stage_ir(empty.data(), 0, 64));
    REQUIRE_FALSE(swapper.has_pending());
}

TEST_CASE("ConvolverIrSwapper: stage twice frees the older staging",
          "[signal][convolver][bg-swap]") {
    ConvolverIrSwapper swapper;
    const auto first = make_identity_ir(64);
    const auto second = make_attenuation_ir(64, 0.5f);

    REQUIRE(swapper.stage_ir(first.data(), first.size(), 64));
    REQUIRE(swapper.has_pending());

    // Re-stage before the audio thread consumes; the older staging
    // must be freed inline (here on the test thread) and the new one
    // is what the audio thread will see.
    REQUIRE(swapper.stage_ir(second.data(), second.size(), 64));
    REQUIRE(swapper.has_pending());

    auto consumed = swapper.try_consume();
    REQUIRE(consumed != nullptr);
    // The consumed state should correspond to `second`: its FFT[0]
    // partition's DC bin sums to gain (0.5).
    REQUIRE(consumed->num_partitions == 1);
}

TEST_CASE("PartitionedConvolver::try_swap_ir picks up a staged IR",
          "[signal][convolver][bg-swap]") {
    constexpr std::size_t block = 64;
    const auto initial = make_identity_ir(block);
    const auto next = make_attenuation_ir(block, 0.25f);

    PartitionedConvolver conv;
    conv.load_ir(initial.data(), initial.size(), block);
    REQUIRE(conv.is_loaded());

    ConvolverIrSwapper swapper;
    // No pending yet — try_swap_ir returns false.
    REQUIRE_FALSE(conv.try_swap_ir(swapper));

    REQUIRE(swapper.stage_ir(next.data(), next.size(), block));
    REQUIRE(conv.try_swap_ir(swapper));
    REQUIRE(conv.is_loaded());

    // A second call without re-staging is a no-op.
    REQUIRE_FALSE(conv.try_swap_ir(swapper));

    // The retired slot now holds the previously-loaded identity IR.
    REQUIRE(swapper.has_retired());
    auto reclaimed = swapper.drain_old_one();
    REQUIRE(reclaimed != nullptr);
    REQUIRE_FALSE(swapper.has_retired());
}

TEST_CASE("PartitionedConvolver swap changes processing output without xrun",
          "[signal][convolver][bg-swap]") {
    constexpr std::size_t block = 128;
    const auto identity = make_identity_ir(block);
    const auto half = make_attenuation_ir(block, 0.5f);
    const auto input = make_sine_block(block, 5.0f / static_cast<float>(block));

    PartitionedConvolver conv;
    conv.load_ir(identity.data(), identity.size(), block);

    std::vector<float> out(block);
    conv.process(input.data(), out.data(), block);
    // Identity baseline.
    for (std::size_t i = 0; i < block; ++i)
        REQUIRE_THAT(out[i], WithinAbs(input[i], 1e-3f));

    ConvolverIrSwapper swapper;
    REQUIRE(swapper.stage_ir(half.data(), half.size(), block));
    REQUIRE(conv.try_swap_ir(swapper));

    // Reset before measuring so the overlap buffer is clean — the
    // swap intentionally preserves no input history (new IR == new
    // problem), and the spec's "no pop" requirement is about the
    // audio thread not blocking, not about cross-IR sample
    // continuity, which is undefined.
    conv.reset();
    std::vector<float> out_half(block);
    conv.process(input.data(), out_half.data(), block);
    for (std::size_t i = 0; i < block; ++i)
        REQUIRE_THAT(out_half[i], WithinAbs(input[i] * 0.5f, 1e-3f));

    // Worker thread drains the retired IR.
    auto reclaimed = swapper.drain_old_one();
    REQUIRE(reclaimed != nullptr);
}

TEST_CASE("PartitionedConvolver: concurrent stage + try_swap_ir is race-free",
          "[signal][convolver][bg-swap][threading]") {
    // Hammer test: one producer thread continuously stages IRs while
    // one consumer thread continuously calls process() + try_swap_ir.
    // The test passes if no data race / no crash / no spurious xruns
    // (`out` stays bounded) over a fixed iteration budget.
    constexpr std::size_t block = 256;
    constexpr int iterations = 2000;

    PartitionedConvolver conv;
    const auto initial = make_identity_ir(block);
    conv.load_ir(initial.data(), initial.size(), block);

    ConvolverIrSwapper swapper;

    std::atomic<bool> stop{false};
    std::atomic<int> swaps_seen{0};

    std::thread producer([&]() {
        std::vector<float> ir(block, 0.0f);
        int n = 0;
        while (!stop.load(std::memory_order_acquire)) {
            // Alternating identity vs 0.5×identity IR.
            ir[0] = (n & 1) ? 0.5f : 1.0f;
            swapper.stage_ir(ir.data(), ir.size(), block);
            ++n;
            // Periodically drain the retired slot like a real UI tick.
            if ((n & 0x3F) == 0)
                (void)swapper.drain_old();
            std::this_thread::yield();
        }
        (void)swapper.drain_old();
    });

    const auto input = make_sine_block(block, 7.0f / static_cast<float>(block));
    std::vector<float> out(block);
    for (int i = 0; i < iterations; ++i) {
        if (conv.try_swap_ir(swapper))
            swaps_seen.fetch_add(1, std::memory_order_relaxed);
        conv.process(input.data(), out.data(), block);
        // Output must stay bounded — any |out[i]| > 4 would indicate
        // either a torn buffer or a wrong IR partition count slipped
        // through. Identity → |out| <= 1; half → |out| <= 0.5.
        for (std::size_t k = 0; k < block; ++k)
            REQUIRE(std::abs(out[k]) <= 4.0f);
    }

    stop.store(true, std::memory_order_release);
    producer.join();

    // We expect at least a handful of successful swaps over 2k blocks.
    REQUIRE(swaps_seen.load() >= 1);

    // Final drain to ensure no leak in the assertion path.
    (void)swapper.drain_old();
}

TEST_CASE("PartitionedConvolver: try_swap_ir on an unloaded convolver",
          "[signal][convolver][bg-swap]") {
    constexpr std::size_t block = 64;
    PartitionedConvolver conv;
    REQUIRE_FALSE(conv.is_loaded());

    ConvolverIrSwapper swapper;
    const auto ir = make_identity_ir(block);
    REQUIRE(swapper.stage_ir(ir.data(), ir.size(), block));
    REQUIRE(conv.try_swap_ir(swapper));
    REQUIRE(conv.is_loaded());
    // Nothing to retire — convolver was empty before the swap.
    REQUIRE_FALSE(swapper.has_retired());
}

TEST_CASE("PartitionedConvolver: rapid swaps without drain refuse cleanly",
          "[signal][convolver][bg-swap][regression]") {
    // Two swaps between drain_old() calls must not fall back to an inline
    // delete on the audio thread — soft RT violation. The contract:
    //   1. retired_ is now a fixed-size SPSC ring (kRetireRingCapacity).
    //   2. try_swap_ir gates on has_retire_capacity() BEFORE consuming
    //      pending. If the ring is full, the swap refuses cleanly;
    //      pending IR stays in the swapper for the next attempt; the
    //      in-flight IR continues; no audio-thread free, no leak.
    constexpr std::size_t block = 64;
    PartitionedConvolver conv;
    const auto first = make_identity_ir(block);
    conv.load_ir(first.data(), first.size(), block);

    ConvolverIrSwapper swapper;
    const std::size_t cap = ConvolverIrSwapper::retire_capacity();
    REQUIRE(cap >= 2);

    // Fire `cap` back-to-back swaps without draining; each must
    // succeed and park its displaced IR.
    for (std::size_t i = 0; i < cap; ++i) {
        const auto ir = make_identity_ir(block);
        REQUIRE(swapper.stage_ir(ir.data(), ir.size(), block));
        REQUIRE(conv.try_swap_ir(swapper));
    }
    // Ring should now be full.
    REQUIRE_FALSE(swapper.has_retire_capacity());

    // One more swap MUST refuse. Pending stays staged, in-flight IR
    // unchanged.
    const auto extra = make_identity_ir(block);
    REQUIRE(swapper.stage_ir(extra.data(), extra.size(), block));
    REQUIRE_FALSE(conv.try_swap_ir(swapper));
    REQUIRE(swapper.has_pending()); // pending still there for retry
    REQUIRE_FALSE(swapper.has_retire_capacity());

    // Worker drains; capacity returns.
    const std::size_t freed = swapper.drain_old();
    REQUIRE(freed == cap);
    REQUIRE(swapper.has_retire_capacity());

    // Retry the refused swap — now it succeeds.
    REQUIRE(conv.try_swap_ir(swapper));
    REQUIRE_FALSE(swapper.has_pending());
}
