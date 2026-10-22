#include <catch2/catch_test_macros.hpp>

#include <pulp/gpu_audio/detail/gpu_ola.hpp>
#include <pulp/gpu_audio/gpu_audio_program.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_convolver.hpp>
#include <pulp/gpu_audio/gpu_multi_convolver.hpp>
#include <pulp/render/gpu_compute.hpp>

#include "detail/gpu_convolver_test_access.hpp"
#include "detail/realtime_gpu_audio_path.hpp"
#include "harness/rt_allocation_probe.hpp"
#include "support/shared_io_loss_provider.hpp"

#if PULP_TEST_HAS_GPU_COMPUTE
#include "support/fake_gpu_compute.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

// A lane with no compute device must SKIP, out loud: Catch2 records a bare
// early return as a PASS, so a GPU-less runner would report this file green
// having asserted nothing about the GPU path at all.
constexpr const char* kNoGpu =
    "no GPU compute device available (Skia/Dawn not built, or no adapter)";

// Direct linear convolution reference: y[n] = sum_k ir[k] * x[n-k].
std::vector<float> direct_convolution(const std::vector<float>& x, const std::vector<float>& ir) {
    std::vector<float> y(x.size(), 0.0f);
    for (std::size_t n = 0; n < x.size(); ++n) {
        double acc = 0.0;
        for (std::size_t k = 0; k < ir.size() && k <= n; ++k)
            acc += static_cast<double>(ir[k]) * x[n - k];
        y[n] = static_cast<float>(acc);
    }
    return y;
}

std::vector<double> direct_convolution_full_tail(const std::vector<float>& input,
                                                 const std::vector<float>& ir) {
    if (input.empty() || ir.empty())
        return {};

    std::vector<double> output(input.size() + ir.size() - 1u, 0.0);
    for (std::size_t n = 0; n < output.size(); ++n) {
        const std::size_t first_ir = n >= input.size() ? n - input.size() + 1u : 0u;
        const std::size_t last_ir = std::min(n, ir.size() - 1u);
        for (std::size_t k = first_ir; k <= last_ir; ++k)
            output[n] += static_cast<double>(ir[k]) * static_cast<double>(input[n - k]);
    }
    return output;
}

bool full_tail_matches(const std::vector<std::vector<float>>& candidate,
                       const std::vector<std::vector<double>>& reference) {
    if (candidate.size() != reference.size())
        return false;
    for (std::size_t channel = 0; channel < reference.size(); ++channel) {
        if (candidate[channel].size() != reference[channel].size())
            return false;
        for (std::size_t i = 0; i < reference[channel].size(); ++i) {
            const double got = candidate[channel][i];
            const double expected = reference[channel][i];
            const double tolerance = 2.0e-2 * (1.0 + std::abs(expected));
            if (!std::isfinite(got) || std::abs(got - expected) >= tolerance)
                return false;
        }
    }
    return true;
}

std::vector<std::vector<float>> make_oracle_input(std::uint32_t channels, std::size_t size) {
    std::vector<std::vector<float>> input(channels, std::vector<float>(size));
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
        for (std::size_t i = 0; i < size; ++i) {
            input[channel][i] = static_cast<float>(0.23 * std::sin(0.019 * i + channel * 0.73) +
                                                   0.11 * std::cos(0.083 * i + channel * 0.31));
        }
        input[channel][0] += channel == 0u ? 0.71f : -0.43f;
        input[channel][97u + 19u * channel] += channel == 0u ? -0.37f : 0.59f;
    }
    return input;
}

std::vector<float> make_oracle_ir() {
    constexpr std::size_t ir_size = 257;
    std::vector<float> ir(ir_size);
    for (std::size_t i = 0; i < ir.size(); ++i) {
        ir[i] = static_cast<float>((0.19 * std::cos(0.031 * i) - 0.07 * std::sin(0.113 * i)) *
                                   std::exp(-0.009 * i));
    }
    return ir;
}

std::vector<std::vector<float>> render_full_tail(std::uint32_t channels, std::uint32_t block,
                                                 const std::vector<float>& ir,
                                                 const std::vector<std::vector<float>>& input) {
    constexpr std::uint32_t sample_rate = 48000;
    const std::size_t output_size = input.front().size() + ir.size() - 1u;
    REQUIRE(output_size % block == 0u);

    pulp::gpu_audio::GpuConvolver node(channels, block, sample_rate, ir);
    // The oracle checks the GPU math, so every block must take the GPU result:
    // the blocking staged readback decides each block inside its call. (The
    // default path judges a result one call later and, driven back to back
    // like this, would serve the blocks from its CPU convolution.)
    REQUIRE(
        node.set_provider_policy(pulp::gpu_audio::GpuConvolver::ProviderPolicy::StagedBlocking));
    REQUIRE(node.prepare());
    if (!node.gpu_available())
        SKIP(kNoGpu);

    std::vector<std::vector<float>> output(channels, std::vector<float>(output_size, 0.0f));
    std::vector<std::vector<float>> input_block(channels, std::vector<float>(block, 0.0f));
    std::vector<const float*> input_ptrs(channels);
    std::vector<float*> output_ptrs(channels);
    for (std::size_t offset = 0; offset < output_size; offset += block) {
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            std::fill(input_block[channel].begin(), input_block[channel].end(), 0.0f);
            if (offset < input[channel].size()) {
                const std::size_t remaining = input[channel].size() - offset;
                std::copy_n(input[channel].data() + offset, std::min<std::size_t>(block, remaining),
                            input_block[channel].data());
            }
            input_ptrs[channel] = input_block[channel].data();
            output_ptrs[channel] = output[channel].data() + offset;
        }
        pulp::audio::BufferView<const float> input_view(input_ptrs.data(), channels, block);
        pulp::audio::BufferView<float> output_view(output_ptrs.data(), channels, block);
        node.process_block(input_view, output_view, block);
    }
    return output;
}

} // namespace

using namespace pulp::gpu_audio;
using pulp::audio::BufferView;

// Golden test: the GPU FFT/overlap-add convolver must reproduce direct linear
// convolution of the input stream with the IR, within f32 tolerance.
TEST_CASE("GpuConvolver matches direct convolution", "[gpu_audio][convolver][gpu]") {
    constexpr uint32_t CH = 1, BS = 64, SR = 48000;
    constexpr int M = 300; // IR length (spans several blocks)

    std::vector<float> ir(M);
    for (int i = 0; i < M; ++i) {
        ir[i] = std::cos(0.05f * i) * std::exp(-0.01f * i);
    }

    GpuConvolver node(CH, BS, SR, ir);
    // Golden check of the GPU math: decide every block on the GPU inside its
    // call (see render_full_tail).
    REQUIRE(node.set_provider_policy(GpuConvolver::ProviderPolicy::StagedBlocking));
    REQUIRE(node.prepare());
    if (!node.gpu_available())
        SKIP(kNoGpu);
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
        for (int i = 0; i < static_cast<int>(BS); ++i)
            in_block[i] = x[b * BS + i];
        const float* in_ptr[1] = {in_block.data()};
        float* out_ptr[1] = {out_block.data()};
        BufferView<const float> iv(in_ptr, 1, BS);
        BufferView<float> ov(out_ptr, 1, BS);
        node.process_block(iv, ov, BS);
        for (int i = 0; i < static_cast<int>(BS); ++i)
            gpu_out[b * BS + i] = out_block[i];
    }

    // Reference: y[n] = sum_k ir[k] * x[n-k].
    for (int n = 0; n < total; ++n) {
        double acc = 0.0;
        for (int k = 0; k < M && k <= n; ++k)
            acc += static_cast<double>(ir[k]) * x[n - k];
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

TEST_CASE("GpuConvolver provider policy is immutable and fail-closed",
          "[gpu_audio][convolver][provider-policy]") {
    constexpr uint32_t BS = 32;
    const std::vector<float> ir{1.0f};

    // The installed arm64 SDK may contain shared support without changing
    // existing callers. This also runs in exact-provider capability-ON builds.
    GpuConvolver automatic(1, BS, 48000, ir);
    REQUIRE(automatic.provider_policy() == GpuConvolver::ProviderPolicy::Auto);
    REQUIRE(automatic.prepare());
    GpuAudioTransport automatic_transport;
    REQUIRE(automatic_transport.prepare(&automatic, {.ring_blocks = 8}));
    REQUIRE(automatic_transport.capability_report().path == GpuAudioExecutionPath::Staged);

    GpuConvolver staged(1, BS, 48000, ir);
    REQUIRE(staged.set_provider_policy(GpuConvolver::ProviderPolicy::StagedOnly));
    REQUIRE(staged.provider_policy() == GpuConvolver::ProviderPolicy::StagedOnly);
    REQUIRE(staged.prepare());
    REQUIRE_FALSE(staged.set_provider_policy(GpuConvolver::ProviderPolicy::Auto));

    GpuAudioTransport staged_transport;
    REQUIRE(staged_transport.prepare(&staged, {.ring_blocks = 8}));
    const auto staged_report = staged_transport.capability_report();
    REQUIRE(staged_report.prepared);
    REQUIRE(staged_report.path == GpuAudioExecutionPath::Staged);
    REQUIRE(staged_report.provider == GpuAudioProvider::Unknown);

    GpuConvolver shared(1, BS, 48000, ir);
    REQUIRE(shared.set_provider_policy(GpuConvolver::ProviderPolicy::SharedRequired));
    const bool prepared = shared.prepare();
    if (!prepared) {
        // No exact authenticated shared provider is a valid result on hosts
        // without the opt-in Dawn/Metal configuration. It must not silently
        // become staged or standalone execution.
        GpuAudioTransport rejected_transport;
        const auto rejected = rejected_transport.capability_report();
        REQUIRE_FALSE(rejected.prepared);
    } else {
        GpuAudioTransport shared_transport;
        REQUIRE(shared_transport.prepare(&shared, {.ring_blocks = 8}));
        const auto report = shared_transport.capability_report();
        REQUIRE(report.prepared);
        REQUIRE(report.path == GpuAudioExecutionPath::SharedMemory);
        REQUIRE(report.provider == GpuAudioProvider::Dawn);
    }
}

TEST_CASE("GpuConvolver public trace configuration is opt-in and prepare-bound",
          "[gpu_audio][convolver][trace]") {
    GpuConvolver node(1, 32, 48000, {1.0f});
    REQUIRE(node.set_provider_policy(GpuConvolver::ProviderPolicy::StagedOnly));
    CHECK_FALSE(node.configure_trace({.enabled = true, .success_stride = 0}));
    REQUIRE(node.configure_trace({.enabled = true,
                                  .capture_admissions = true,
                                  .capture_callback_timing = true,
                                  .success_stride = 2}));
    REQUIRE(node.prepare());
    CHECK_FALSE(node.configure_trace({.enabled = true}));
}

TEST_CASE("GpuConvolver shared realtime path requires explicit provider selection",
          "[gpu_audio][convolver][realtime-path]") {
#if PULP_GPU_AUDIO_ENABLE_EXPERIMENTAL_SHARED_IO_CONVOLVER
    constexpr uint32_t BS = 32;
    GpuConvolver node(1, BS, 48000, {1.0f});
    REQUIRE(node.set_provider_policy(GpuConvolver::ProviderPolicy::SharedRequired));
    REQUIRE(node.prepare());
    REQUIRE(detail::realtime_gpu_node_path(&node).active());

    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {.ring_blocks = 8, .run_worker_thread = false}));

    std::vector<float> input(BS), output(BS);
    const float* input_channels[1] = {input.data()};
    float* output_channels[1] = {output.data()};
    BufferView<const float> input_view(input_channels, 1, BS);
    BufferView<float> output_view(output_channels, 1, BS);

    std::uint64_t expected_produced = 0;
    for (uint32_t sequence = 0; sequence < 8; ++sequence) {
        if (sequence == 4) {
            // Reattach to the same prepared node; its shared session and CPU
            // history retain the absolute timeline across transport lifetimes.
            REQUIRE(transport.prepare(&node, {.ring_blocks = 8, .run_worker_thread = false}));
            expected_produced = 0;
        }
        std::fill(input.begin(), input.end(), static_cast<float>(sequence + 1));
        transport.process(input_view, output_view, BS);
        const float expected =
            sequence < GpuConvolver::kLatencyBlocks ? 0.0f : static_cast<float>(sequence - 1);
        for (float sample : output)
            REQUIRE(std::abs(sample - expected) < 1.0e-4f);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        ++expected_produced;
        while (transport.stats().produced_blocks < expected_produced &&
               std::chrono::steady_clock::now() < deadline) {
            transport.pump(1);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        REQUIRE(transport.stats().produced_blocks >= expected_produced);
    }
    REQUIRE(transport.stats().miss_blocks == 0);
    const auto admitted_before_offline = transport.stats().produced_blocks;
    // Offline fences the shared provider but retains every continuously primed
    // CPU delay/history sample. Returning to RT remains CPU-only.
    for (uint32_t sequence = 8; sequence < 16; ++sequence) {
        std::fill(input.begin(), input.end(), static_cast<float>(sequence + 1));
        if (sequence < 12)
            transport.process_offline(input_view, output_view, BS);
        else
            transport.process(input_view, output_view, BS);
        for (float sample : output)
            REQUIRE(std::abs(sample - static_cast<float>(sequence - 1)) < 1.0e-4f);
        transport.pump(1);
    }
    REQUIRE(transport.stats().produced_blocks == admitted_before_offline);
#else
    const std::vector<float> ir(64, 0.1f);
    GpuConvolver node(1, 64, 48000, ir);
    REQUIRE(node.prepare());
    REQUIRE_FALSE(detail::realtime_gpu_node_path(&node).active());
#endif
}

