#include <catch2/catch_test_macros.hpp>

#include <pulp/gpu_audio/gpu_hyper_freeze.hpp>

#include "support/gpu_audio_test_helpers.hpp"

#include <cmath>
#include <vector>

using namespace pulp::gpu_audio;
using namespace pulp::gpu_audio_test;

namespace {
void fill_sine(std::vector<float>& f, uint32_t k) {
    for (uint32_t i = 0; i < f.size(); ++i)
        f[i] = std::sin(2.0f * 3.14159265f * k * i / f.size());
}
}  // namespace

TEST_CASE("GpuHyperFreeze stacks and morphs frozen layers", "[gpu_audio][spectral][gpu]") {
    constexpr uint32_t FFT = 512, HOP = 128, K1 = 21, K2 = 53;
    GpuHyperFreeze hf;
    REQUIRE(hf.prepare(FFT, HOP, 3));
    if (!hf.gpu_available()) return;

    std::vector<float> a(FFT), b(FFT);
    fill_sine(a, K1);
    fill_sine(b, K2);
    REQUIRE(hf.capture(0, a.data()));
    REQUIRE(hf.capture(1, b.data()));
    REQUIRE(hf.layer_active(0));
    REQUIRE(hf.layer_active(1));
    REQUIRE_FALSE(hf.layer_active(2));

    std::vector<float> out(FFT);

    // Equal stack (null weights): BOTH frozen tones present.
    REQUIRE(hf.render(out.data()));
    REQUIRE(band_mag(out, K1) > 1.0f);
    REQUIRE(band_mag(out, K2) > 1.0f);

    // Morph to layer 0 only.
    const float w_a[3] = {1.0f, 0.0f, 0.0f};
    REQUIRE(hf.render(out.data(), w_a));
    REQUIRE(peak_bin(out) == K1);
    REQUIRE(band_mag(out, K2) < 0.2f * band_mag(out, K1));

    // Morph to layer 1 only.
    const float w_b[3] = {0.0f, 1.0f, 0.0f};
    REQUIRE(hf.render(out.data(), w_b));
    REQUIRE(peak_bin(out) == K2);
}

TEST_CASE("GpuHyperFreeze smear spreads spectral energy", "[gpu_audio][spectral][gpu]") {
    constexpr uint32_t FFT = 512, HOP = 128, K1 = 40;
    GpuHyperFreeze hf;
    REQUIRE(hf.prepare(FFT, HOP, 1));
    if (!hf.gpu_available()) return;

    std::vector<float> a(FFT);
    fill_sine(a, K1);
    REQUIRE(hf.capture(0, a.data()));

    std::vector<float> dry(FFT), wet(FFT);
    REQUIRE(hf.render(dry.data(), nullptr, /*smear=*/0.0f));
    REQUIRE(hf.render(wet.data(), nullptr, /*smear=*/0.9f));

    // Smear blurs the sharp peak — energy bleeds into neighbouring bins.
    REQUIRE(mag_at(wet, K1 + 6) > 3.0f * mag_at(dry, K1 + 6));
    REQUIRE(peak_bin(wet) == K1);  // still centred on the tone
}

TEST_CASE("GpuHyperFreeze render before capture fails", "[gpu_audio][spectral][gpu]") {
    GpuHyperFreeze hf;
    REQUIRE(hf.prepare(256, 128, 2));
    if (!hf.gpu_available()) return;
    std::vector<float> out(256, 0.0f);
    REQUIRE_FALSE(hf.render(out.data()));
}
