#include <catch2/catch_test_macros.hpp>

#include <pulp/gpu_audio/spectral_stack.hpp>
#include <pulp/signal/windowing.hpp>

#include "support/gpu_audio_test_helpers.hpp"

#include <algorithm>
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

namespace {
// Drive one stack through `muted_hops` renders that park layer 1 at weight 0
// (layer 0 audible), then a final render that makes layer 1 the only audible
// layer. Returns that final frame — it is a pure function of how many times
// layer 1's phase advanced while it was muted.
template <typename Stack>
std::vector<float> muted_then_unmuted(Stack& st, uint32_t fft, int muted_hops) {
    const float mute_l1[2] = {1.0f, 0.0f};  // layer 1 parked (muted)
    const float only_l1[2] = {0.0f, 1.0f};  // layer 1 alone, audible
    std::vector<float> out(fft, 0.0f);
    for (int i = 0; i < muted_hops; ++i)
        st.render(out.data(), mute_l1, 0.0f, 0.0f);
    st.render(out.data(), only_l1, 0.0f, 0.0f);
    return out;
}
template <typename Stack>
std::vector<float> always_hot(Stack& st, uint32_t fft, int hops) {
    const float both[2] = {1.0f, 1.0f};     // both layers audible the whole time
    const float only_l1[2] = {0.0f, 1.0f};
    std::vector<float> out(fft, 0.0f);
    for (int i = 0; i < hops; ++i)
        st.render(out.data(), both, 0.0f, 0.0f);
    st.render(out.data(), only_l1, 0.0f, 0.0f);
    return out;
}
float max_abs_diff(const std::vector<float>& a, const std::vector<float>& b) {
    float m = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(a[i] - b[i]));
    return m;
}
}  // namespace

// SF-4 regression: a muted-but-active layer must keep advancing its phase, so it
// re-enters phase-coherent on un-mute (no click). The consolidation gated the
// phase advance on the WEIGHT (`weights_[L] == 0.0f`), which froze a parked
// layer's phase; the fix gates on `active`. This proves layer 1's phase advanced
// identically whether it spent those hops MUTED or AUDIBLE — bit-exact on the CPU
// reference (fails on the pre-fix `weights_[L] == 0.0f` gate, which would leave
// layer 1 many radians behind), and continuity-preserving on the GPU backend,
// which already advances every layer unconditionally (its advance shader has no
// weight binding). Together this pins CPU/GPU parity for the parked-layer case.
TEST_CASE("SpectralStack keeps a muted layer's phase advancing (no un-mute click)",
          "[gpu_audio][spectral][sf4][regression]") {
    constexpr uint32_t FFT = 512, HOP = 128, K1 = 21, K2 = 53;
    constexpr int kMutedHops = 7;

    std::vector<float> a(FFT), b(FFT);
    fill_sine(a, K1);
    fill_sine(b, K2);

    SECTION("CPU reference — bit-exact phase continuity") {
        CpuSpectralStack muted, hot;
        REQUIRE(muted.prepare(FFT, HOP, 2));
        REQUIRE(hot.prepare(FFT, HOP, 2));
        REQUIRE(muted.available());
        for (auto* s : {&muted, &hot}) {
            REQUIRE(s->capture(0, a.data()));
            REQUIRE(s->capture(1, b.data()));
        }
        const auto out_muted = muted_then_unmuted(muted, FFT, kMutedHops);
        const auto out_hot = always_hot(hot, FFT, kMutedHops);
        // Layer 1 advanced kMutedHops+1 times in BOTH runs → identical frame.
        for (uint32_t i = 0; i < FFT; ++i)
            REQUIRE(out_muted[i] == out_hot[i]);
        // Teeth: the un-muted layer is genuinely non-trivial (not silence).
        REQUIRE(peak_bin(out_muted) == K2);
    }

    SECTION("GPU backend — same continuity, CPU/GPU in lockstep") {
        GpuSpectralStack gmuted, ghot;
        REQUIRE(gmuted.prepare(FFT, HOP, 2));
        REQUIRE(ghot.prepare(FFT, HOP, 2));
        if (!gmuted.available()) return;  // no device in this environment
        for (auto* s : {&gmuted, &ghot}) {
            REQUIRE(s->capture(0, a.data()));
            REQUIRE(s->capture(1, b.data()));
        }
        const auto g_out_muted = muted_then_unmuted(gmuted, FFT, kMutedHops);
        const auto g_out_hot = always_hot(ghot, FFT, kMutedHops);
        // The GPU advances every layer unconditionally, so muting layer 1 must not
        // change where its phase lands — same frame as the always-hot run.
        REQUIRE(max_abs_diff(g_out_muted, g_out_hot) < 1e-4f);
        REQUIRE(peak_bin(g_out_muted) == K2);

        // CPU/GPU parity for the parked-layer case: both backends land layer 1 at
        // the same phase, so their un-mute frames agree (to cross-backend FFT
        // tolerance).
        CpuSpectralStack cmuted;
        REQUIRE(cmuted.prepare(FFT, HOP, 2));
        REQUIRE(cmuted.capture(0, a.data()));
        REQUIRE(cmuted.capture(1, b.data()));
        const auto c_out_muted = muted_then_unmuted(cmuted, FFT, kMutedHops);
        REQUIRE(peak_bin(c_out_muted) == peak_bin(g_out_muted));
    }
}