// The CPU fallback is a signal::PartitionedConvolver loaded at the node's block
// size, and load_ir() rounds a non-power-of-two block UP to the next power of two.
// The fallback would then be partitioned for a size the transport never delivers,
// so every fallback block would be a block-size violation (silence). Refuse to
// prepare instead of running a convolver whose fallback can only fail closed.
TEST_CASE("GpuConvolver rejects a non-power-of-two block size", "[gpu_audio][convolver]") {
    const std::vector<float> ir(128, 0.1f);
    for (uint32_t block : {3u, 100u, 192u, 500u}) {
        GpuConvolver node(1, block, 48000, ir);
        INFO("block = " << block);
        REQUIRE_FALSE(node.prepare());
    }
    // Powers of two still prepare.
    for (uint32_t block : {1u, 64u, 512u}) {
        GpuConvolver node(1, block, 48000, ir);
        INFO("block = " << block);
        REQUIRE(node.prepare());
    }
}

// The no-GPU worker path is a zero-latency convolution: process_block (which
// drives worker_fallback_ when there is no device) must reproduce direct linear
// convolution of the stream, block-aligned (no one-block streaming lag), on both
// engines the fixed-IR fallback can select.
TEST_CASE("GpuConvolver process_block uses CPU fallback without a GPU", "[gpu_audio][convolver]") {
    constexpr uint32_t CH = 1, BS = 64, SR = 48000;
#if defined(PULP_SIMD_DEFAULT_BACKEND_ACCELERATE)
    // Where the policy has a direct-engine range, the short IR exercises it.
    REQUIRE(pulp::signal::FixedIrConvolver::choose_backend(100, BS) !=
            pulp::signal::FixedIrConvolver::choose_backend(1025, BS));
#endif
    for (const int M : {100, 1025}) {
        INFO("IR length " << M);
        std::vector<float> ir(M);
        for (int i = 0; i < M; ++i)
            ir[i] = std::cos(0.05f * i) * std::exp(-0.02f * i);

        GpuConvolver node(CH, BS, SR, ir);
        REQUIRE(node.prepare());
        if (node.gpu_available())
            return; // this case covers the no-device build only

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
            if (std::abs(out[i]) > 1.0e-6f)
                produced_audio = true;
            REQUIRE(std::abs(out[i] - ref[i]) < 1.0e-3f * (1.0f + std::abs(ref[i])));
        }
        REQUIRE(produced_audio);
    }
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
    for (uint32_t i = 0; i < BS; ++i)
        input[i] = static_cast<float>((i % 7) + 1);
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
    for (int k = 0; k < 5; ++k)
        node.process_block(iv, scratch_view, BS);
    node.process_cpu_fallback(iv, miss_view, BS);
    expected.process_cpu_fallback(iv, expected_miss_view, BS);

    for (uint32_t i = 0; i < BS; ++i)
        REQUIRE(miss_out[i] == expected_miss_out[i]);
}

// BUG 1 (stale history): the RT fallback must be a CORRECT CONTINUATION when it
// takes over after a run of GPU hits — its overlap-add tail must carry the IR
// energy of the preceding (primed) blocks. A fallback fed only on the miss block
// (the old behavior) starts cold and is missing that tail.
TEST_CASE("GpuConvolver fallback is a correct continuation after hits", "[gpu_audio][convolver]") {
    constexpr uint32_t CH = 1, BS = 64, SR = 48000;
    constexpr int M = 200; // IR spans several blocks so the tail is significant
    std::vector<float> ir(M);
    for (int i = 0; i < M; ++i)
        ir[i] = std::cos(0.07f * i) * std::exp(-0.01f * i);

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
    // block (the stale-history behavior) would differ materially.
    pulp::signal::PartitionedConvolver cold;
    cold.load_ir(ir.data(), ir.size(), BS);
    std::vector<float> cold_out(BS, 0.0f);
    cold.process(input.data() + src_block * BS, cold_out.data(), BS);
    float max_diff = 0.0f;
    for (uint32_t i = 0; i < BS; ++i)
        max_diff = std::max(max_diff, std::abs(cold_out[i] - sub[i]));
    REQUIRE(max_diff > 1.0e-3f); // continuation != cold start
}

// BUG 2 (latency off-by-one): the fallback substitute for wall-clock block t must
// equal the wet block for input block t-kLatencyBlocks — sample-aligned with the
// PDC the transport reports, neither a block early nor late. An all-miss run must
// reproduce the reference convolution delayed by exactly kLatencyBlocks blocks.
TEST_CASE("GpuConvolver fallback latency matches reported PDC", "[gpu_audio][convolver]") {
    constexpr uint32_t CH = 2, BS = 64, SR = 48000;
    // Cover both engines the fixed-IR fallback can select, including the
    // FFT engine's overlap-save history across the PDC delay ring. The short
    // IR sits at the policy's direct-engine limit for this block size, so the
    // test follows the measured table instead of a fixed length.
    constexpr std::size_t kDirectLimit = pulp::signal::FixedIrConvolver::direct_fir_limit(BS);
    constexpr int kShortIr = kDirectLimit > 0 ? static_cast<int>(kDirectLimit) : 150;
    constexpr int kLongIr = 1025;
    static_assert(kDirectLimit < static_cast<std::size_t>(kLongIr),
                  "the long IR must select the FFT engine at this block size");
#if defined(PULP_SIMD_DEFAULT_BACKEND_ACCELERATE)
    // Where the policy has a direct-engine range, the short IR exercises it.
    REQUIRE(pulp::signal::FixedIrConvolver::choose_backend(kShortIr, BS) ==
            pulp::signal::ConvolutionBackend::direct_fir);
    REQUIRE(pulp::signal::FixedIrConvolver::choose_backend(kLongIr, BS) ==
            pulp::signal::ConvolutionBackend::partitioned_fft);
#endif
    for (const int M : {kShortIr, kLongIr}) {
        INFO("IR length " << M);
        std::vector<float> ir(M);
        for (int i = 0; i < M; ++i)
            ir[i] = std::sin(0.04f * i + 0.3f) * std::exp(-0.015f * i);

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
        for (uint32_t c = 0; c < CH; ++c)
            ref[c] = direct_convolution(in_ch[c], ir);

        std::vector<std::vector<float>> out(CH, std::vector<float>(BS * NBLK, 0.0f));
        for (uint32_t b = 0; b < NBLK; ++b) {
            const float* in_ptr[CH] = {in_ch[0].data() + b * BS, in_ch[1].data() + b * BS};
            float* out_ptr[CH] = {out[0].data() + b * BS, out[1].data() + b * BS};
            BufferView<const float> iv(in_ptr, CH, BS);
            BufferView<float> ov(out_ptr, CH, BS);
            node.prime_fallback(iv, BS);           // every block
            node.process_cpu_fallback(iv, ov, BS); // every block is a "miss"
        }

        for (uint32_t c = 0; c < CH; ++c) {
            for (uint32_t b = 0; b < NBLK; ++b) {
                for (uint32_t i = 0; i < BS; ++i) {
                    const float got = out[c][b * BS + i];
                    const float expected =
                        (b < L) ? 0.0f : ref[c][(b - L) * BS + i]; // delayed by L, silence before
                    REQUIRE(std::abs(got - expected) < 1.0e-3f * (1.0f + std::abs(expected)));
                }
            }
        }
    }
}

