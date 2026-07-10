#include <catch2/catch_test_macros.hpp>

#include <pulp/gpu_audio/spectral_stack.hpp>
#include <pulp/signal/windowing.hpp>

#include "support/gpu_audio_test_helpers.hpp"

#include <cmath>
#include <vector>

// Coverage for GpuSpectralStack — the batched, single-submit engine that
// superseded (and replaced) the retired per-layer GpuHyperFreeze. These cases
// mirror the multi-layer stacking / weighted-morph / spectral-smear behaviour
// the hyper-freeze test used to guard, now asserted against the surviving
// engine directly.

using namespace pulp::gpu_audio;
using namespace pulp::gpu_audio_test;

namespace {
void fill_sine(std::vector<float>& f, uint32_t k) {
    for (uint32_t i = 0; i < f.size(); ++i)
        f[i] = std::sin(2.0f * 3.14159265f * k * i / f.size());
}
// A Hann-windowed sine — a realistic analysis frame with a rounded (unimodal)
// spectral peak, so a heavy symmetric smear keeps the mode centred instead of
// flattening a bare delta into an argmax-ambiguous plateau.
void fill_windowed_sine(std::vector<float>& f, uint32_t k) {
    const auto w = pulp::signal::WindowFunction::generate(
        static_cast<int>(f.size()), pulp::signal::WindowFunction::Type::hann);
    for (uint32_t i = 0; i < f.size(); ++i)
        f[i] = std::sin(2.0f * 3.14159265f * k * i / f.size()) * w[i];
}
}  // namespace

TEST_CASE("GpuSpectralStack stacks and morphs frozen layers", "[gpu_audio][spectral][gpu]") {
    constexpr uint32_t FFT = 512, HOP = 128, K1 = 21, K2 = 53;
    GpuSpectralStack st;
    REQUIRE(st.prepare(FFT, HOP, 3));
    if (!st.available()) return;

    std::vector<float> a(FFT), b(FFT);
    fill_sine(a, K1);
    fill_sine(b, K2);
    REQUIRE(st.capture(0, a.data()));
    REQUIRE(st.capture(1, b.data()));
    REQUIRE(st.layer_active(0));
    REQUIRE(st.layer_active(1));
    REQUIRE_FALSE(st.layer_active(2));

    std::vector<float> out(FFT);

    // Equal stack (null weights): BOTH frozen tones present.
    REQUIRE(st.render(out.data(), nullptr, 0.0f, 0.0f));
    REQUIRE(band_mag(out, K1) > 1.0f);
    REQUIRE(band_mag(out, K2) > 1.0f);

    // Morph to layer 0 only.
    const float w_a[3] = {1.0f, 0.0f, 0.0f};
    REQUIRE(st.render(out.data(), w_a, 0.0f, 0.0f));
    REQUIRE(peak_bin(out) == K1);
    REQUIRE(band_mag(out, K2) < 0.2f * band_mag(out, K1));

    // Morph to layer 1 only.
    const float w_b[3] = {0.0f, 1.0f, 0.0f};
    REQUIRE(st.render(out.data(), w_b, 0.0f, 0.0f));
    REQUIRE(peak_bin(out) == K2);
}

TEST_CASE("GpuSpectralStack smear spreads spectral energy", "[gpu_audio][spectral][gpu]") {
    constexpr uint32_t FFT = 512, HOP = 128, K1 = 40;
    GpuSpectralStack st;
    REQUIRE(st.prepare(FFT, HOP, 1));
    if (!st.available()) return;

    std::vector<float> a(FFT);
    fill_windowed_sine(a, K1);
    REQUIRE(st.capture(0, a.data()));

    std::vector<float> dry(FFT), wet(FFT);
    REQUIRE(st.render(dry.data(), nullptr, /*smear=*/0.0f, 0.0f));
    REQUIRE(st.render(wet.data(), nullptr, /*smear=*/0.9f, 0.0f));

    // Smear blurs the sharp peak — energy bleeds into neighbouring bins.
    REQUIRE(mag_at(wet, K1 + 6) > 3.0f * mag_at(dry, K1 + 6));
    REQUIRE(peak_bin(wet) == K1);  // still centred on the tone
}

TEST_CASE("GpuSpectralStack render before capture fails", "[gpu_audio][spectral][gpu]") {
    GpuSpectralStack st;
    REQUIRE(st.prepare(256, 128, 2));
    if (!st.available()) return;
    std::vector<float> out(256, 0.0f);
    REQUIRE_FALSE(st.render(out.data(), nullptr, 0.0f, 0.0f));
}
