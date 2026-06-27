#include <catch2/catch_test_macros.hpp>

#include <pulp/gpu_audio/gpu_spectral_freeze.hpp>
#include <pulp/signal/fft.hpp>

#include <cmath>
#include <complex>
#include <vector>

using namespace pulp::gpu_audio;

TEST_CASE("GpuSpectralFreeze sustains the captured spectrum", "[gpu_audio][spectral][gpu]") {
    constexpr uint32_t FFT = 512;
    constexpr uint32_t K0 = 20;  // sine at an integer bin → clean capture

    GpuSpectralFreeze fz;
    REQUIRE(fz.prepare(FFT));
    if (!fz.gpu_available()) return;

    std::vector<float> frame(FFT);
    for (uint32_t i = 0; i < FFT; ++i)
        frame[i] = std::sin(2.0f * 3.14159265f * K0 * i / FFT);

    REQUIRE(fz.capture(frame.data()));
    REQUIRE(fz.is_captured());

    std::vector<float> out1(FFT), out2(FFT);
    REQUIRE(fz.render(out1.data()));
    REQUIRE(fz.render(out2.data()));

    // Deterministic sustain: the same frozen spectrum renders identically.
    for (uint32_t i = 0; i < FFT; ++i) REQUIRE(std::abs(out1[i] - out2[i]) < 1e-6f);

    // The rendered frame carries real energy and its dominant frequency is K0.
    double energy = 0.0;
    for (uint32_t i = 0; i < FFT; ++i) energy += std::abs(out1[i]);
    REQUIRE(energy > 1.0);

    std::vector<std::complex<float>> spec(FFT);
    for (uint32_t i = 0; i < FFT; ++i) spec[i] = std::complex<float>(out1[i], 0.0f);
    pulp::signal::Fft fft(static_cast<int>(FFT));
    fft.forward(spec.data());

    uint32_t peak = 0;
    float peak_mag = 0.0f;
    for (uint32_t k = 1; k < FFT / 2; ++k) {
        const float m = std::abs(spec[k]);
        if (m > peak_mag) { peak_mag = m; peak = k; }
    }
    REQUIRE(peak == K0);
}

TEST_CASE("GpuSpectralFreeze render before capture fails", "[gpu_audio][spectral][gpu]") {
    GpuSpectralFreeze fz;
    REQUIRE(fz.prepare(256));
    if (!fz.gpu_available()) return;
    std::vector<float> out(256, 0.0f);
    REQUIRE_FALSE(fz.render(out.data()));
}