TEST_CASE("GpuConvolver configurable lead keeps PDC and fallback timeline aligned",
          "[gpu_audio][convolver][lead]") {
    constexpr uint32_t CH = 1, BS = 16, SR = 48000;
    const std::vector<float> ir{0.5f, -0.25f, 0.125f};
    constexpr uint32_t kBlocks = 14;
    std::vector<float> input(BS * kBlocks);
    for (uint32_t i = 0; i < input.size(); ++i)
        input[i] = std::sin(0.11f * static_cast<float>(i));
    const auto reference = direct_convolution(input, ir);

    for (const uint32_t lead : {1u, 2u, 4u, 8u}) {
        GpuConvolver node(CH, BS, SR, ir, lead);
        REQUIRE(node.prepare());
        REQUIRE(node.descriptor().latency_blocks == lead);

        for (uint32_t block = 0; block < kBlocks; ++block) {
            const float* in_ptr[CH] = {input.data() + block * BS};
            float* out_storage[CH];
            std::vector<float> output(BS, 0.0f);
            out_storage[0] = output.data();
            BufferView<const float> in_view(in_ptr, CH, BS);
            BufferView<float> out_view(out_storage, CH, BS);
            node.prime_fallback(in_view, BS);
            node.process_cpu_fallback(in_view, out_view, BS);
            for (uint32_t frame = 0; frame < BS; ++frame) {
                const float expected = block < lead ? 0.0f : reference[(block - lead) * BS + frame];
                REQUIRE(std::abs(output[frame] - expected) < 1.0e-3f * (1.0f + std::abs(expected)));
            }
        }
    }
}

TEST_CASE("GpuConvolver rejects unusable lead values and preserves the default",
          "[gpu_audio][convolver][lead][adversarial]") {
    const std::vector<float> ir{1.0f};
    GpuConvolver default_node(1, 16, 48000, ir);
    REQUIRE(default_node.descriptor().latency_blocks == GpuConvolver::kLatencyBlocks);

    GpuConvolver zero_node(1, 16, 48000, ir, 0);
    CHECK_FALSE(zero_node.prepare());
    GpuConvolver oversized_node(1, 16, 48000, ir, GpuConvolver::kMaxLatencyBlocks + 1u);
    CHECK_FALSE(oversized_node.prepare());
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
        for (float r : reals)
            s[2u * i++] = r;
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
    for (uint32_t i = 0; i < N; ++i)
        REQUIRE(out[i] == 0.0f);
    for (uint32_t i = 0; i < FFT; ++i)
        REQUIRE(carry[i] == 0.0f); // no NaN retained

    // An Inf is caught the same way.
    auto poison2 = make_src({1, 1, 1, 1, inf, 1, 1, 1});
    REQUIRE_FALSE(overlap_add_block(carry.data(), poison2.data(), 2, out.data(), FFT, N));
    for (uint32_t i = 0; i < FFT; ++i)
        REQUIRE(std::isfinite(carry[i]));

    // Recovery: a subsequent finite block produces correct, finite output.
    REQUIRE(overlap_add_block(carry.data(), good.data(), 2, out.data(), FFT, N));
    for (uint32_t i = 0; i < N; ++i)
        REQUIRE(std::isfinite(out[i]));
    REQUIRE(out[0] == 1.0f); // clean carry, no lingering poison
}

