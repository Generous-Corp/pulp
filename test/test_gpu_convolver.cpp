#include <catch2/catch_test_macros.hpp>

#include <pulp/gpu_audio/gpu_convolver.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/detail/gpu_ola.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <vector>

namespace {

// Direct linear convolution reference: y[n] = sum_k ir[k] * x[n-k].
std::vector<float> direct_convolution(const std::vector<float>& x,
                                      const std::vector<float>& ir) {
    std::vector<float> y(x.size(), 0.0f);
    for (std::size_t n = 0; n < x.size(); ++n) {
        double acc = 0.0;
        for (std::size_t k = 0; k < ir.size() && k <= n; ++k)
            acc += static_cast<double>(ir[k]) * x[n - k];
        y[n] = static_cast<float>(acc);
    }
    return y;
}

}  // namespace

using namespace pulp::gpu_audio;
using pulp::audio::BufferView;

// Golden test: the GPU FFT/overlap-add convolver must reproduce direct linear
// convolution of the input stream with the IR, within f32 tolerance.
TEST_CASE("GpuConvolver matches direct convolution", "[gpu_audio][convolver][gpu]") {
    constexpr uint32_t CH = 1, BS = 64, SR = 48000;
    constexpr int M = 300;  // IR length (spans several blocks)

    std::vector<float> ir(M);
    for (int i = 0; i < M; ++i) {
        ir[i] = std::cos(0.05f * i) * std::exp(-0.01f * i);
    }

    GpuConvolver node(CH, BS, SR, ir);
    REQUIRE(node.prepare());
    if (!node.gpu_available()) return;  // headless CI / no adapter → skip GPU path
    REQUIRE(node.fft_size() >= BS + M);

    constexpr int NBLK = 8;
    constexpr int total = NBLK * BS;
    std::vector<float> x(total);
    for (int i = 0; i < total; ++i) {
        x[i] = std::sin(0.1f * i) + 0.3f * std::sin(0.37f * i);
    }

    // Run the node block by block; collect output.
    std::vector<float> gpu_out(total, 0.0f);
    std::vector<float> in_block(BS), out_block(BS);
    for (int b = 0; b < NBLK; ++b) {
        for (int i = 0; i < static_cast<int>(BS); ++i) in_block[i] = x[b * BS + i];
        const float* in_ptr[1] = {in_block.data()};
        float* out_ptr[1] = {out_block.data()};
        BufferView<const float> iv(in_ptr, 1, BS);
        BufferView<float> ov(out_ptr, 1, BS);
        node.process_block(iv, ov, BS);
        for (int i = 0; i < static_cast<int>(BS); ++i) gpu_out[b * BS + i] = out_block[i];
    }

    // Reference: y[n] = sum_k ir[k] * x[n-k].
    for (int n = 0; n < total; ++n) {
        double acc = 0.0;
        for (int k = 0; k < M && k <= n; ++k) acc += static_cast<double>(ir[k]) * x[n - k];
        const float ref = static_cast<float>(acc);
        REQUIRE(std::abs(gpu_out[n] - ref) < 1e-2f * (1.0f + std::abs(ref)));
    }
}

// The node always prepares (CPU fallback) even when the IR is valid; an empty
// IR is rejected.
TEST_CASE("GpuConvolver rejects empty IR", "[gpu_audio][convolver][gpu]") {
    GpuConvolver node(1, 64, 48000, {});
    REQUIRE_FALSE(node.prepare());
}

