#include <catch2/catch_test_macros.hpp>

#include <pulp/gpu_audio/gpu_spectral_freeze.hpp>
#include <pulp/gpu_audio/spectral_jitter.hpp>
#include <pulp/gpu_audio/spectral_stack.hpp>
#include <pulp/signal/windowing.hpp>

#include "support/gpu_audio_test_helpers.hpp"

#include <cmath>
#include <vector>

using namespace pulp::gpu_audio;
using namespace pulp::gpu_audio_test;

namespace {
// Scale-invariant normalized correlation of two frames in [-1, 1].
double frame_correlation(const std::vector<float>& a, const std::vector<float>& b) {
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * b[i];
        na += static_cast<double>(a[i]) * a[i];
        nb += static_cast<double>(b[i]) * b[i];
    }
    return (na > 0.0 && nb > 0.0) ? dot / std::sqrt(na * nb) : 0.0;
}
}  // namespace

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

// SF-4: the freeze jitter knob now follows the SHARED spectral-jitter contract
// (full-turn-at-1, 2*pi-scaled), so jitter=1 produces a DEEP per-hop phase
// wander. On a bin whose nominal per-hop advance is a whole turn (K0 a multiple
// of the 1/hop-ratio = 4 here), a jitter-free freeze is a near-static loop
// (successive frames ~identical); full jitter randomizes the phase enough to
// nearly decorrelate them — a depth only the 2*pi-scaled model reaches (the old
// ~2*pi-shallower LCG model at max would only mildly perturb it).
TEST_CASE("GpuSpectralFreeze jitter is deep (2pi-scaled) per the shared contract",
          "[gpu_audio][spectral][gpu]") {
    constexpr uint32_t FFT = 512, HOP = 128, K0 = 20;  // static loop at jitter 0
    std::vector<float> frame(FFT);
    for (uint32_t i = 0; i < FFT; ++i)
        frame[i] = std::sin(2.0f * 3.14159265f * K0 * i / FFT);

    auto successive_correlation = [&](float jitter) {
        GpuSpectralFreeze fz;
        REQUIRE(fz.prepare(FFT, HOP));
        if (!fz.gpu_available()) return 2.0;  // sentinel: no GPU adapter
        REQUIRE(fz.capture(frame.data()));
        std::vector<float> a(FFT), b(FFT);
        REQUIRE(fz.render(a.data(), jitter));
        REQUIRE(fz.render(b.data(), jitter));
        return frame_correlation(a, b);
    };

    const double c0 = successive_correlation(0.0f);
    if (c0 > 1.5) return;  // no GPU adapter
    const double c1 = successive_correlation(1.0f);

    // Full 2*pi jitter randomizes each bin's phase by up to ±pi per hop, so
    // successive frames become nearly uncorrelated — a collapse only the deep
    // (2*pi-scaled) model reaches; the old ~2*pi-shallower LCG model at max would
    // leave successive frames still strongly correlated.
    REQUIRE(c1 < 0.3);
    // ...and clearly more decorrelated than the jitter-free baseline.
    REQUIRE(c1 < c0 - 0.3);
}

// SF-4: the ported freeze must produce the SAME sound as a single-layer
// SpectralStack for the same captured spectrum + jitter, proving the two
// engines now share one jitter/phase model (CpuSpectralStack is the always-
// available CPU reference the GPU stack is bit-matched against).
TEST_CASE("GpuSpectralFreeze matches CpuSpectralStack under the unified jitter",
          "[gpu_audio][spectral][gpu]") {
    constexpr uint32_t FFT = 512, HOP = 128;
    constexpr float JIT = 0.6f;

    // A small chord so the comparison rides on real, well-conditioned energy.
    std::vector<float> raw(FFT, 0.0f);
    for (uint32_t i = 0; i < FFT; ++i)
        raw[i] = std::sin(2.0f * 3.14159265f * 21 * i / FFT)
               + 0.6f * std::sin(2.0f * 3.14159265f * 53 * i / FFT);

    GpuSpectralFreeze fz;
    REQUIRE(fz.prepare(FFT, HOP));
    if (!fz.gpu_available()) return;

    // The stack captures unwindowed frames, so hand it the SAME Hann-windowed
    // frame the freeze's GpuStft analysis applies internally.
    const auto win = pulp::signal::WindowFunction::generate(
        static_cast<int>(FFT), pulp::signal::WindowFunction::Type::hann);
    std::vector<float> windowed(FFT);
    for (uint32_t i = 0; i < FFT; ++i) windowed[i] = raw[i] * win[static_cast<std::size_t>(i)];

    CpuSpectralStack stack;
    REQUIRE(stack.prepare(FFT, HOP, 1));
    REQUIRE(stack.available());

    REQUIRE(fz.capture(raw.data()));
    const float w1 = 1.0f;
    REQUIRE(stack.capture(0, windowed.data()));

    // Render several hops in lockstep — both advance the shared seed each hop.
    std::vector<float> fo(FFT), so(FFT);
    for (int hop = 0; hop < 5; ++hop) {
        REQUIRE(fz.render(fo.data(), JIT));
        REQUIRE(stack.render(so.data(), &w1, 0.0f, JIT));
    }

    // Same dominant bin, and the two waveforms track (normalized correlation is
    // scale-invariant, tolerating the GPU-vs-CPU inverse-FFT gain/rounding).
    REQUIRE(peak_bin(fo) == peak_bin(so));
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (uint32_t i = 0; i < FFT; ++i) {
        dot += static_cast<double>(fo[i]) * so[i];
        na += static_cast<double>(fo[i]) * fo[i];
        nb += static_cast<double>(so[i]) * so[i];
    }
    const double corr = (na > 0.0 && nb > 0.0) ? dot / std::sqrt(na * nb) : 0.0;
    REQUIRE(corr > 0.8);
}
