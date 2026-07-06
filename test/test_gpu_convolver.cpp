#include <catch2/catch_test_macros.hpp>

#include <pulp/gpu_audio/gpu_convolver.hpp>

#include <cmath>
#include <vector>

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