// The no-GPU worker path is a zero-latency partitioned convolution: process_block
// (which drives worker_fallback_ when there is no device) must reproduce direct
// linear convolution of the stream, block-aligned (no one-block streaming lag).
TEST_CASE("GpuConvolver process_block uses CPU fallback without a GPU",
          "[gpu_audio][convolver]") {
    constexpr uint32_t CH = 1, BS = 64, SR = 48000;
    constexpr int M = 100;
    std::vector<float> ir(M);
    for (int i = 0; i < M; ++i) ir[i] = std::cos(0.05f * i) * std::exp(-0.02f * i);

    GpuConvolver node(CH, BS, SR, ir);
    REQUIRE(node.prepare());
    if (node.gpu_available()) return;  // this case covers the no-device build only

    constexpr uint32_t NBLK = 8;
    std::vector<float> input(BS * NBLK);
    for (uint32_t i = 0; i < input.size(); ++i)
        input[i] = std::sin(0.11f * i) + 0.3f * std::sin(0.37f * i);
    const std::vector<float> ref = direct_convolution(input, ir);

    std::vector<float> out(BS * NBLK, 0.0f);
    bool produced_audio = false;
    for (uint32_t b = 0; b < NBLK; ++b) {
        const float* in_ptr[1] = {input.data() + b * BS};
        float* out_ptr[1] = {out.data() + b * BS};
        BufferView<const float> iv(in_ptr, 1, BS);
        BufferView<float> ov(out_ptr, 1, BS);
        node.process_block(iv, ov, BS);
    }
    for (uint32_t i = 0; i < out.size(); ++i) {
        if (std::abs(out[i]) > 1.0e-6f) produced_audio = true;
        REQUIRE(std::abs(out[i] - ref[i]) < 1.0e-3f * (1.0f + std::abs(ref[i])));
    }
    REQUIRE(produced_audio);
}

// The worker path (process_block) must not touch the realtime miss fallback: they
// are separate convolvers on separate threads. Feeding the RT fallback is done
// only by prime_fallback(); process_block advancing it would corrupt the miss
// substitute the moment a real miss occurs.
TEST_CASE("GpuConvolver worker fallback does not advance realtime fallback state",
          "[gpu_audio][convolver]") {
    constexpr uint32_t CH = 1, BS = 64, SR = 48000;
    std::vector<float> ir = {0.5f, 0.25f, 0.1f};

    GpuConvolver node(CH, BS, SR, ir);
    REQUIRE(node.prepare());

    GpuConvolver expected(CH, BS, SR, ir);
    REQUIRE(expected.prepare());

    std::vector<float> input(BS);
    for (uint32_t i = 0; i < BS; ++i) input[i] = static_cast<float>((i % 7) + 1);
    std::vector<float> scratch(BS, 0.0f), miss_out(BS, -1.0f), expected_miss_out(BS, -2.0f);

    const float* in_ptr[1] = {input.data()};
    float* scratch_ptr[1] = {scratch.data()};
    float* miss_ptr[1] = {miss_out.data()};
    float* expected_miss_ptr[1] = {expected_miss_out.data()};
    BufferView<const float> iv(in_ptr, 1, BS);
    BufferView<float> scratch_view(scratch_ptr, 1, BS);
    BufferView<float> miss_view(miss_ptr, 1, BS);
    BufferView<float> expected_miss_view(expected_miss_ptr, 1, BS);

    // Hammer the worker path on `node` many times; its RT miss fallback must be
    // byte-identical to a pristine node's, since only prime_fallback() feeds it.
    for (int k = 0; k < 5; ++k) node.process_block(iv, scratch_view, BS);
    node.process_cpu_fallback(iv, miss_view, BS);
    expected.process_cpu_fallback(iv, expected_miss_view, BS);

    for (uint32_t i = 0; i < BS; ++i)
        REQUIRE(miss_out[i] == expected_miss_out[i]);
}

