#include <catch2/catch_test_macros.hpp>

#include <pulp/gpu_audio/gpu_stft.hpp>
#include <pulp/signal/fft.hpp>
#include <pulp/signal/windowing.hpp>

#include <cmath>
#include <complex>
#include <vector>

using namespace pulp::gpu_audio;
using pulp::signal::WindowFunction;

TEST_CASE("GpuStft analyze magnitude matches CPU windowed FFT", "[gpu_audio][stft][gpu]") {
    constexpr uint32_t FFT = 512;
    GpuStft st;
    REQUIRE(st.prepare(FFT, WindowFunction::Type::hann));
    if (!st.gpu_available()) return;

    std::vector<float> frame(FFT);
    for (uint32_t i = 0; i < FFT; ++i) frame[i] = std::sin(0.05f * i) + 0.3f * std::cos(0.017f * i);

    std::vector<float> spec(FFT * 2);
    REQUIRE(st.analyze(frame.data(), spec.data()));

    // CPU reference: window the frame, forward FFT, magnitude.
    const auto& w = st.window();
    std::vector<std::complex<float>> cpu(FFT);
    for (uint32_t i = 0; i < FFT; ++i) cpu[i] = std::complex<float>(frame[i] * w[i], 0.0f);
    pulp::signal::Fft fft(static_cast<int>(FFT));
    fft.forward(cpu.data());

    for (uint32_t i = 0; i < FFT; ++i) {
        const float gpu_mag = std::sqrt(spec[i * 2] * spec[i * 2] + spec[i * 2 + 1] * spec[i * 2 + 1]);
        const float cpu_mag = std::abs(cpu[i]);
        REQUIRE(std::abs(gpu_mag - cpu_mag) < 1e-2f * (1.0f + cpu_mag));
    }
}

TEST_CASE("GpuStft analyze/synthesize overlap-add reconstructs (COLA)", "[gpu_audio][stft][gpu]") {
    constexpr uint32_t FFT = 256, HOP = 128;  // Hann at 50% overlap → COLA
    GpuStft st;
    REQUIRE(st.prepare(FFT, WindowFunction::Type::hann));
    if (!st.gpu_available()) return;

    constexpr int NF = 12;
    const int L = (NF - 1) * static_cast<int>(HOP) + static_cast<int>(FFT);
    std::vector<float> input(L);
    for (int i = 0; i < L; ++i) input[i] = std::sin(0.07f * i) + 0.3f * std::sin(0.013f * i);

    std::vector<float> output(L, 0.0f), frame(FFT), spec(FFT * 2), recon(FFT);
    for (int f = 0; f < NF; ++f) {
        for (uint32_t i = 0; i < FFT; ++i) frame[i] = input[f * HOP + i];
        REQUIRE(st.analyze(frame.data(), spec.data()));
        REQUIRE(st.synthesize(spec.data(), recon.data()));
        for (uint32_t i = 0; i < FFT; ++i) output[f * HOP + i] += recon[i];
    }

    // Interior is covered by full overlap → output == C * input (perfect
    // reconstruction up to the window's COLA constant). Measure C once, then
    // require every interior sample matches — normalization-agnostic.
    const int lo = static_cast<int>(FFT), hi = L - static_cast<int>(FFT);
    const int mid = (lo + hi) / 2;
    REQUIRE(std::abs(input[mid]) > 0.05f);
    const float c = output[mid] / input[mid];
    REQUIRE(c > 0.1f);  // sane, nonzero reconstruction
    for (int n = lo; n < hi; ++n) {
        REQUIRE(std::abs(output[n] - c * input[n]) < 1e-2f * (1.0f + std::abs(c * input[n])));
    }
}

TEST_CASE("GpuStft rejects non-power-of-two", "[gpu_audio][stft][gpu]") {
    GpuStft st;
    REQUIRE_FALSE(st.prepare(300));
}