// BUG 3 at the node level: a single NaN in a primed input block must not poison
// the fallback for the rest of the session — it recovers on the next clean block.
TEST_CASE("GpuConvolver fallback recovers from a NaN input block", "[gpu_audio][convolver]") {
    constexpr uint32_t CH = 1, BS = 64, SR = 48000;
    std::vector<float> ir = {0.5f, 0.3f, 0.15f, 0.05f};
    GpuConvolver node(CH, BS, SR, ir);
    REQUIRE(node.prepare());
    const uint32_t L = GpuConvolver::kLatencyBlocks;

    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> good(BS), bad(BS);
    for (uint32_t i = 0; i < BS; ++i) {
        good[i] = std::sin(0.2f * i);
        bad[i] = good[i];
    }
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

    prime(bad); // poison attempt
    for (uint32_t k = 0; k < L; ++k)
        prime(good); // flush the delay ring
    // Feed several more clean blocks; output must be finite (self-healed).
    for (uint32_t k = 0; k < 6; ++k) {
        prime(good);
        auto o = miss();
        for (uint32_t i = 0; i < BS; ++i)
            REQUIRE(std::isfinite(o[i]));
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
    constexpr int M = 200; // IR spans several blocks: a smear would last many blocks
    std::vector<float> ir(M);
    for (int i = 0; i < M; ++i)
        ir[i] = std::cos(0.05f * i) * std::exp(-0.01f * i);

    GpuConvolver node(CH, BS, SR, ir);
    REQUIRE(node.prepare());
    if (node.gpu_available())
        return; // exercises the no-device worker path only

    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> good(BS), bad(BS);
    for (uint32_t i = 0; i < BS; ++i) {
        good[i] = std::sin(0.2f * i);
        bad[i] = good[i];
    }
    bad[BS / 2] = nan;

    auto run = [&](const std::vector<float>& blk) {
        std::vector<float> o(BS, -99.0f);
        const float* ip[1] = {blk.data()};
        float* op[1] = {o.data()};
        BufferView<const float> iv(ip, 1, BS);
        BufferView<float> ov(op, 1, BS);
        node.process_block(iv, ov, BS); // no GPU -> render_worker_fallback
        return o;
    };

    run(good);                // prime some history
    auto poisoned = run(bad); // the NaN block
    for (uint32_t i = 0; i < BS; ++i)
        REQUIRE(poisoned[i] == 0.0f); // silence, not NaN

    // Every subsequent block must be finite (no tail smear) and, once history
    // rebuilds, actually produce audio.
    bool produced_audio = false;
    for (uint32_t b = 0; b < 8; ++b) {
        auto o = run(good);
        for (uint32_t i = 0; i < BS; ++i) {
            REQUIRE(std::isfinite(o[i]));
            if (std::abs(o[i]) > 1.0e-6f)
                produced_audio = true;
        }
    }
    REQUIRE(produced_audio);
}

// Audible-behavior parity through the real transport: with no worker ever
// pumping, every block past the primed latency is a miss, so the transport's
// output IS the fallback stream. It must equal the direct convolution delayed by
// latency_samples() — the seamless, correct fallback the design promises.
TEST_CASE("GpuAudioTransport fallback stream matches reference convolution",
          "[gpu_audio][convolver][transport]") {
    using pulp::gpu_audio::GpuAudioTransport;
    constexpr uint32_t CH = 1, BS = 64, SR = 48000;
    constexpr int M = 180;
    std::vector<float> ir(M);
    for (int i = 0; i < M; ++i)
        ir[i] = std::cos(0.06f * i) * std::exp(-0.012f * i);

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
        transport.process(iv, ov, BS); // never pumped -> misses -> fallback substitute
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
    REQUIRE(s.miss_blocks > 0); // we really exercised the miss path
}

TEST_CASE("GpuConvolver full-tail oracle matrix", "[gpu_audio][convolver][p2][gpu]") {
    constexpr std::size_t input_size = 384;
    const auto ir = make_oracle_ir();
    for (const std::uint32_t channels : {1u, 2u}) {
        const auto input = make_oracle_input(channels, input_size);
        std::vector<std::vector<double>> reference(channels);
        for (std::uint32_t channel = 0; channel < channels; ++channel)
            reference[channel] = direct_convolution_full_tail(input[channel], ir);

        std::vector<std::vector<float>> partition_reference;
        for (const std::uint32_t block : {32u, 64u, 128u}) {
            CAPTURE(channels, block);
            const auto output = render_full_tail(channels, block, ir, input);
            REQUIRE(full_tail_matches(output, reference));
            if (partition_reference.empty()) {
                partition_reference = output;
            } else {
                for (std::uint32_t channel = 0; channel < channels; ++channel) {
                    REQUIRE(output[channel].size() == partition_reference[channel].size());
                    for (std::size_t i = 0; i < output[channel].size(); ++i) {
                        const float expected = partition_reference[channel][i];
                        REQUIRE(std::abs(output[channel][i] - expected) <
                                2.0e-2f * (1.0f + std::abs(expected)));
                    }
                }
            }
        }
    }
}

TEST_CASE("GpuConvolver full-tail oracle rejects planted mutations",
          "[gpu_audio][convolver][p2][oracle]") {
    constexpr std::size_t input_size = 384;
    const auto ir = make_oracle_ir();
    const auto input = make_oracle_input(2u, input_size);
    std::vector<std::vector<double>> reference(2u);
    std::vector<std::vector<float>> candidate(2u);
    for (std::uint32_t channel = 0; channel < 2u; ++channel) {
        reference[channel] = direct_convolution_full_tail(input[channel], ir);
        candidate[channel].assign(reference[channel].begin(), reference[channel].end());
    }
    REQUIRE(full_tail_matches(candidate, reference));

    SECTION("finite inverse FFT normalization") {
        for (auto& channel : candidate)
            for (float& sample : channel)
                sample /= 512.0f;
        REQUIRE_FALSE(full_tail_matches(candidate, reference));
    }
    SECTION("channel routing swap") {
        std::swap(candidate[0], candidate[1]);
        REQUIRE_FALSE(full_tail_matches(candidate, reference));
    }
    SECTION("one sample phase shift") {
        for (auto& channel : candidate)
            std::rotate(channel.rbegin(), channel.rbegin() + 1, channel.rend());
        REQUIRE_FALSE(full_tail_matches(candidate, reference));
    }
    SECTION("block order reversal") {
        constexpr std::size_t block = 64;
        for (auto& channel : candidate) {
            for (std::size_t first = 0, last = channel.size() - block; first < last;
                 first += block, last -= block) {
                std::swap_ranges(channel.begin() + first, channel.begin() + first + block,
                                 channel.begin() + last);
            }
        }
        REQUIRE_FALSE(full_tail_matches(candidate, reference));
    }
}

TEST_CASE("GpuConvolver delta fallback preserves the due block after a rejected callback",
          "[gpu_audio][convolver][transport][lifecycle]") {
    constexpr std::uint32_t BS = 32;
    for (const bool invalid_offline : {false, true}) {
        for (const bool short_output : {false, true}) {
            CAPTURE(invalid_offline, short_output);
            GpuConvolver node(1, BS, 48000, {1.0f});
            REQUIRE(node.prepare());
            GpuAudioTransport transport;
            REQUIRE(transport.prepare(&node, {.ring_blocks = 8}));
            std::vector<float> samples(BS, 0.0f), rendered(BS, 0.0f);
            const float* inputs[] = {samples.data()};
            float* outputs[] = {rendered.data()};
            BufferView<const float> input(inputs, 1, BS);
            BufferView<float> output(outputs, 1, BS);
            samples[0] = 1.0f;
            transport.process(input, output, BS); // absolute position zero
            std::fill(samples.begin(), samples.end(), 0.0f);
            std::fill(rendered.begin(), rendered.end(), 7.0f);
            BufferView<float> rejected_output(outputs, 1, short_output ? BS / 2 : BS);
            if (invalid_offline)
                transport.process_offline(input, rejected_output, short_output ? BS : BS / 2);
            else
                transport.process(input, rejected_output, short_output ? BS : BS / 2);
            for (std::uint32_t frame = 0; frame < rejected_output.num_samples(); ++frame)
                CHECK(rendered[frame] == 0.0f);
            if (short_output)
                CHECK(rendered.back() == 7.0f);   // never writes past a malformed view
            transport.process(input, output, BS); // position two: the impulse is due now
            CHECK(std::abs(rendered[0] - 1.0f) < 1.0e-5f);
            for (std::uint32_t frame = 1; frame < BS; ++frame)
                CHECK(std::abs(rendered[frame]) < 1.0e-5f);
            transport.process(input, output, BS); // no shifted impulse at position three
            for (float sample : rendered)
                CHECK(std::abs(sample) < 1.0e-5f);
        }
    }
}

TEST_CASE("GPU-off convolver nodes expose fail-closed provider status",
          "[gpu_audio][convolver][provider-boundary]") {
    GpuConvolver convolver(1, 32, 48000, {1.0f});
    REQUIRE(convolver.prepare());
    if (convolver.gpu_available())
        SKIP("GPU provider enabled");
    REQUIRE_FALSE(convolver.gpu_available());
    REQUIRE(convolver.backend().empty());

    // With no compute device the worker runs the exact CPU convolution: the
    // status names the CPU engine, an unknown provider and Degraded, never a
    // GPU kernel, and it passes the public validator.
    const auto status = convolver.status_snapshot();
    INFO("status error " << static_cast<int>(validate_gpu_audio_status(status).error));
    CHECK(validate_gpu_audio_status(status).accepted());
    CHECK(status.provider_state == GpuAudioProviderState::Degraded);
    CHECK(status.selected_engine == GpuAudioEngine::Cpu);
    CHECK(status.provider == GpuAudioProvider::Unknown);
    CHECK(status.fallback_available);
    CHECK_FALSE(status.gpu_kernel_available);
    CHECK(convolver.recovery_reason() == GpuAudioRecoveryReason::None);
    CHECK_FALSE(convolver.fenced());

    GpuConvolver unprepared(1, 32, 48000, {1.0f});
    CHECK(unprepared.status_snapshot().provider_state == GpuAudioProviderState::Uninitialized);
    CHECK(validate_gpu_audio_status(unprepared.status_snapshot()).accepted());
    CHECK(unprepared.fenced());

    GpuMultiConvolver multi(32, 48000, {{1.0f}});
    REQUIRE_FALSE(multi.prepare());
    REQUIRE_FALSE(multi.gpu_available());
    REQUIRE(multi.backend().empty());
    const GpuAudioNode& multi_node = multi;
    const auto multi_status = multi_node.status_snapshot();
    CHECK(multi_status.provider_state == GpuAudioProviderState::Uninitialized);
    CHECK(validate_gpu_audio_status(multi_status).accepted());
}

namespace {

using pulp::gpu_audio::detail::GpuConvolverTestAccess;
using pulp::test::LossProvider;

constexpr std::uint32_t kHealthBlock = 2;

// A prepared convolver whose provider is the deterministic loss provider.
// Returns null (and the test skips) when shared-I/O state is not compiled.
std::unique_ptr<GpuConvolver> convolver_with_loss_provider(LossProvider*& provider) {
    auto convolver =
        std::make_unique<GpuConvolver>(1, kHealthBlock, 48000, std::vector<float>{1.0f});
    REQUIRE(convolver->prepare());
    if (!pulp::test::install_loss_session(*convolver, kHealthBlock, provider))
        return nullptr;
    return convolver;
}

} // namespace

// Every status a health test observes must satisfy the v3 invariants.
pulp::gpu_audio::GpuAudioStatus validated(const pulp::gpu_audio::GpuAudioStatus& status) {
    const auto validation = pulp::gpu_audio::validate_gpu_audio_status(status);
    INFO("status error " << static_cast<int>(validation.error));
    CHECK(validation.accepted());
    return status;
}

TEST_CASE("GpuConvolver status is Uninitialized before prepare and fenced",
          "[gpu_audio][convolver][status]") {
    GpuConvolver convolver(1, 32, 48000, {1.0f});
    const auto status = validated(convolver.status_snapshot());
    CHECK(status.schema_version == GpuAudioStatus::kSchemaVersion);
    CHECK(status.provider_state == GpuAudioProviderState::Uninitialized);
    CHECK(convolver.recovery_reason() == GpuAudioRecoveryReason::None);
    CHECK(convolver.fenced());
}

TEST_CASE("GpuConvolver shared-path status reports provider loss through the transport",
          "[gpu_audio][convolver][status][health]") {
    LossProvider* provider = nullptr;
    auto convolver = convolver_with_loss_provider(provider);
    if (!convolver)
        SKIP("shared-I/O provider state is not compiled into this build");

    // The node path the transport captures carries the health hook.
    const auto path = pulp::gpu_audio::detail::realtime_gpu_node_path(convolver.get());
    REQUIRE(path.active());
    REQUIRE(path.health != nullptr);

    GpuAudioTransport transport;
    REQUIRE(transport.prepare(convolver.get(), {8}));
    REQUIRE(transport.capability_report().provider == GpuAudioProvider::Dawn);

    // Positive control: an authenticated, unfenced session is Ready on both
    // the transport and the node.
    auto status = validated(transport.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Ready);
    CHECK(status.selected_engine == GpuAudioEngine::Gpu);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::None);
    CHECK(status.delivery_epoch_active);
    CHECK(status.epoch != 0);
    const auto prepared_epoch = status.epoch;
    CHECK(validated(convolver->status_snapshot()).provider_state == GpuAudioProviderState::Ready);
    CHECK_FALSE(convolver->fenced());

    provider->lost = true;
    transport.pump();

    status = validated(transport.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Unavailable);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::ProviderLost);
    CHECK(status.epoch == prepared_epoch);
    CHECK(status.selected_engine == GpuAudioEngine::Cpu);
    CHECK_FALSE(status.delivery_epoch_active);

    const auto node_status = validated(convolver->status_snapshot());
    CHECK(node_status.provider_state == GpuAudioProviderState::Unavailable);
    CHECK(node_status.recovery_reason == GpuAudioRecoveryReason::ProviderLost);
    CHECK(node_status.provider == GpuAudioProvider::Dawn);
    CHECK(node_status.selected_engine == GpuAudioEngine::Cpu);
    CHECK(node_status.fallback_policy == MissPolicy::CpuFallback);
    CHECK(node_status.fallback_available);
    CHECK(convolver->recovery_reason() == GpuAudioRecoveryReason::ProviderLost);
    CHECK(convolver->fenced());

    // Host/UI reads stay allocation-free on the fenced path.
    std::size_t allocations = 0;
    GpuAudioStatus hosted_status;
    GpuAudioStatus node_read;
    {
        pulp::test::RtAllocationProbe probe;
        hosted_status = transport.status_snapshot();
        node_read = convolver->status_snapshot();
        (void)convolver->fenced();
        allocations = probe.allocation_count();
    }
    // Validate outside the probe window: Catch's message streams allocate on
    // first use, which is not the code under measurement.
    (void)validated(hosted_status);
    (void)validated(node_read);
    CHECK(allocations == 0);
    transport.release();
}

TEST_CASE("GpuConvolver shared-path status counts a refused GPU block as CPU fallback",
          "[gpu_audio][convolver][status][health]") {
    LossProvider* provider = nullptr;
    auto convolver = convolver_with_loss_provider(provider);
    if (!convolver)
        SKIP("shared-I/O provider state is not compiled into this build");
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(convolver.get(), {8}));
    auto status = validated(transport.status_snapshot());
    REQUIRE(status.provider_state == GpuAudioProviderState::Ready);
    for (const auto count : status.terminal_counts)
        CHECK(count == 0);

    // One callback block is admitted; the idle program refuses its
    // submission, which is one terminal per admitted block.
    std::vector<float> in(kHealthBlock, 0.5f), out(kHealthBlock, 0.0f);
    const float* in_ptrs[] = {in.data()};
    float* out_ptrs[] = {out.data()};
    BufferView<const float> input(in_ptrs, 1, kHealthBlock);
    BufferView<float> output(out_ptrs, 1, kHealthBlock);
    transport.process(input, output, kHealthBlock);
    transport.pump();
    // Run until that block's slot is due, so the CPU fallback has delivered
    // the block the GPU refused (a terminal count never runs ahead of the
    // deliveries it describes).
    for (std::uint32_t block = 0; block < GpuConvolver::kLatencyBlocks + 1; ++block) {
        transport.process(input, output, kHealthBlock);
        transport.pump();
    }

    status = validated(transport.status_snapshot());
    CHECK(status.fallback_blocks >= 1);
    CHECK(status.terminal_counts[static_cast<std::size_t>(
              GpuAudioTerminalDisposition::CpuFallback)] == 1);
    CHECK(status.terminal_counts[static_cast<std::size_t>(
              GpuAudioTerminalDisposition::GpuDelivered)] == 0);
    CHECK(status.provider_state == GpuAudioProviderState::Degraded);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::ProviderFailure);
    // Without tracing the node keeps no delivery counters of its own, so it
    // leaves terminal counts to the transport rather than over-report them.
    CHECK(
        validated(convolver->status_snapshot())
            .terminal_counts[static_cast<std::size_t>(GpuAudioTerminalDisposition::CpuFallback)] ==
        0);
    transport.release();
}