// BUG 1 (stale history): the RT fallback must be a CORRECT CONTINUATION when it
// takes over after a run of GPU hits — its overlap-add tail must carry the IR
// energy of the preceding (primed) blocks. A fallback fed only on the miss block
// (the old behaviour) starts cold and is missing that tail.
TEST_CASE("GpuConvolver fallback is a correct continuation after hits",
          "[gpu_audio][convolver]") {
    constexpr uint32_t CH = 1, BS = 64, SR = 48000;
    constexpr int M = 200;  // IR spans several blocks so the tail is significant
    std::vector<float> ir(M);
    for (int i = 0; i < M; ++i) ir[i] = std::cos(0.07f * i) * std::exp(-0.01f * i);

    GpuConvolver node(CH, BS, SR, ir);
    REQUIRE(node.prepare());
    const uint32_t L = GpuConvolver::kLatencyBlocks;

    constexpr uint32_t NBLK = 10;
    std::vector<float> input(BS * NBLK);
    for (uint32_t i = 0; i < input.size(); ++i)
        input[i] = std::sin(0.09f * i) + 0.25f * std::sin(0.31f * i);
    const std::vector<float> ref = direct_convolution(input, ir);

    // Prime every block (simulating GPU hits); take the fallback substitute only
    // on the final block — the moment the fallback "kicks in".
    std::vector<float> sub(BS, 0.0f);
    for (uint32_t b = 0; b < NBLK; ++b) {
        const float* in_ptr[1] = {input.data() + b * BS};
        BufferView<const float> iv(in_ptr, 1, BS);
        node.prime_fallback(iv, BS);
        if (b == NBLK - 1) {
            float* sub_ptr[1] = {sub.data()};
            BufferView<float> sv(sub_ptr, 1, BS);
            node.process_cpu_fallback(iv, sv, BS);
        }
    }

    // The substitute for wall-clock block (NBLK-1) is the wet block for input
    // block (NBLK-1-L): a correct, tail-inclusive continuation of the stream.
    const uint32_t src_block = NBLK - 1 - L;
    for (uint32_t i = 0; i < BS; ++i) {
        const float expected = ref[src_block * BS + i];
        REQUIRE(std::abs(sub[i] - expected) < 1.0e-3f * (1.0f + std::abs(expected)));
    }

    // Prove the tail energy is actually present: a COLD fallback fed only this one
    // block (the stale-history behaviour) would differ materially.
    pulp::signal::PartitionedConvolver cold;
    cold.load_ir(ir.data(), ir.size(), BS);
    std::vector<float> cold_out(BS, 0.0f);
    cold.process(input.data() + src_block * BS, cold_out.data(), BS);
    float max_diff = 0.0f;
    for (uint32_t i = 0; i < BS; ++i)
        max_diff = std::max(max_diff, std::abs(cold_out[i] - sub[i]));
    REQUIRE(max_diff > 1.0e-3f);  // continuation != cold start
}

// BUG 2 (latency off-by-one): the fallback substitute for wall-clock block t must
// equal the wet block for input block t-kLatencyBlocks — sample-aligned with the
// PDC the transport reports, neither a block early nor late. An all-miss run must
// reproduce the reference convolution delayed by exactly kLatencyBlocks blocks.
TEST_CASE("GpuConvolver fallback latency matches reported PDC",
          "[gpu_audio][convolver]") {
    constexpr uint32_t CH = 2, BS = 64, SR = 48000;
    constexpr int M = 150;
    std::vector<float> ir(M);
    for (int i = 0; i < M; ++i) ir[i] = std::sin(0.04f * i + 0.3f) * std::exp(-0.015f * i);

    GpuConvolver node(CH, BS, SR, ir);
    REQUIRE(node.prepare());
    const uint32_t L = GpuConvolver::kLatencyBlocks;

    constexpr uint32_t NBLK = 12;
    // Two distinct per-channel signals to prove channels stay independent.
    std::vector<std::vector<float>> in_ch(CH, std::vector<float>(BS * NBLK));
    for (uint32_t i = 0; i < BS * NBLK; ++i) {
        in_ch[0][i] = std::sin(0.10f * i);
        in_ch[1][i] = 0.7f * std::sin(0.05f * i + 1.0f);
    }
    std::vector<std::vector<float>> ref(CH);
    for (uint32_t c = 0; c < CH; ++c) ref[c] = direct_convolution(in_ch[c], ir);

    std::vector<std::vector<float>> out(CH, std::vector<float>(BS * NBLK, 0.0f));
    for (uint32_t b = 0; b < NBLK; ++b) {
        const float* in_ptr[CH] = {in_ch[0].data() + b * BS, in_ch[1].data() + b * BS};
        float* out_ptr[CH] = {out[0].data() + b * BS, out[1].data() + b * BS};
        BufferView<const float> iv(in_ptr, CH, BS);
        BufferView<float> ov(out_ptr, CH, BS);
        node.prime_fallback(iv, BS);        // every block
        node.process_cpu_fallback(iv, ov, BS);  // every block is a "miss"
    }

    for (uint32_t c = 0; c < CH; ++c) {
        for (uint32_t b = 0; b < NBLK; ++b) {
            for (uint32_t i = 0; i < BS; ++i) {
                const float got = out[c][b * BS + i];
                const float expected =
                    (b < L) ? 0.0f : ref[c][(b - L) * BS + i];  // delayed by L, silence before
                REQUIRE(std::abs(got - expected) < 1.0e-3f * (1.0f + std::abs(expected)));
            }
        }
    }
}

