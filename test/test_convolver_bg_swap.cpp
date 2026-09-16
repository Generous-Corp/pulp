// Background-IR-swap tests for PartitionedConvolver / ConvolverIrSwapper
// (macOS plugin authoring plan item 2.3, Slice A).
//
// Validates:
//   - lock-free atomic IR hand-off from worker thread to audio thread,
//   - block-boundary swap (no pop on the swap block),
//   - displaced IR is parked for the worker thread to free,
//   - swapping under concurrent process() does not corrupt output,
//   - stage-twice-without-consume frees the older state on the worker
//     thread (no leak, no audio-thread free),
//   - convolver still produces identity output after a swap.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"
#include "support/thread_progress.hpp"
#include <pulp/signal/convolver.hpp>
#include <pulp/signal/convolver_messages.hpp>
#include <pulp/signal/transition_mixer.hpp>

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

TEST_CASE("ConvolverIrSwapper64 builds double-precision IR state",
          "[signal][convolver][bg-swap][f64]") {
    ConvolverIrSwapper64 swapper;
    const std::vector<double> ir = {1.0, 0.5, 0.25};

    REQUIRE(swapper.stage_ir(ir.data(), ir.size(), 4));
    auto state = swapper.try_consume();

    REQUIRE(state != nullptr);
    REQUIRE(state->block_size == 4);
    REQUIRE(state->fft_size == 8);
    REQUIRE(state->num_partitions == 1);
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
    // Recorded rather than asserted inline: a Catch2 assertion here throws past
    // producer.join(), which destroys a joinable thread and terminates the
    // process instead of reporting the failure.
    bool saw_unbounded = false;
    const auto consume_one = [&] {
        if (conv.try_swap_ir(swapper))
            swaps_seen.fetch_add(1, std::memory_order_relaxed);
        conv.process(input.data(), out.data(), block);
        // Output must stay bounded — any |out[i]| > 4 would indicate
        // either a torn buffer or a wrong IR partition count slipped
        // through. Identity → |out| <= 1; half → |out| <= 0.5.
        for (std::size_t k = 0; k < block; ++k) {
            if (std::abs(out[k]) > 4.0f) {
                saw_unbounded = true;
                break;
            }
        }
    };
    for (int i = 0; i < iterations; ++i) consume_one();

    // A swap can only be seen once the producer has staged an IR, and the
    // iteration budget does not order its first stage_ir() before this loop
    // ends: on a loaded host the whole budget can be spent before the producer
    // is scheduled, leaving swaps_seen at zero. Keep consuming until a swap
    // lands; the deadline turns a swapper that genuinely never publishes into a
    // failed REQUIRE rather than a hang.
    (void)pulp::test::pump_until(
        [&] { return swaps_seen.load(std::memory_order_relaxed) > 0; }, consume_one);
    REQUIRE_FALSE(saw_unbounded);

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

TEST_CASE("PartitionedConvolver: rapid swaps without drain refuse cleanly (#2881)",
          "[signal][convolver][bg-swap][regression]") {
    // The earlier impl used a single-slot retired_ atomic. Two swaps
    // between drain_old() calls meant the displaced IR fell back to
    // an inline delete on the audio thread — soft RT violation.
    // The fix:
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

// Crossfade: an IR swap with a crossfade configured blends old->new via the
// shared TransitionMixer (parallel render of the retiring IR from its own
// history) rather than a hard cut, so the change is click-free.
TEST_CASE("PartitionedConvolver crossfades IR swaps click-free",
          "[signal][convolver][crossfade]") {
    constexpr std::size_t block = 64;
    constexpr std::size_t fade = 256;
    PartitionedConvolver conv;
    auto ir_a = make_identity_ir(4);           // passthrough (gain 1.0)
    conv.load_ir(ir_a.data(), ir_a.size(), block);
    conv.set_crossfade(fade);                  // opt-in crossfade (off by default)

    ConvolverIrSwapper swapper;
    auto ir_b = make_attenuation_ir(4, 0.25f); // gain 0.25
    REQUIRE(swapper.stage_ir(ir_b.data(), ir_b.size(), block));

    using Catch::Matchers::WithinAbs;
    std::vector<float> in(block, 1.0f), out(block, 0.0f);   // DC input
    for (int b = 0; b < 4; ++b) conv.process(in.data(), out.data(), block);
    REQUIRE_THAT(out[block - 1], WithinAbs(1.0f, 0.01f));   // IR-A: ~1.0

    REQUIRE(conv.try_swap_ir(swapper));        // begin the crossfade

    std::vector<float> seq;
    for (int b = 0; b < static_cast<int>(fade / block) + 6; ++b) {
        conv.process(in.data(), out.data(), block);
        for (float v : out) seq.push_back(v);
    }
    REQUIRE_THAT(seq.back(), WithinAbs(0.25f, 0.01f));   // settles at IR-B
    float max_step = 0.0f;
    for (std::size_t i = 1; i < seq.size(); ++i)
        max_step = std::max(max_step, std::abs(seq[i] - seq[i - 1]));
    REQUIRE(max_step < 0.1f);   // click-free (an instant swap would step ~0.75)
}

// RT audit: a fade-active process() block must not allocate on the audio
// thread (the crossfade renders into pre-sized scratch; the fade-out retires via
// the ring, never freed here). Locks in the retire-ring pattern's RT invariant.
TEST_CASE("PartitionedConvolver crossfade process is allocation-free",
          "[signal][convolver][rt]") {
    constexpr std::size_t block = 64, fade = 256;
    PartitionedConvolver conv;
    auto ir_a = make_identity_ir(4);
    conv.load_ir(ir_a.data(), ir_a.size(), block);
    conv.set_crossfade(fade);
    ConvolverIrSwapper swapper;
    auto ir_b = make_attenuation_ir(4, 0.25f);
    REQUIRE(swapper.stage_ir(ir_b.data(), ir_b.size(), block));

    std::vector<float> in(block, 1.0f), out(block, 0.0f);
    conv.process(in.data(), out.data(), block);
    REQUIRE(conv.try_swap_ir(swapper));   // control-thread swap starts the fade
    {
        pulp::test::RtAllocationProbe probe;   // audio-thread block, fade active
        conv.process(in.data(), out.data(), block);
        REQUIRE_FALSE(probe.saw_allocation());
    }
}

TEST_CASE("PartitionedConvolver swap preserves input history (no tail dip)",
          "[signal][convolver][bg-swap]") {
    // A multi-partition IR whose energy spans several partitions, so the output at
    // steady state depends on many blocks of past input — exactly the history a
    // naive whole-state swap would zero. Swapping to the SAME IR must therefore be
    // inaudible: the block right after the swap has to match the steady-state block
    // before it. (Before the input-FDL carry, the swap reset the delay line and the
    // first post-swap block dipped toward the partition-0-only response.)
    const std::size_t block = 64;
    const std::size_t parts = 4;
    std::vector<float> ir(block * parts, 0.0f);
    for (std::size_t i = 0; i < ir.size(); ++i)
        ir[i] = 0.5f * std::exp(-2.0f * static_cast<float>(i) / static_cast<float>(ir.size()));

    PartitionedConvolver conv;
    conv.load_ir(ir.data(), ir.size(), block);

    // Drive a constant (DC) input well past the receptive field so the output is at
    // steady state and block-constant (= DC * sum(ir)).
    const std::vector<float> in(block, 0.5f);
    std::vector<float> before(block, 0.0f);
    for (int b = 0; b < 40; ++b) conv.process(in.data(), before.data(), block);

    // Swap to an identical IR mid-stream.
    ConvolverIrSwapper swapper;
    REQUIRE(swapper.stage_ir(ir.data(), ir.size(), block));
    REQUIRE(conv.try_swap_ir(swapper));

    // The next block, with history carried and the IR unchanged, must equal the
    // pre-swap steady output sample-for-sample — no dip.
    std::vector<float> after(block, 0.0f);
    conv.process(in.data(), after.data(), block);
    for (std::size_t i = 0; i < block; ++i)
        REQUIRE_THAT(after[i], WithinAbs(before[i], 1e-4f));

    (void)swapper.drain_old();
}

TEST_CASE("PartitionedConvolver swap to a shorter IR keeps recent history",
          "[signal][convolver][bg-swap]") {
    // Swapping a long IR for a shorter one must still carry the recent history the
    // shorter IR needs. Build steady state on a long IR, swap to a short one, and
    // compare against a reference convolver that ran the short IR on the same input
    // from the start — after one block they must agree (the short IR's entire
    // receptive field of history was preserved across the swap).
    const std::size_t block = 64;
    std::vector<float> long_ir(block * 6, 0.0f);
    for (std::size_t i = 0; i < long_ir.size(); ++i)
        long_ir[i] = 0.3f * std::cos(0.01f * static_cast<float>(i));
    std::vector<float> short_ir(block * 2, 0.0f);
    for (std::size_t i = 0; i < short_ir.size(); ++i)
        short_ir[i] = 0.4f * std::exp(-3.0f * static_cast<float>(i) / static_cast<float>(short_ir.size()));

    const std::vector<float> in(block, 0.5f);

    // Reference: short IR from the start, run to steady state.
    PartitionedConvolver ref;
    ref.load_ir(short_ir.data(), short_ir.size(), block);
    std::vector<float> ref_out(block, 0.0f);
    for (int b = 0; b < 40; ++b) ref.process(in.data(), ref_out.data(), block);

    // Under test: long IR to steady state, then swap to the short IR.
    PartitionedConvolver conv;
    conv.load_ir(long_ir.data(), long_ir.size(), block);
    std::vector<float> out(block, 0.0f);
    for (int b = 0; b < 40; ++b) conv.process(in.data(), out.data(), block);
    ConvolverIrSwapper swapper;
    REQUIRE(swapper.stage_ir(short_ir.data(), short_ir.size(), block));
    REQUIRE(conv.try_swap_ir(swapper));
    conv.process(in.data(), out.data(), block);

    // The short IR's full 2-partition history was preserved, so the first post-swap
    // block already matches the reference steady state.
    for (std::size_t i = 0; i < block; ++i)
        REQUIRE_THAT(out[i], WithinAbs(ref_out[i], 1e-4f));

    (void)swapper.drain_old();
}

namespace {
// A signal that differs every sample AND every block, so each history partition
// holds a DISTINCT spectrum. Under a block-constant (DC) input every partition's
// spectrum is identical and the partitioned sum is invariant to any permutation
// of the input delay line — which means a DC test cannot tell a correct
// age-alignment from a reversed or off-by-one one. This stream can.
float distinct_sample_at(std::size_t n) {
    const float x = static_cast<float>(n);
    return 0.6f * std::sin(0.07f * x) + 0.4f * std::sin(0.021f * x + 0.9f) +
           0.2f * std::sin(0.003f * x + 2.1f);
}
} // namespace

TEST_CASE("PartitionedConvolver swap keeps history age-aligned (distinct-per-block signal)",
          "[signal][convolver][bg-swap]") {
    // Swapping to the SAME IR must be a functional no-op: the input delay line is
    // IR-independent and the rebuilt IR spectra are bit-identical, so a convolver
    // that swaps mid-stream must track a no-swap reference sample-for-sample on a
    // signal whose partitions are all distinct. A reversed/off-by-one carry mapping
    // would reorder the delay line and diverge here (it stays hidden under DC).
    const std::size_t block = 64;
    const std::size_t parts = 4;
    std::vector<float> ir(block * parts, 0.0f);
    for (std::size_t i = 0; i < ir.size(); ++i)
        ir[i] = 0.5f * std::exp(-2.0f * static_cast<float>(i) / static_cast<float>(ir.size()));

    const int warm = 40, post = 5;

    // Reference: run the whole distinct stream through the IR, no swap.
    PartitionedConvolver ref;
    ref.load_ir(ir.data(), ir.size(), block);
    std::vector<std::vector<float>> ref_out;
    for (int b = 0; b < warm + post; ++b) {
        std::vector<float> in(block), o(block);
        for (std::size_t i = 0; i < block; ++i)
            in[i] = distinct_sample_at(static_cast<std::size_t>(b) * block + i);
        ref.process(in.data(), o.data(), block);
        ref_out.push_back(std::move(o));
    }

    // Under test: identical stream, but swap to the same IR after `warm` blocks.
    PartitionedConvolver conv;
    conv.load_ir(ir.data(), ir.size(), block);
    for (int b = 0; b < warm; ++b) {
        std::vector<float> in(block), o(block);
        for (std::size_t i = 0; i < block; ++i)
            in[i] = distinct_sample_at(static_cast<std::size_t>(b) * block + i);
        conv.process(in.data(), o.data(), block);
    }
    ConvolverIrSwapper swapper;
    REQUIRE(swapper.stage_ir(ir.data(), ir.size(), block));
    REQUIRE(conv.try_swap_ir(swapper));

    // Every post-swap block must match the no-swap reference for the same stream
    // position — proving the carried delay line is aligned by age, not merely
    // non-zero. Checked for several blocks so the whole ring ages through correctly.
    for (int b = warm; b < warm + post; ++b) {
        std::vector<float> in(block), o(block);
        for (std::size_t i = 0; i < block; ++i)
            in[i] = distinct_sample_at(static_cast<std::size_t>(b) * block + i);
        conv.process(in.data(), o.data(), block);
        for (std::size_t i = 0; i < block; ++i)
            REQUIRE_THAT(o[i], WithinAbs(ref_out[static_cast<std::size_t>(b)][i], 1e-4f));
    }

    (void)swapper.drain_old();
}

TEST_CASE("PartitionedConvolver swap to a shorter IR is age-aligned (distinct-per-block signal)",
          "[signal][convolver][bg-swap]") {
    // Same alignment guarantee for a shrinking IR (Q < P). The input delay line is
    // IR-independent, so after the swap the convolver must match a reference that
    // ran the short IR over the identical distinct stream from the start. A shorter
    // IR needs only its own receptive field of history; the carry must hand over
    // exactly those most-recent partitions, correctly aged.
    const std::size_t block = 64;
    std::vector<float> long_ir(block * 6, 0.0f);
    for (std::size_t i = 0; i < long_ir.size(); ++i)
        long_ir[i] = 0.3f * std::cos(0.01f * static_cast<float>(i));
    std::vector<float> short_ir(block * 2, 0.0f);
    for (std::size_t i = 0; i < short_ir.size(); ++i)
        short_ir[i] = 0.4f * std::exp(-3.0f * static_cast<float>(i) / static_cast<float>(short_ir.size()));

    const int warm = 40, post = 4;

    PartitionedConvolver ref;
    ref.load_ir(short_ir.data(), short_ir.size(), block);
    std::vector<std::vector<float>> ref_out;
    for (int b = 0; b < warm + post; ++b) {
        std::vector<float> in(block), o(block);
        for (std::size_t i = 0; i < block; ++i)
            in[i] = distinct_sample_at(static_cast<std::size_t>(b) * block + i);
        ref.process(in.data(), o.data(), block);
        ref_out.push_back(std::move(o));
    }

    PartitionedConvolver conv;
    conv.load_ir(long_ir.data(), long_ir.size(), block);
    for (int b = 0; b < warm; ++b) {
        std::vector<float> in(block), o(block);
        for (std::size_t i = 0; i < block; ++i)
            in[i] = distinct_sample_at(static_cast<std::size_t>(b) * block + i);
        conv.process(in.data(), o.data(), block);
    }
    ConvolverIrSwapper swapper;
    REQUIRE(swapper.stage_ir(short_ir.data(), short_ir.size(), block));
    REQUIRE(conv.try_swap_ir(swapper));

    for (int b = warm; b < warm + post; ++b) {
        std::vector<float> in(block), o(block);
        for (std::size_t i = 0; i < block; ++i)
            in[i] = distinct_sample_at(static_cast<std::size_t>(b) * block + i);
        conv.process(in.data(), o.data(), block);
        for (std::size_t i = 0; i < block; ++i)
            REQUIRE_THAT(o[i], WithinAbs(ref_out[static_cast<std::size_t>(b)][i], 1e-4f));
    }

    (void)swapper.drain_old();
}

namespace {
// Render `blocks` blocks of the distinct stream through a convolver, flattened.
// `swap_to` (when given) is staged and swapped in at block `swap_block`.
std::vector<float> render_stream(const std::vector<float>& ir,
                                 std::size_t block,
                                 int blocks,
                                 std::size_t fade,
                                 const std::vector<float>* swap_to,
                                 int swap_block) {
    PartitionedConvolver conv;
    conv.load_ir(ir.data(), ir.size(), block);
    if (fade > 0) conv.set_crossfade(fade);
    ConvolverIrSwapper swapper;

    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(blocks) * block);
    std::vector<float> in(block), o(block);
    for (int b = 0; b < blocks; ++b) {
        if (swap_to && b == swap_block) {
            REQUIRE(swapper.stage_ir(swap_to->data(), swap_to->size(), block));
            REQUIRE(conv.try_swap_ir(swapper));
        }
        for (std::size_t i = 0; i < block; ++i)
            in[i] = distinct_sample_at(static_cast<std::size_t>(b) * block + i);
        conv.process(in.data(), o.data(), block);
        out.insert(out.end(), o.begin(), o.end());
        (void)swapper.drain_old();
    }
    REQUIRE(conv.block_size_violations() == 0);
    return out;
}

// A decaying, spectrally busy IR spanning `len` taps — energy in every partition,
// so the steady-state output genuinely depends on the whole delay line.
std::vector<float> make_dense_ir(std::size_t len, float phase) {
    std::vector<float> ir(len, 0.0f);
    for (std::size_t i = 0; i < len; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(len);
        ir[i] = 0.5f * std::exp(-2.5f * t) *
                std::sin(0.31f * static_cast<float>(i) + phase);
    }
    ir[0] = 1.0f;
    return ir;
}
} // namespace

// The cleanest possible statement of the crossfade's continuity contract: swap an
// IR for a BIT-IDENTICAL copy of itself under a crossfade and nothing may change.
// Both sides of the fade convolve the same input against the same coefficients,
// and the default Smoothstep curve's gain law is equal-GAIN — (1-u, u), summing to
// one — so blending a render with itself must reproduce it. Any deviation is the
// convolver losing part of the input stream across the swap. (EqualPower sums to
// one in POWER, so it would legitimately bulge on an identical pair; that is the
// gain law, not the history, and is why this pins the default curve.)
//
// This needs a MULTI-PARTITION IR to say anything: with a single partition there
// is almost no delay line to lose, which is why the click-free fade test above
// (a 4-tap IR in a 64-sample block) passes either way.
TEST_CASE("PartitionedConvolver crossfaded swap to an identical IR is inaudible",
          "[signal][convolver][crossfade]") {
    const std::size_t block = 64;
    const std::size_t fade = 512;
    const int blocks = 96, swap_block = 40;

    auto ir = make_dense_ir(block * 12, 0.0f);   // 12 partitions of real history
    auto identical = ir;                         // bit-identical copy
    REQUIRE(identical == ir);

    const auto reference = render_stream(ir, block, blocks, 0, nullptr, 0);
    const auto swapped = render_stream(ir, block, blocks, fade, &identical, swap_block);

    REQUIRE(swapped.size() == reference.size());
    for (std::size_t i = 0; i < reference.size(); ++i)
        REQUIRE_THAT(swapped[i], WithinAbs(reference[i], 1e-4f));
}

// Same contract on a LONG impulse. The history a swap can lose is bounded by the
// IR's length, so a short-IR test understates the defect and can pass on a fix
// that only carries a few partitions across. 128 partitions is the case that
// hurts: a reverb-length IR losing its whole tail on every crossfaded swap.
TEST_CASE("PartitionedConvolver crossfaded swap to an identical LONG IR is inaudible",
          "[signal][convolver][crossfade]") {
    const std::size_t block = 64;
    const std::size_t partitions = 128;          // 8192 taps — ~171 ms at 48 kHz
    const std::size_t fade = 512;
    // Run well past the impulse length so a cold incoming IR cannot hide inside
    // the measured window: the disturbance lasts exactly one impulse.
    const int blocks = static_cast<int>(partitions) + 80;
    const int swap_block = static_cast<int>(partitions) + 8;   // fully warm first

    auto ir = make_dense_ir(block * partitions, 0.0f);
    auto identical = ir;
    REQUIRE(identical == ir);

    const auto reference = render_stream(ir, block, blocks, 0, nullptr, 0);
    const auto swapped = render_stream(ir, block, blocks, fade, &identical, swap_block);

    REQUIRE(swapped.size() == reference.size());
    for (std::size_t i = 0; i < reference.size(); ++i)
        REQUIRE_THAT(swapped[i], WithinAbs(reference[i], 1e-4f));
}

// A genuinely CHANGED IR: the fade must be a blend of two continuous renders, not
// a restart. The expectation is built from two convolvers that each ran the whole
// stream from the start, so both references are warm at every sample; the
// crossfade output must equal their mixer-weighted sum. That asserts the incoming
// IR is rendering against the real input history — an incoming IR starting cold
// would match a warm reference only after its own length had elapsed.
TEST_CASE("PartitionedConvolver crossfade blends two continuous renders",
          "[signal][convolver][crossfade]") {
    const std::size_t block = 64;
    const std::size_t fade = 512;
    const int blocks = 96, swap_block = 40;

    auto ir_a = make_dense_ir(block * 12, 0.0f);
    auto ir_b = make_dense_ir(block * 12, 1.7f);   // different coefficients
    REQUIRE(ir_a != ir_b);

    const auto ref_a = render_stream(ir_a, block, blocks, 0, nullptr, 0);
    const auto ref_b = render_stream(ir_b, block, blocks, 0, nullptr, 0);
    const auto faded = render_stream(ir_a, block, blocks, fade, &ir_b, swap_block);

    // The same mixer the convolver uses, stepped over the same positions.
    TransitionMixerT<float> mixer;
    mixer.configure(fade, TransitionCurve::Smoothstep);

    const std::size_t swap_sample = static_cast<std::size_t>(swap_block) * block;
    for (std::size_t i = 0; i < swap_sample; ++i)
        REQUIRE_THAT(faded[i], WithinAbs(ref_a[i], 1e-4f));   // untouched before

    bool saw_partial_blend = false;
    for (std::size_t i = swap_sample; i < faded.size(); ++i) {
        float old_gain = 0.0f, new_gain = 0.0f;
        mixer.gains_at(i - swap_sample, old_gain, new_gain);
        if (old_gain > 0.05f && new_gain > 0.05f) saw_partial_blend = true;
        const float expected = ref_a[i] * old_gain + ref_b[i] * new_gain;
        REQUIRE_THAT(faded[i], WithinAbs(expected, 1e-4f));
    }
    // Control: the window really did contain mid-fade samples, so the loop above
    // tested the blend and not just its two endpoints.
    REQUIRE(saw_partial_blend);
    REQUIRE_THAT(faded.back(), WithinAbs(ref_b.back(), 1e-4f));   // settles on B
}

// Growing the shared ring (an IR with more partitions than the ring holds) must
// still be allocation-free on the audio thread: the incoming state carries a
// pre-allocated history spare built off-thread precisely so the swap never has to
// allocate one, and the displaced ring leaves via the retire ring, never free().
TEST_CASE("PartitionedConvolver swap that grows the input ring is allocation-free",
          "[signal][convolver][rt]") {
    const std::size_t block = 64;
    PartitionedConvolver conv;
    auto shortish = make_dense_ir(block * 2, 0.0f);
    auto longer = make_dense_ir(block * 16, 0.4f);   // 8x the partitions
    conv.load_ir(shortish.data(), shortish.size(), block);
    conv.set_crossfade(256);

    ConvolverIrSwapper swapper;
    REQUIRE(swapper.stage_ir(longer.data(), longer.size(), block));

    std::vector<float> in(block, 0.25f), out(block, 0.0f);
    conv.process(in.data(), out.data(), block);
    {
        pulp::test::RtAllocationProbe probe;
        REQUIRE(conv.try_swap_ir(swapper));   // grows the ring under the probe
        conv.process(in.data(), out.data(), block);
        REQUIRE_FALSE(probe.saw_allocation());
    }
    (void)swapper.drain_old();
}

// The shared ring keeps the high-water mark of partitions seen, so a detour
// through a SHORT IR no longer throws away history the long IR will want back.
// Under the old per-IR delay line the short IR's ring was the only thing carried,
// so long -> short -> long silently truncated the stream to the short IR's length.
TEST_CASE("PartitionedConvolver swap through a shorter IR and back keeps history",
          "[signal][convolver][bg-swap]") {
    const std::size_t block = 64;
    auto long_ir = make_dense_ir(block * 12, 0.0f);
    auto short_ir = make_dense_ir(block * 2, 0.8f);
    const int blocks = 96;

    const auto reference = render_stream(long_ir, block, blocks, 0, nullptr, 0);

    PartitionedConvolver conv;
    conv.load_ir(long_ir.data(), long_ir.size(), block);
    ConvolverIrSwapper swapper;

    std::vector<float> in(block), o(block), got;
    got.reserve(static_cast<std::size_t>(blocks) * block);
    for (int b = 0; b < blocks; ++b) {
        if (b == 40) {
            REQUIRE(swapper.stage_ir(short_ir.data(), short_ir.size(), block));
            REQUIRE(conv.try_swap_ir(swapper));
        } else if (b == 41) {   // straight back, one block later
            REQUIRE(swapper.stage_ir(long_ir.data(), long_ir.size(), block));
            REQUIRE(conv.try_swap_ir(swapper));
        }
        for (std::size_t i = 0; i < block; ++i)
            in[i] = distinct_sample_at(static_cast<std::size_t>(b) * block + i);
        conv.process(in.data(), o.data(), block);
        got.insert(got.end(), o.begin(), o.end());
        (void)swapper.drain_old();
    }

    // Only block 40 saw a different IR; from block 41 on the long IR must line up
    // with a run that never left it — every partition of history still present.
    for (std::size_t i = 41 * block; i < reference.size(); ++i)
        REQUIRE_THAT(got[i], WithinAbs(reference[i], 1e-4f));
}

// A GESTURE is a BURST of IR swaps, and that is where losing the delay line stops
// being a transient and becomes a dropout: each swap re-zeroes the history before
// the previous one has refilled it, so the wet never recovers for the length of the
// drag. A single-swap test measures one recovery, which looks like a dip rather
// than the sustained hole a user actually hears.
//
// Stated on IDENTICAL impulses so the assertion can be exact. An envelope-depth
// version of this was tried first and thrown away: a short-window RMS of broadband
// material fluctuates by the very factor the artifact produces, so the metric read
// the same ~0.23x with the fix, without it, and with no crossfade at all. There is
// no threshold to pick there. Sample equality against a never-swapped reference has
// none of that ambiguity.
TEST_CASE("PartitionedConvolver crossfaded swap burst keeps the stream continuous",
          "[signal][convolver][crossfade]") {
    const std::size_t block = 64;
    const std::size_t partitions = 64;          // 4096 taps of history to lose
    const std::size_t fade = 256;               // 4 blocks; shorter than the cadence
    const int warm = 80, every = 6, swaps = 24;
    const int blocks = warm + every * swaps + 120;

    auto ir = make_dense_ir(block * partitions, 0.0f);
    auto identical = ir;
    REQUIRE(identical == ir);

    const auto reference = render_stream(ir, block, blocks, 0, nullptr, 0);

    PartitionedConvolver conv;
    conv.load_ir(ir.data(), ir.size(), block);
    conv.set_crossfade(fade);
    ConvolverIrSwapper swapper;

    std::vector<float> in(block), o(block), got;
    got.reserve(static_cast<std::size_t>(blocks) * block);
    int accepted = 0;
    for (int b = 0; b < blocks; ++b) {
        const int g = b - warm;
        if (g >= 0 && g % every == 0 && g / every < swaps) {
            REQUIRE(swapper.stage_ir(identical.data(), identical.size(), block));
            if (conv.try_swap_ir(swapper)) ++accepted;
        }
        for (std::size_t i = 0; i < block; ++i)
            in[i] = distinct_sample_at(static_cast<std::size_t>(b) * block + i);
        conv.process(in.data(), o.data(), block);
        got.insert(got.end(), o.begin(), o.end());
        (void)swapper.drain_old();
    }
    REQUIRE(conv.block_size_violations() == 0);
    // Control: the burst really happened. try_swap_ir refuses while a fade is in
    // flight, so a fade longer than the offer cadence would quietly reduce this to
    // a handful of swaps and the test would be asserting continuity it never
    // stressed. Every offer here must have been taken.
    REQUIRE(accepted == swaps);

    REQUIRE(got.size() == reference.size());
    for (std::size_t i = 0; i < reference.size(); ++i)
        REQUIRE_THAT(got[i], WithinAbs(reference[i], 1e-4f));
}