TEST_CASE("GpuConvolver shared-path status latches the offline fence as Degraded",
          "[gpu_audio][convolver][status][health]") {
    LossProvider* provider = nullptr;
    auto convolver = convolver_with_loss_provider(provider);
    if (!convolver)
        SKIP("shared-I/O provider state is not compiled into this build");
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(convolver.get(), {8}));
    std::vector<float> in(kHealthBlock, 0.5f), out(kHealthBlock, 0.0f);
    const float* in_ptrs[] = {in.data()};
    float* out_ptrs[] = {out.data()};
    BufferView<const float> input(in_ptrs, 1, kHealthBlock);
    BufferView<float> output(out_ptrs, 1, kHealthBlock);
    transport.process_offline(input, output, kHealthBlock);
    const auto status = validated(transport.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Degraded);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::OfflineFence);
    CHECK(convolver->fenced());
    transport.release();
}

TEST_CASE("GpuConvolver staged status is honest and reports a lost device",
          "[gpu_audio][convolver][status][health]") {
    GpuConvolver convolver(1, 32, 48000, {1.0f});
    REQUIRE(convolver.set_provider_policy(GpuConvolver::ProviderPolicy::StagedOnly));
    REQUIRE(convolver.prepare());
    auto* device = GpuConvolverTestAccess::staged_compute(convolver);
    if (device == nullptr)
        SKIP(kNoGpu);

    // Positive control: a live staged device proves neither the provider nor
    // the engine, and nothing has been latched.
    auto status = validated(convolver.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Degraded);
    CHECK(status.provider == GpuAudioProvider::Unknown);
    CHECK(status.selected_engine == GpuAudioEngine::Unknown);
    CHECK(status.requested_engine == GpuAudioEngine::Gpu);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::None);
    CHECK(status.prepared_lead_blocks == 0);
    CHECK(status.latency_samples == GpuConvolver::kLatencyBlocks * 32u);
    CHECK_FALSE(convolver.fenced());

    // The transport folds the staged node's own status in.
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&convolver, {8}));
    CHECK(validated(transport.status_snapshot()).provider_state == GpuAudioProviderState::Degraded);

    device->notify_device_lost();
    status = validated(convolver.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Unavailable);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::ProviderLost);
    CHECK(convolver.fenced());
    // The transport worker samples the staged node's status on its next pump.
    transport.pump();
    const auto hosted = validated(transport.status_snapshot());
    CHECK(hosted.provider_state == GpuAudioProviderState::Unavailable);
    CHECK(hosted.recovery_reason == GpuAudioRecoveryReason::ProviderLost);
    transport.release();

    // With no device the worker runs the exact CPU convolution, and says so.
    GpuConvolverTestAccess::install_staged_compute(convolver, nullptr);
    status = validated(convolver.status_snapshot());
    CHECK(status.selected_engine == GpuAudioEngine::Cpu);
    CHECK(status.provider_state == GpuAudioProviderState::Degraded);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::None);
}

TEST_CASE("GpuMultiConvolver status declares silence with no fallback",
          "[gpu_audio][convolver][status]") {
    GpuMultiConvolver multi(32, 48000, {{1.0f}, {0.5f}});
    auto status = validated(multi.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Uninitialized);
    CHECK(multi.fenced());
    if (!multi.prepare())
        SKIP(kNoGpu);
    status = validated(multi.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Degraded);
    CHECK(status.fallback_policy == MissPolicy::Silence);
    CHECK_FALSE(status.fallback_available);
    CHECK(status.provider == GpuAudioProvider::Unknown);
    CHECK(status.selected_engine == GpuAudioEngine::Unknown);
    CHECK(status.latency_samples == 64u);
    CHECK(multi.recovery_reason() == GpuAudioRecoveryReason::None);
    CHECK_FALSE(multi.fenced());
}

#if PULP_TEST_HAS_GPU_COMPUTE
// ── Staged worker path against a scripted device ────────────────────────────
//
// These run the real GpuConvolver::process_block against FakeGpuCompute, so
// device loss, late results and a never-resolving provider are reproducible on
// any host. The fake returns the exact linear convolution scaled by a marker
// gain (1.01), so the node's output pins exactly which input blocks the GPU
// carried and which the worker's CPU convolution carried:
//   expected = 1.01 * conv(GPU blocks) + conv(CPU blocks),
// delayed by the node's worker pipeline (one block on the default path).
namespace {

using pulp::test::FakeGpuCompute;
constexpr float kGpuMark = 1.01f;

class InjectableConvolver : public GpuConvolver {
  public:
    using GpuConvolver::GpuConvolver;
    using GpuConvolver::staged_breaker_trips;
    using GpuConvolver::staged_cpu_blocks;
    using GpuConvolver::staged_gpu_blocks;
    using GpuConvolver::staged_latched_reason;
    FakeGpuCompute* adopt_fake(std::vector<float> ir, float gain = kGpuMark) {
        auto fake = std::make_unique<FakeGpuCompute>(std::move(ir));
        fake->set_result_gain(gain);
        auto* raw = fake.get();
        gpu_ = std::move(fake);
        // The pipeline depth follows the prepared device; re-derive it for the
        // scripted one so the result does not depend on the host's adapter.
        configure_staged_pipeline();
        return raw;
    }
    std::uint32_t pipeline() const {
        return descriptor().worker_pipeline_blocks;
    }
};

std::vector<float> staged_test_ir() {
    std::vector<float> ir(150);
    for (std::size_t i = 0; i < ir.size(); ++i)
        ir[i] = static_cast<float>(std::cos(0.07 * i) * std::exp(-0.015 * i));
    return ir;
}

std::vector<float> staged_test_input(std::size_t size) {
    std::vector<float> x(size);
    for (std::size_t i = 0; i < size; ++i)
        x[i] = static_cast<float>(std::sin(0.09 * i) + 0.25 * std::sin(0.31 * i));
    return x;
}

// Expected output when the blocks in `cpu_blocks` were convolved on the CPU and
// every other block by the (marked) GPU, delayed by `delay_blocks`.
std::vector<float> routed_reference(const std::vector<float>& input, const std::vector<float>& ir,
                                    std::uint32_t block, const std::vector<bool>& cpu_blocks,
                                    std::uint32_t delay_blocks, float gpu_gain = kGpuMark) {
    std::vector<float> gpu_part(input.size(), 0.0f), cpu_part(input.size(), 0.0f);
    for (std::size_t i = 0; i < input.size(); ++i)
        (cpu_blocks[i / block] ? cpu_part : gpu_part)[i] = input[i];
    const auto g = direct_convolution(gpu_part, ir);
    const auto c = direct_convolution(cpu_part, ir);
    const std::size_t delay = static_cast<std::size_t>(delay_blocks) * block;
    std::vector<float> out(input.size(), 0.0f);
    for (std::size_t i = delay; i < out.size(); ++i)
        out[i] = gpu_gain * g[i - delay] + c[i - delay];
    return out;
}

// This thread's CPU time. Unlike the wall clock it does not grow when a loaded
// host preempts the thread. Falls back to the wall clock where no per-thread
// clock exists.
std::chrono::nanoseconds thread_cpu_now() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
#else
    return std::chrono::steady_clock::now().time_since_epoch();
#endif
}

// Drive `node` block by block over a mono stream; `before_block(b)` runs before
// block b so a test can change the fake's behaviour mid-stream.
template <typename BeforeBlock>
std::vector<float> run_staged_stream(GpuConvolver& node, const std::vector<float>& input,
                                     std::uint32_t block, BeforeBlock before_block,
                                     std::vector<std::chrono::nanoseconds>* elapsed = nullptr,
                                     std::vector<std::chrono::nanoseconds>* cpu = nullptr) {
    std::vector<float> out(input.size(), -9.0f);
    for (std::size_t b = 0; b * block < input.size(); ++b) {
        before_block(b);
        const float* in_ptr[1] = {input.data() + b * block};
        float* out_ptr[1] = {out.data() + b * block};
        BufferView<const float> iv(in_ptr, 1, block);
        BufferView<float> ov(out_ptr, 1, block);
        const auto t0 = std::chrono::steady_clock::now();
        const auto c0 = thread_cpu_now();
        node.process_block(iv, ov, block);
        if (elapsed)
            elapsed->push_back(std::chrono::steady_clock::now() - t0);
        if (cpu)
            cpu->push_back(thread_cpu_now() - c0);
    }
    return out;
}

std::size_t first_mismatch(const std::vector<float>& got, const std::vector<float>& ref) {
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (!(std::abs(got[i] - ref[i]) < 1.0e-4f * (1.0f + std::abs(ref[i]))))
            return i;
    }
    return got.size();
}

std::vector<bool> blocks_where(std::size_t count, const std::function<bool(std::size_t)>& pred) {
    std::vector<bool> v(count);
    for (std::size_t b = 0; b < count; ++b)
        v[b] = pred(b);
    return v;
}

// Records which blocks reached the device as accepted submits.
struct SubmitLog {
    std::vector<std::size_t> blocks;
    std::uint64_t seen = 0;
    void before(std::size_t b, const FakeGpuCompute& fake) {
        if (b > 0 && fake.async_submits() != seen)
            blocks.push_back(b - 1);
        seen = fake.async_submits();
    }
    void finish(std::size_t nblk, const FakeGpuCompute& fake) {
        if (fake.async_submits() != seen)
            blocks.push_back(nblk - 1);
    }
};

} // namespace