// BUG 3 (NaN poison): the guarded overlap-add must never let a single non-finite
// readback sample persist in the carry. A poisoned block resets the carry and
// emits silence; the next finite block convolves correctly from a clean state.
TEST_CASE("Guarded overlap-add contains a NaN instead of poisoning the carry",
          "[gpu_audio][convolver]") {
    using pulp::gpu_audio::detail::overlap_add_block;
    constexpr uint32_t FFT = 8, N = 4;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    // Interleaved-complex source (stride 2), like the GPU readback.
    auto make_src = [](std::initializer_list<float> reals) {
        std::vector<float> s(reals.size() * 2, 0.0f);
        uint32_t i = 0;
        for (float r : reals) s[2u * i++] = r;
        return s;
    };

    std::vector<float> carry(FFT, 0.0f), out(N, -1.0f);

    // Finite block accumulates normally.
    auto good = make_src({1, 2, 3, 4, 5, 6, 7, 8});
    REQUIRE(overlap_add_block(carry.data(), good.data(), 2, out.data(), FFT, N));
    REQUIRE(out[0] == 1.0f);

    // A NaN anywhere in the readback -> carry reset, silence out, returns false.
    auto poison = make_src({0, 0, nan, 0, 0, 0, 0, 0});
    REQUIRE_FALSE(overlap_add_block(carry.data(), poison.data(), 2, out.data(), FFT, N));
    for (uint32_t i = 0; i < N; ++i) REQUIRE(out[i] == 0.0f);
    for (uint32_t i = 0; i < FFT; ++i) REQUIRE(carry[i] == 0.0f);  // no NaN retained

    // An Inf is caught the same way.
    auto poison2 = make_src({1, 1, 1, 1, inf, 1, 1, 1});
    REQUIRE_FALSE(overlap_add_block(carry.data(), poison2.data(), 2, out.data(), FFT, N));
    for (uint32_t i = 0; i < FFT; ++i) REQUIRE(std::isfinite(carry[i]));

    // Recovery: a subsequent finite block produces correct, finite output.
    REQUIRE(overlap_add_block(carry.data(), good.data(), 2, out.data(), FFT, N));
    for (uint32_t i = 0; i < N; ++i) REQUIRE(std::isfinite(out[i]));
    REQUIRE(out[0] == 1.0f);  // clean carry, no lingering poison
}

// BUG 3 at the node level: a single NaN in a primed input block must not poison
// the fallback for the rest of the session — it recovers on the next clean block.
TEST_CASE("GpuConvolver fallback recovers from a NaN input block",
          "[gpu_audio][convolver]") {
    constexpr uint32_t CH = 1, BS = 64, SR = 48000;
    std::vector<float> ir = {0.5f, 0.3f, 0.15f, 0.05f};
    GpuConvolver node(CH, BS, SR, ir);
    REQUIRE(node.prepare());
    const uint32_t L = GpuConvolver::kLatencyBlocks;

    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> good(BS), bad(BS);
    for (uint32_t i = 0; i < BS; ++i) { good[i] = std::sin(0.2f * i); bad[i] = good[i]; }
    bad[BS / 2] = nan;

    auto prime = [&](const std::vector<float>& blk) {
        const float* p[1] = {blk.data()};
        BufferView<const float> iv(p, 1, BS);
        node.prime_fallback(iv, BS);
    };
    auto miss = [&]() {
        std::vector<float> o(BS, 0.0f);
        const float* p[1] = {good.data()};
        float* op[1] = {o.data()};
        BufferView<const float> iv(p, 1, BS);
        BufferView<float> ov(op, 1, BS);
        node.process_cpu_fallback(iv, ov, BS);
        return o;
    };

    prime(bad);                       // poison attempt
    for (uint32_t k = 0; k < L; ++k) prime(good);  // flush the delay ring
    // Feed several more clean blocks; output must be finite (self-healed).
    for (uint32_t k = 0; k < 6; ++k) {
        prime(good);
        auto o = miss();
        for (uint32_t i = 0; i < BS; ++i) REQUIRE(std::isfinite(o[i]));
    }
}

