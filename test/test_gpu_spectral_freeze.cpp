#include <catch2/catch_test_macros.hpp>

#include <pulp/gpu_audio/gpu_spectral_freeze.hpp>

#include "support/gpu_audio_test_helpers.hpp"

#include <cmath>
#include <vector>

using namespace pulp::gpu_audio;
using namespace pulp::gpu_audio_test;

TEST_CASE("GpuSpectralFreeze sustains pitch with an evolving phase", "[gpu_audio][spectral][gpu]") {
    constexpr uint32_t FFT = 512, HOP = 128, K0 = 21;  // K0 not a multiple of 4 → phase evolves

    GpuSpectralFreeze fz;
    REQUIRE(fz.prepare(FFT, HOP));
    if (!fz.gpu_available()) return;

    std::vector<float> frame(FFT);
    for (uint32_t i = 0; i < FFT; ++i)
        frame[i] = std::sin(2.0f * 3.14159265f * K0 * i / FFT);

    REQUIRE(fz.capture(frame.data()));
    REQUIRE(fz.is_captured());

    std::vector<float> out1(FFT), out2(FFT);
    REQUIRE(fz.render(out1.data()));
    REQUIRE(fz.render(out2.data()));

    // The freeze sustains the captured frequency across hops...
    REQUIRE(peak_bin(out1) == K0);
    REQUIRE(peak_bin(out2) == K0);

    // ...while the phase-vocoder advance makes successive frames evolve (a
    // seamless loop, not a static repeat) and carry real energy.
    double diff = 0.0, energy = 0.0;
    for (uint32_t i = 0; i < FFT; ++i) {
        diff += std::abs(out1[i] - out2[i]);
        energy += std::abs(out1[i]);
    }
    REQUIRE(energy > 1.0);
    REQUIRE(diff > 0.01 * energy);
}

TEST_CASE("GpuSpectralFreeze jitter stays real and on-pitch", "[gpu_audio][spectral][gpu]") {
    // Phase jitter is applied conjugate-symmetrically; a broken pairing would
    // make the spectrum non-Hermitian and the real-part output collapse/corrupt.
    constexpr uint32_t FFT = 512, HOP = 128, K0 = 21;
    GpuSpectralFreeze fz;
    REQUIRE(fz.prepare(FFT, HOP));
    if (!fz.gpu_available()) return;

    std::vector<float> frame(FFT);
    for (uint32_t i = 0; i < FFT; ++i)
        frame[i] = std::sin(2.0f * 3.14159265f * K0 * i / FFT);
    REQUIRE(fz.capture(frame.data()));

    std::vector<float> out(FFT);
    for (int r = 0; r < 4; ++r) REQUIRE(fz.render(out.data(), /*phase_jitter=*/0.5f));

    // Energy preserved (no Hermitian-break collapse) and pitch still dominant.
    double energy = 0.0;
    for (uint32_t i = 0; i < FFT; ++i) energy += std::abs(out[i]);
    REQUIRE(energy > 1.0);
    REQUIRE(peak_bin(out) == K0);
}

TEST_CASE("GpuSpectralFreeze render before capture fails", "[gpu_audio][spectral][gpu]") {
    GpuSpectralFreeze fz;
    REQUIRE(fz.prepare(256, 128));
    if (!fz.gpu_available()) return;
    std::vector<float> out(256, 0.0f);
    REQUIRE_FALSE(fz.render(out.data()));
}