TEST_CASE("Staged GpuConvolver hits come from the GPU without a CPU convolution",
          "[gpu_audio][convolver][staged]") {
    constexpr std::uint32_t BS = 64;
    constexpr std::size_t NBLK = 24;
    const auto ir = staged_test_ir();
    InjectableConvolver node(1, BS, 48000, ir);
    REQUIRE(node.prepare());
    auto* fake = node.adopt_fake(ir);
    REQUIRE(node.pipeline() == 1u);

    const auto input = staged_test_input(BS * NBLK);
    const auto out = run_staged_stream(node, input, BS, [](std::size_t) {});

    // Every block is the marked GPU result one block late; none was also
    // convolved on the CPU (that would add an unmarked copy on top).
    CHECK(first_mismatch(out, routed_reference(input, ir, BS, std::vector<bool>(NBLK, false), 1)) ==
          out.size());
    // Positive control for the marker: it is visible against the plain result.
    CHECK(first_mismatch(out, routed_reference(input, ir, BS, std::vector<bool>(NBLK, true), 1)) <
          out.size());
    CHECK(fake->async_submits() == NBLK);
    CHECK(fake->convolve_calls() == 0u);
    CHECK(node.staged_cpu_blocks() == 0u);
    CHECK(node.staged_gpu_blocks() == NBLK - 1u); // the last block is not due yet
    CHECK(node.staged_breaker_trips() == 0u);
    CHECK(node.staged_latched_reason() == GpuAudioRecoveryReason::None);
    CHECK(validate_gpu_audio_status(node.status_snapshot()).accepted());
}

TEST_CASE("Staged GpuConvolver routes missed blocks, and only those, to the CPU",
          "[gpu_audio][convolver][staged]") {
    constexpr std::uint32_t BS = 64;
    constexpr std::size_t NBLK = 32;
    const auto ir = staged_test_ir();
    InjectableConvolver node(1, BS, 48000, ir);
    REQUIRE(node.prepare());
    auto* fake = node.adopt_fake(ir);

    const auto input = staged_test_input(BS * NBLK);
    const auto is_miss = [](std::size_t b) { return b == 6 || b == 12 || b == 13; };
    const auto expected = routed_reference(input, ir, BS, blocks_where(NBLK, is_miss), 1);

    SECTION("expired results are served misses, not a recovery cause") {
        const auto out = run_staged_stream(node, input, BS, [&](std::size_t b) {
            fake->set_async_mode(is_miss(b) ? FakeGpuCompute::AsyncMode::Expire
                                            : FakeGpuCompute::AsyncMode::Resolve);
        });
        CHECK(first_mismatch(out, expected) == out.size());
        CHECK(node.staged_latched_reason() == GpuAudioRecoveryReason::None);
        CHECK(validate_gpu_audio_status(node.status_snapshot()).accepted());
        // The 150-tap IR's tail spans 3 more 64-sample blocks, so the CPU runs
        // for 6..9 and 12..16 and nowhere else.
        CHECK(node.staged_cpu_blocks() == 9u);
    }
    SECTION("a failed map latches ProviderFailure") {
        const auto out = run_staged_stream(node, input, BS, [&](std::size_t b) {
            fake->set_async_mode(b == 13      ? FakeGpuCompute::AsyncMode::Fail
                                 : is_miss(b) ? FakeGpuCompute::AsyncMode::Expire
                                              : FakeGpuCompute::AsyncMode::Resolve);
        });
        CHECK(first_mismatch(out, expected) == out.size());
        CHECK(node.staged_latched_reason() == GpuAudioRecoveryReason::ProviderFailure);
        CHECK(validate_gpu_audio_status(node.status_snapshot()).accepted());
    }
    SECTION("a non-finite readback routes the whole block to the CPU") {
        const auto out = run_staged_stream(node, input, BS, [&](std::size_t b) {
            fake->set_async_mode(is_miss(b) && b != 12 ? FakeGpuCompute::AsyncMode::Expire
                                                       : FakeGpuCompute::AsyncMode::Resolve);
            if (b == 12)
                fake->poison_next_result();
        });
        CHECK(first_mismatch(out, expected) == out.size());
        CHECK(node.staged_latched_reason() == GpuAudioRecoveryReason::ProviderFailure);
        CHECK(validate_gpu_audio_status(node.status_snapshot()).accepted());
    }
}

TEST_CASE("Staged GpuConvolver discards a result that lands after its block was due",
          "[gpu_audio][convolver][staged]") {
    constexpr std::uint32_t BS = 64;
    constexpr std::size_t NBLK = 16, kLate = 6;
    const auto ir = staged_test_ir();
    InjectableConvolver node(1, BS, 48000, ir);
    REQUIRE(node.prepare());
    auto* fake = node.adopt_fake(ir);
    // The worker polls once per call. Block 6's map lands on the third poll
    // after its submit, i.e. in the call after the one where it was due.
    fake->set_delay_polls(3);

    const auto input = staged_test_input(BS * NBLK);
    const auto out = run_staged_stream(node, input, BS, [&](std::size_t b) {
        fake->set_async_mode(b == kLate ? FakeGpuCompute::AsyncMode::Delayed
                                        : FakeGpuCompute::AsyncMode::Resolve);
    });

    // Block 6 is still in flight when due, so it is served on the CPU and its
    // successful late completion is ignored. In-order completion holds block 7
    // behind it until that same later poll, which is still in time for 7.
    const auto cpu = blocks_where(NBLK, [](std::size_t b) { return b == kLate; });
    CHECK(first_mismatch(out, routed_reference(input, ir, BS, cpu, 1)) == out.size());
    CHECK(fake->async_submits() == NBLK);
    CHECK(node.staged_cpu_blocks() == 4u); // block 6 plus its 3-block IR tail
    CHECK(node.staged_latched_reason() == GpuAudioRecoveryReason::None);
    CHECK(validate_gpu_audio_status(node.status_snapshot()).accepted());
}

TEST_CASE("Staged GpuConvolver keeps a readback slower than one period but inside the lead",
          "[gpu_audio][convolver][staged]") {
    constexpr std::uint32_t BS = 64;
    constexpr std::size_t NBLK = 24;
    const auto ir = staged_test_ir();
    const auto input = staged_test_input(BS * NBLK);

    // Every readback lands on the third poll after its submit: two calls after
    // the block, i.e. later than one period. The worker polls once per call.
    auto run = [&](std::uint32_t lead, std::uint64_t& cpu_blocks, GpuAudioRecoveryReason& reason) {
        InjectableConvolver node(1, BS, 48000, ir, lead);
        REQUIRE(node.prepare());
        auto* fake = node.adopt_fake(ir);
        fake->set_async_mode(FakeGpuCompute::AsyncMode::Delayed);
        fake->set_delay_polls(3);
        REQUIRE(node.pipeline() == lead - 1u);
        const auto out = run_staged_stream(node, input, BS, [](std::size_t) {});
        cpu_blocks = node.staged_cpu_blocks();
        reason = node.staged_latched_reason();
        CHECK(validate_gpu_audio_status(node.status_snapshot()).accepted());
        return out;
    };

    // Lead 3 (pipeline 2): the result is due two calls after submit, so every
    // block is the marked GPU result, with no CPU block and no breaker trip.
    std::uint64_t cpu_blocks = 0;
    GpuAudioRecoveryReason reason{};
    const auto out = run(3, cpu_blocks, reason);
    CHECK(first_mismatch(out, routed_reference(input, ir, BS, std::vector<bool>(NBLK, false), 2)) ==
          out.size());
    CHECK(cpu_blocks == 0u);
    CHECK(reason == GpuAudioRecoveryReason::None);

    // Positive control: the same readback is late for a lead of 2 (due one
    // call after submit), so it misses and eventually trips the breaker.
    std::uint64_t short_cpu = 0;
    GpuAudioRecoveryReason short_reason{};
    const auto short_out = run(2, short_cpu, short_reason);
    CHECK(short_cpu > 0u);
    CHECK(short_reason == GpuAudioRecoveryReason::SustainedDeadlineMisses);
    CHECK(first_mismatch(short_out, routed_reference(input, ir, BS, std::vector<bool>(NBLK, false),
                                                     1)) < short_out.size());
}

TEST_CASE("Staged GpuConvolver continues on the CPU after device loss",
          "[gpu_audio][convolver][staged]") {
    constexpr std::uint32_t BS = 64;
    constexpr std::size_t NBLK = 24, kLossBlock = 9;
    const auto ir = staged_test_ir();
    InjectableConvolver node(1, BS, 48000, ir);
    REQUIRE(node.prepare());
    auto* fake = node.adopt_fake(ir);

    const auto input = staged_test_input(BS * NBLK);
    const auto out = run_staged_stream(node, input, BS, [&](std::size_t b) {
        if (b == kLossBlock)
            fake->lose_device();
    });

    // GPU up to the loss, CPU from the loss block on, with the GPU carry's tail
    // of the earlier blocks still delivered: no silent block, no seam.
    const auto cpu = blocks_where(NBLK, [](std::size_t b) { return b >= kLossBlock; });
    CHECK(first_mismatch(out, routed_reference(input, ir, BS, cpu, 1)) == out.size());
    CHECK(fake->async_submits() == kLossBlock); // a lost device is never submitted to
    CHECK(node.staged_latched_reason() == GpuAudioRecoveryReason::ProviderLost);
    CHECK(validate_gpu_audio_status(node.status_snapshot()).accepted());

    // prepare() clears the latch for the next device.
    REQUIRE(node.prepare());
    CHECK(node.staged_latched_reason() == GpuAudioRecoveryReason::None);
    CHECK(validate_gpu_audio_status(node.status_snapshot()).accepted());
}

TEST_CASE("Staged GpuConvolver breaker trips on intermittent lateness and recovers",
          "[gpu_audio][convolver][staged]") {
    constexpr std::uint32_t BS = 64;
    constexpr std::size_t NBLK = 64, kRecoverAt = 20;
    const auto ir = staged_test_ir();
    InjectableConvolver node(1, BS, 48000, ir);
    REQUIRE(node.configure_staged_breaker(
        {.trip_after_misses = 3, .probe_interval_blocks = 5, .recover_after_hits = 2}));
    REQUIRE(node.prepare());
    auto* fake = node.adopt_fake(ir);

    SubmitLog log;
    const auto input = staged_test_input(BS * NBLK);
    const auto out = run_staged_stream(node, input, BS, [&](std::size_t b) {
        log.before(b, *fake);
        fake->set_async_mode(b < kRecoverAt ? FakeGpuCompute::AsyncMode::Expire
                                            : FakeGpuCompute::AsyncMode::Resolve);
    });
    log.finish(NBLK, *fake);

    // Results are judged one block after submit. Misses of 0, 1, 2 trip the
    // breaker while 3 is already in flight; then one probe per 5 blocks: 8,
    // 13, 18 miss, 23 lands (probation; 24 was decided before that verdict),
    // 25 lands (recovered), and every later block submits.
    std::vector<std::size_t> expected_submits = {0, 1, 2, 3, 8, 13, 18, 23};
    for (std::size_t b = 25; b < NBLK; ++b)
        expected_submits.push_back(b);
    CHECK(log.blocks == expected_submits);

    const auto cpu = blocks_where(NBLK, [](std::size_t b) { return b < 23 || b == 24; });
    CHECK(first_mismatch(out, routed_reference(input, ir, BS, cpu, 1)) == out.size());
    CHECK(node.staged_latched_reason() == GpuAudioRecoveryReason::SustainedDeadlineMisses);
    CHECK(validate_gpu_audio_status(node.status_snapshot()).accepted());
}