// BUG 3 on the worker/no-GPU path: render_worker_fallback() is the SOLE audio
// path when the device fails to init (and in the no-render build). A single NaN
// input sample must not smear across the FFT bins and poison worker_fallback_'s
// overlap state for the whole IR tail — it must emit one silent block and recover
// on the very next finite block.
TEST_CASE("GpuConvolver worker fallback recovers from a NaN input block",
          "[gpu_audio][convolver]") {
    constexpr uint32_t CH = 1, BS = 64, SR = 48000;
    constexpr int M = 200;  // IR spans several blocks: a smear would last many blocks
    std::vector<float> ir(M);
    for (int i = 0; i < M; ++i) ir[i] = std::cos(0.05f * i) * std::exp(-0.01f * i);

    GpuConvolver node(CH, BS, SR, ir);
    REQUIRE(node.prepare());
    if (node.gpu_available()) return;  // exercises the no-device worker path only

    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> good(BS), bad(BS);
    for (uint32_t i = 0; i < BS; ++i) { good[i] = std::sin(0.2f * i); bad[i] = good[i]; }
    bad[BS / 2] = nan;

    auto run = [&](const std::vector<float>& blk) {
        std::vector<float> o(BS, -99.0f);
        const float* ip[1] = {blk.data()};
        float* op[1] = {o.data()};
        BufferView<const float> iv(ip, 1, BS);
        BufferView<float> ov(op, 1, BS);
        node.process_block(iv, ov, BS);  // no GPU -> render_worker_fallback
        return o;
    };

    run(good);                 // prime some history
    auto poisoned = run(bad);  // the NaN block
    for (uint32_t i = 0; i < BS; ++i) REQUIRE(poisoned[i] == 0.0f);  // silence, not NaN

    // Every subsequent block must be finite (no tail smear) and, once history
    // rebuilds, actually produce audio.
    bool produced_audio = false;
    for (uint32_t b = 0; b < 8; ++b) {
        auto o = run(good);
        for (uint32_t i = 0; i < BS; ++i) {
            REQUIRE(std::isfinite(o[i]));
            if (std::abs(o[i]) > 1.0e-6f) produced_audio = true;
        }
    }
    REQUIRE(produced_audio);
}

// Audible-behaviour parity through the real transport: with no worker ever
// pumping, every block past the primed latency is a miss, so the transport's
// output IS the fallback stream. It must equal the direct convolution delayed by
// latency_samples() — the seamless, correct fallback the design promises.
TEST_CASE("GpuAudioTransport fallback stream matches reference convolution",
          "[gpu_audio][convolver][transport]") {
    using pulp::gpu_audio::GpuAudioTransport;
    constexpr uint32_t CH = 1, BS = 64, SR = 48000;
    constexpr int M = 180;
    std::vector<float> ir(M);
    for (int i = 0; i < M; ++i) ir[i] = std::cos(0.06f * i) * std::exp(-0.012f * i);

    GpuConvolver node(CH, BS, SR, ir);
    REQUIRE(node.prepare());

    // No worker thread and we never pump(), so the GPU is never driven regardless
    // of whether a device exists: every read past the primed latency is a miss and
    // the transport's output is purely the continuously-fed CPU fallback stream.
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {/*ring_blocks=*/8}));
    const uint32_t L = transport.latency_samples() / BS;

    constexpr uint32_t NBLK = 24;
    std::vector<float> input(BS * NBLK);
    for (uint32_t i = 0; i < input.size(); ++i)
        input[i] = std::sin(0.08f * i) + 0.2f * std::sin(0.41f * i);
    const std::vector<float> ref = direct_convolution(input, ir);

    std::vector<float> out(BS * NBLK, 0.0f);
    for (uint32_t b = 0; b < NBLK; ++b) {
        const float* in_ptr[1] = {input.data() + b * BS};
        float* out_ptr[1] = {out.data() + b * BS};
        BufferView<const float> iv(in_ptr, 1, BS);
        BufferView<float> ov(out_ptr, 1, BS);
        transport.process(iv, ov, BS);  // never pumped -> misses -> fallback substitute
    }

    // First L blocks are the primed silence; the rest are ref delayed by L blocks.
    for (uint32_t b = 0; b < NBLK; ++b) {
        for (uint32_t i = 0; i < BS; ++i) {
            const float got = out[b * BS + i];
            const float expected = (b < L) ? 0.0f : ref[(b - L) * BS + i];
            REQUIRE(std::abs(got - expected) < 1.0e-3f * (1.0f + std::abs(expected)));
        }
    }

    const auto s = transport.stats();
    REQUIRE(s.miss_blocks > 0);  // we really exercised the miss path
}