TEST_CASE("Staged GpuConvolver worker never waits under constant expiry",
          "[gpu_audio][convolver][staged]") {
    constexpr std::uint32_t BS = 64, SR = 48000;
    constexpr std::size_t NBLK = 48;
    const auto period = std::chrono::nanoseconds(std::uint64_t{BS} * 1'000'000'000u / SR);
    const auto ir = staged_test_ir();
    InjectableConvolver node(1, BS, SR, ir);
    REQUIRE(node.prepare()); // default breaker: trips after 8 misses
    auto* fake = node.adopt_fake(ir);

    for (const auto mode : {FakeGpuCompute::AsyncMode::Late, FakeGpuCompute::AsyncMode::Hang}) {
        REQUIRE(node.prepare());
        fake = node.adopt_fake(ir);
        fake->set_async_mode(mode);
        const auto input = staged_test_input(BS * NBLK);
        std::vector<std::chrono::nanoseconds> elapsed, cpu;
        const auto out = run_staged_stream(node, input, BS, [](std::size_t) {}, &elapsed, &cpu);

        // Thread CPU time per block stays well inside one period: there is no
        // wait to spend it in. The wall-clock ceiling only rules out a block
        // that waited (preemption on a shared host can stretch wall time).
        const auto worst_cpu = *std::max_element(cpu.begin(), cpu.end());
        const auto worst_wall = *std::max_element(elapsed.begin(), elapsed.end());
        INFO("mode " << static_cast<int>(mode) << ": worst cpu " << worst_cpu.count()
                     << " ns, worst wall " << worst_wall.count() << " ns, period " << period.count()
                     << " ns");
        CHECK(worst_cpu < period);
        CHECK(worst_wall < 10 * period);
        CHECK(first_mismatch(out, routed_reference(input, ir, BS, std::vector<bool>(NBLK, true),
                                                   1)) == out.size());
        CHECK(node.staged_latched_reason() == GpuAudioRecoveryReason::SustainedDeadlineMisses);
        CHECK(validate_gpu_audio_status(node.status_snapshot()).accepted());
        CHECK(node.staged_breaker_trips() == 1u);
        CHECK(node.staged_gpu_blocks() == 0u);
        // A tripped breaker is a degraded, CPU-delivering node, not a lost one.
        const auto status = node.status_snapshot();
        CHECK(status.provider_state == GpuAudioProviderState::Degraded);
        CHECK(status.recovery_reason == GpuAudioRecoveryReason::SustainedDeadlineMisses);
        CHECK(status.selected_engine == GpuAudioEngine::Cpu);
        // Accepted submits stop at the provider's in-flight cap or the trip.
        CHECK(fake->async_submits() <= 10u);
    }
}

TEST_CASE("Staged GpuConvolver with a one-block lead decides each block with a bounded wait",
          "[gpu_audio][convolver][staged]") {
    constexpr std::uint32_t BS = 64, SR = 48000;
    constexpr std::size_t NBLK = 12;
    const auto period = std::chrono::nanoseconds(std::uint64_t{BS} * 1'000'000'000u / SR);
    const auto ir = staged_test_ir();
    InjectableConvolver node(1, BS, SR, ir, /*latency_blocks=*/1);
    REQUIRE(node.prepare());
    auto* fake = node.adopt_fake(ir);
    REQUIRE(node.pipeline() == 0u); // no room in a one-block lead to pipeline
    fake->set_async_mode(FakeGpuCompute::AsyncMode::Hang);

    const auto input = staged_test_input(BS * NBLK);
    std::vector<std::chrono::nanoseconds> elapsed, cpu;
    const auto out = run_staged_stream(node, input, BS, [](std::size_t) {}, &elapsed, &cpu);

    const auto worst_cpu = *std::max_element(cpu.begin(), cpu.end());
    INFO("worst cpu " << worst_cpu.count() << " ns, period " << period.count() << " ns");
    CHECK(worst_cpu < period);
    // Positive control: this path really waits on the provider.
    CHECK(elapsed.front() >= std::chrono::microseconds(100));
    CHECK(fake->async_submits() == 4u); // then the in-flight cap rejects up front
    CHECK(first_mismatch(out, routed_reference(input, ir, BS, std::vector<bool>(NBLK, true), 0)) ==
          out.size());
}

TEST_CASE("StagedBlocking keeps the blocking readback and still covers failures",
          "[gpu_audio][convolver][staged]") {
    constexpr std::uint32_t BS = 64;
    constexpr std::size_t NBLK = 24;
    const auto ir = staged_test_ir();
    InjectableConvolver node(1, BS, 48000, ir);
    REQUIRE(node.set_provider_policy(GpuConvolver::ProviderPolicy::StagedBlocking));
    REQUIRE(node.prepare());
    auto* fake = node.adopt_fake(ir);
    REQUIRE(node.pipeline() == 0u);

    const auto input = staged_test_input(BS * NBLK);
    const auto out = run_staged_stream(node, input, BS, [&](std::size_t b) {
        fake->set_blocking_ok(b != 5);
        if (b == 15)
            fake->lose_device();
    });

    const auto cpu = blocks_where(NBLK, [](std::size_t b) { return b == 5 || b >= 15; });
    CHECK(first_mismatch(out, routed_reference(input, ir, BS, cpu, 0)) == out.size());
    CHECK(fake->async_submits() == 0u);
    CHECK(fake->convolve_calls() == 15u);
    CHECK(node.staged_latched_reason() == GpuAudioRecoveryReason::ProviderLost);
    CHECK(validate_gpu_audio_status(node.status_snapshot()).accepted());
}

TEST_CASE("Staged GpuConvolver validates sample rate and breaker settings",
          "[gpu_audio][convolver][staged]") {
    GpuConvolver zero_rate(1, 64, 0, staged_test_ir());
    CHECK_FALSE(zero_rate.prepare());

    GpuConvolver node(1, 64, 48000, staged_test_ir());
    CHECK_FALSE(node.configure_staged_breaker({.trip_after_misses = 0}));
    CHECK_FALSE(node.configure_staged_breaker({.probe_interval_blocks = 0}));
    CHECK_FALSE(node.configure_staged_breaker({.recover_after_hits = 0}));
    REQUIRE(node.configure_staged_breaker({.trip_after_misses = 2}));
    CHECK(node.staged_breaker().trip_after_misses == 2u);
    REQUIRE(node.prepare());
    CHECK_FALSE(node.configure_staged_breaker({}));
    CHECK(node.staged_breaker().trip_after_misses == 2u); // survives prepare()
}

namespace {
// Drive `node` through a transport block by block. `pump_block(b)` says whether
// the worker runs after callback b (false leaves that block to the callback's
// CPU fallback).
template <typename PumpBlock>
std::vector<float> run_transport(GpuAudioTransport& transport, const std::vector<float>& input,
                                 std::uint32_t block, PumpBlock pump_block,
                                 const std::function<void(std::size_t)>& before = {}) {
    std::vector<float> out(input.size(), -9.0f);
    for (std::size_t b = 0; b * block < input.size(); ++b) {
        if (before)
            before(b);
        const float* in_ptr[1] = {input.data() + b * block};
        float* out_ptr[1] = {out.data() + b * block};
        BufferView<const float> iv(in_ptr, 1, block);
        BufferView<float> ov(out_ptr, 1, block);
        transport.process(iv, ov, block);
        if (pump_block(b))
            transport.pump();
    }
    return out;
}

class PipelineDeclaringNode : public GpuAudioNode {
  public:
    PipelineDeclaringNode(std::uint32_t latency, std::uint32_t pipeline)
        : latency_(latency), pipeline_(pipeline) {}
    GpuAudioNodeDescriptor descriptor() const override {
        GpuAudioNodeDescriptor d;
        d.input_channels = d.output_channels = 1;
        d.block_size = 64;
        d.sample_rate = 48000;
        d.latency_blocks = latency_;
        d.worker_pipeline_blocks = pipeline_;
        return d;
    }
    bool prepare() override {
        return true;
    }
    void process_block(const BufferView<const float>&, BufferView<float>& out,
                       std::uint32_t) override {
        out.clear();
    }

  private:
    std::uint32_t latency_;
    std::uint32_t pipeline_;
};
} // namespace

TEST_CASE("Transport total latency is exactly latency_blocks with and without a worker pipeline",
          "[gpu_audio][convolver][staged][transport]") {
    constexpr std::uint32_t BS = 64;
    constexpr std::size_t NBLK = 32;
    const auto ir = staged_test_ir();
    const auto input = staged_test_input(BS * NBLK);
    const auto ref = direct_convolution(input, ir);

    for (const auto policy :
         {GpuConvolver::ProviderPolicy::Auto, GpuConvolver::ProviderPolicy::StagedBlocking}) {
        InjectableConvolver node(1, BS, 48000, ir);
        REQUIRE(node.set_provider_policy(policy));
        REQUIRE(node.prepare());
        node.adopt_fake(ir, 1.0f);
        const auto pipeline = node.pipeline();
        CHECK(pipeline == (policy == GpuConvolver::ProviderPolicy::Auto ? 1u : 0u));

        GpuAudioTransport transport;
        REQUIRE(transport.prepare(&node, {/*ring_blocks=*/8}));
        // The PDC is the declared lead, pipeline or not.
        REQUIRE(transport.latency_samples() == GpuConvolver::kLatencyBlocks * BS);
        const std::uint32_t L = GpuConvolver::kLatencyBlocks;

        SECTION(std::string("worker keeps pace, pipeline ") + std::to_string(pipeline)) {
            const auto out = run_transport(transport, input, BS, [](std::size_t) { return true; });
            for (std::size_t i = 0; i < out.size(); ++i) {
                const float expected = i < L * BS ? 0.0f : ref[i - L * BS];
                REQUIRE(std::abs(out[i] - expected) < 1.0e-3f * (1.0f + std::abs(expected)));
            }
            const auto d = transport.delivery_snapshot();
            CHECK(d.worker_output_blocks == NBLK - L);
            CHECK(d.cpu_fallback_blocks == 0u);
        }
        SECTION(std::string("worker skips blocks, pipeline ") + std::to_string(pipeline)) {
            // Twice every 6 blocks the worker does not run in time (two in a row
            // outlasts the primed slack with or without a pipeline), so the
            // callback substitutes its own CPU fallback. Both paths must sit on
            // the same timeline: the stream stays the reference delayed by L.
            const auto out =
                run_transport(transport, input, BS, [](std::size_t b) { return b % 6 < 4; });
            for (std::size_t i = 0; i < out.size(); ++i) {
                const float expected = i < L * BS ? 0.0f : ref[i - L * BS];
                REQUIRE(std::abs(out[i] - expected) < 1.0e-3f * (1.0f + std::abs(expected)));
            }
            const auto d = transport.delivery_snapshot();
            CHECK(d.worker_output_blocks > 0u);
            CHECK(d.cpu_fallback_blocks > 0u);
        }
    }
}

TEST_CASE("Transport rejects a worker pipeline that leaves no primed block",
          "[gpu_audio][convolver][staged][transport]") {
    PipelineDeclaringNode ok(2, 1), too_deep(2, 2);
    GpuAudioTransport transport;
    CHECK(transport.prepare(&ok, {/*ring_blocks=*/8})); // positive control
    transport.release();
    CHECK_FALSE(transport.prepare(&too_deep, {/*ring_blocks=*/8}));
}

TEST_CASE("Transport output stays continuous when the staged device is lost",
          "[gpu_audio][convolver][staged][transport]") {
    constexpr std::uint32_t BS = 64;
    constexpr std::size_t NBLK = 32, kLossBlock = 11;
    const auto ir = staged_test_ir();
    InjectableConvolver node(1, BS, 48000, ir);
    REQUIRE(node.prepare());
    auto* fake = node.adopt_fake(ir, 1.0f);

    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {/*ring_blocks=*/8}));
    const std::uint32_t L = transport.latency_samples() / BS;

    const auto input = staged_test_input(BS * NBLK);
    const auto ref = direct_convolution(input, ir);
    const auto out = run_transport(
        transport, input, BS, [](std::size_t) { return true; },
        [&](std::size_t b) {
            if (b == kLossBlock)
                fake->lose_device();
        });

    // After the primed latency the delivered stream is the reference delayed by
    // L blocks, straight through the loss.
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float expected = i < L * BS ? 0.0f : ref[i - L * BS];
        REQUIRE(std::abs(out[i] - expected) < 1.0e-3f * (1.0f + std::abs(expected)));
    }
    // Every post-priming block was a worker delivery: the loss never pushed the
    // callback onto its own miss substitute or a silent block.
    const auto delivered = transport.delivery_snapshot();
    CHECK(delivered.silence_blocks == 0u);
    CHECK(delivered.cpu_fallback_blocks == 0u);
    CHECK(delivered.worker_output_blocks == NBLK - L);
    CHECK(node.staged_latched_reason() == GpuAudioRecoveryReason::ProviderLost);
    CHECK(validate_gpu_audio_status(node.status_snapshot()).accepted());
}

// Worker cost per block, for the record (hidden; run with "[staged-bench]").
// The fake skips its reference convolution here so only the node's own work is
// timed. "GPU hit" is the default path when the readback is on time; "CPU
// block" is a block served by the worker's CPU convolution (a miss, or breaker
// CPU mode), which is also what the earlier CPU-every-block design paid on top
// of every hit. "Constant expiry" is the default path when every readback is
// late. With a real adapter the Auto and StagedBlocking rows time the device.
TEST_CASE("Staged GpuConvolver worker cost per block", "[.][staged-bench]") {
    auto mean_us = [](const std::vector<std::chrono::nanoseconds>& v, std::size_t skip) {
        double total = 0.0;
        for (std::size_t i = skip; i < v.size(); ++i)
            total += static_cast<double>(v[i].count());
        return total / static_cast<double>(v.size() - skip) / 1000.0;
    };
    for (const std::uint32_t bs : {64u, 256u}) {
        for (const std::size_t ir_len : {std::size_t{150}, std::size_t{96000}}) {
            std::vector<float> ir(ir_len);
            for (std::size_t i = 0; i < ir_len; ++i)
                ir[i] = static_cast<float>(std::exp(-6.0 * i / ir_len) * std::cos(0.01 * i));
            const std::size_t nblk = 200;
            const auto input = staged_test_input(bs * nblk);
            auto run = [&](GpuConvolver& node) {
                std::vector<std::chrono::nanoseconds> wall, cpu;
                (void)run_staged_stream(node, input, bs, [](std::size_t) {}, &wall, &cpu);
                auto worst = *std::max_element(cpu.begin() + 20, cpu.end());
                return std::tuple{mean_us(wall, 20), mean_us(cpu, 20), worst.count() / 1000.0};
            };
            InjectableConvolver hit(1, bs, 48000, ir);
            REQUIRE(hit.prepare());
            hit.adopt_fake(ir)->set_compute_results(false);
            const auto [hit_wall, hit_cpu, hit_worst] = run(hit);

            InjectableConvolver cpu_mode(1, bs, 48000, ir);
            REQUIRE(cpu_mode.prepare());
            auto* lost = cpu_mode.adopt_fake(ir);
            lost->set_compute_results(false);
            lost->lose_device();
            const auto [cpu_wall, cpu_cpu, cpu_worst] = run(cpu_mode);

            InjectableConvolver late(1, bs, 48000, ir);
            REQUIRE(late.prepare());
            auto* late_fake = late.adopt_fake(ir);
            late_fake->set_compute_results(false);
            late_fake->set_async_mode(FakeGpuCompute::AsyncMode::Late);
            const auto [late_wall, late_cpu, late_worst] = run(late);

            std::printf("bs=%u ir=%zu period=%.0fus | GPU hit cpu %.1fus | CPU block cpu %.1fus | "
                        "constant expiry cpu mean %.1fus worst %.1fus\n",
                        bs, ir_len, bs * 1e6 / 48000.0, hit_cpu, cpu_cpu, late_cpu, late_worst);
            (void)hit_wall;
            (void)hit_worst;
            (void)cpu_wall;
            (void)cpu_worst;
            (void)late_wall;

            for (const auto policy : {GpuConvolver::ProviderPolicy::Auto,
                                      GpuConvolver::ProviderPolicy::StagedBlocking}) {
                InjectableConvolver real(1, bs, 48000, ir);
                REQUIRE(real.set_provider_policy(policy));
                REQUIRE(real.prepare());
                if (!real.gpu_available())
                    continue;
                // Paced one block per period, as a live worker would be, so an
                // on-time readback can land before its block is due.
                std::vector<std::chrono::nanoseconds> wall, cpu;
                const auto period = std::chrono::microseconds(bs * 1'000'000 / 48000);
                (void)run_staged_stream(
                    real, input, bs, [&](std::size_t) { std::this_thread::sleep_for(period); },
                    &wall, &cpu);
                const auto w = mean_us(wall, 20);
                const auto c = mean_us(cpu, 20);
                const auto worst =
                    (*std::max_element(cpu.begin() + 20, cpu.end())).count() / 1000.0;
                std::printf("  real Metal %-14s: worker wall %.3f ms/block, cpu %.3f ms, worst cpu "
                            "%.3f ms; "
                            "GPU hit rate %.1f%% (%llu/%zu); CPU blocks %llu; breaker trips %llu; "
                            "reason %u\n",
                            policy == GpuConvolver::ProviderPolicy::Auto ? "Auto (A)"
                                                                         : "StagedBlocking",
                            w / 1000.0, c / 1000.0, worst / 1000.0,
                            100.0 * static_cast<double>(real.staged_gpu_blocks()) / nblk,
                            static_cast<unsigned long long>(real.staged_gpu_blocks()), nblk,
                            static_cast<unsigned long long>(real.staged_cpu_blocks()),
                            static_cast<unsigned long long>(real.staged_breaker_trips()),
                            static_cast<unsigned>(real.staged_latched_reason()));
            }
        }
    }
    SUCCEED();
}

TEST_CASE("GpuMultiConvolver refuses a lost device and keeps Silence",
          "[gpu_audio][convolver][multi][staged]") {
    constexpr std::uint32_t BS = 64;
    GpuMultiConvolver multi(BS, 48000, {{1.0f, 0.5f}, {0.25f}});
    auto fake = std::make_unique<FakeGpuCompute>();
    auto* raw = fake.get();
    REQUIRE(detail::prepare_gpu_multi_convolver_with(multi, std::move(fake)));
    REQUIRE(multi.gpu_available());

    std::vector<float> mono(BS, 0.1f), l(BS, -1.0f), r(BS, -1.0f);
    // Positive control: a live device produces the fake's marker output.
    REQUIRE(multi.convolve_stereo(mono.data(), l.data(), r.data(), BS));
    CHECK(raw->multi_calls() == 1u);
    CHECK(l[0] == FakeGpuCompute::kMultiMarker);
    CHECK_FALSE(multi.device_lost());

    raw->lose_device();
    CHECK(multi.device_lost());
    CHECK_FALSE(multi.convolve_stereo(mono.data(), l.data(), r.data(), BS));
    CHECK(raw->multi_calls() == 1u); // refused before reaching the device

    std::vector<float> in_l(BS, 0.1f), in_r(BS, 0.1f), out_l(BS, -1.0f), out_r(BS, -1.0f);
    const float* in_ptrs[2] = {in_l.data(), in_r.data()};
    float* out_ptrs[2] = {out_l.data(), out_r.data()};
    BufferView<const float> iv(in_ptrs, 2, BS);
    BufferView<float> ov(out_ptrs, 2, BS);
    multi.process_block(iv, ov, BS);
    for (std::uint32_t i = 0; i < BS; ++i) {
        CHECK(out_l[i] == 0.0f);
        CHECK(out_r[i] == 0.0f);
    }
}
#endif // PULP_TEST_HAS_GPU_COMPUTE
