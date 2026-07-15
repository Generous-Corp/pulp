// Tests for EqCurveDemo. The point of this plugin is that the drawn curve and
// the processed audio agree, so the tests assert exactly that: the response the
// editor would draw (via the shared frequency_response helpers) matches the
// gain the process() path actually applies to a sine.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "eq_curve_demo.hpp"
#include "eq_curve_editor.hpp"

#include <pulp/signal/filter_design.hpp>
#include <pulp/signal/frequency_response.hpp>
#include <pulp/view/eq_curve_view.hpp>

#include <cmath>
#include <vector>

using namespace pulp;
using namespace pulp::examples;
using Catch::Matchers::WithinAbs;

namespace {
constexpr float sr = 48000.0f;

// The magnitude the editor's curve would show at a frequency, in dB, for a
// given per-band parameter set — built the same way the processor designs its
// sections, then evaluated with the shared cascade helper.
float curve_db_at(const std::vector<float>& freqs,
                  const std::vector<float>& gains,
                  const std::vector<float>& qs,
                  float hz) {
    std::vector<pulp::signal::BiquadCoefficients> sos;
    for (int b = 0; b < kEqBandCount; ++b) {
        pulp::signal::Biquad f;
        f.set_coefficients(kEqBands[b].type, freqs[b], qs[b], sr, gains[b]);
        sos.push_back(f.coefficients());
    }
    return pulp::signal::cascade_magnitude_db(sos, hz, sr);
}
} // namespace

TEST_CASE("EqCurveDemo defines three parameters per band", "[eq-curve-demo]") {
    EqCurveDemoProcessor proc;
    state::StateStore store;
    proc.define_parameters(store);
    for (int b = 0; b < kEqBandCount; ++b) {
        REQUIRE(store.info(eq_freq_param(b)) != nullptr);
        REQUIRE(store.info(eq_gain_param(b)) != nullptr);
        REQUIRE(store.info(eq_q_param(b)) != nullptr);
    }
}

TEST_CASE("flat EQ passes audio unchanged", "[eq-curve-demo]") {
    // All gains at 0 dB: shelves and peaks are transparent, so a mid sine comes
    // through at unity.
    std::vector<float> freqs{80, 250, 700, 2000, 5000, 12000}, gains(6, 0.0f), qs(6, 1.0f);
    REQUIRE_THAT(curve_db_at(freqs, gains, qs, 1000.0f), WithinAbs(0.0, 0.2));
}

TEST_CASE("each band shapes its own region", "[eq-curve-demo]") {
    // Low shelf up, a peak cut, the other peak boost, high shelf up.
    std::vector<float> freqs{80, 250, 700, 2000, 5000, 12000};
    std::vector<float> gains{6, -9, 0, 7, 0, 4};
    std::vector<float> qs{0.707f, 3.0f, 1.5f, 2.0f, 2.5f, 0.707f};

    REQUIRE_THAT(curve_db_at(freqs, gains, qs, 25.0f), WithinAbs(6.0, 0.8));   // low shelf plateau
    REQUIRE_THAT(curve_db_at(freqs, gains, qs, 250.0f), WithinAbs(-9.0, 0.6)); // peak cut
    REQUIRE_THAT(curve_db_at(freqs, gains, qs, 2000.0f), WithinAbs(7.0, 0.6)); // peak boost
    REQUIRE_THAT(curve_db_at(freqs, gains, qs, 19000.0f), WithinAbs(4.0, 0.9)); // high shelf plateau
}

TEST_CASE("dragging a handle writes the plugin's parameters", "[eq-curve-demo][editor]") {
    // The interactive promise: grab a band's handle, drag it down, and the
    // plugin's gain parameter for that band follows — which is what the audio
    // path reads. Drives the real editor build_eq_curve_editor produces.
    EqCurveDemoProcessor proc;
    state::StateStore store;
    proc.set_state_store(&store);
    proc.define_parameters(store);

    auto view = build_eq_curve_editor(store, sr);
    view->set_bounds({0, 0, 800, 400});
    auto* eq = dynamic_cast<view::EqCurveView*>(view.get());
    REQUIRE(eq != nullptr);

    // Peak 2 (band index 2) starts at 0 dB. Its handle sits at (freq_x, 0 dB).
    constexpr int band = 2;
    const auto fscale = eq->frequency_scale();
    const auto gscale = eq->gain_scale();
    const float hx = fscale.to_x(store.get_value(eq_freq_param(band)));
    const float hy = gscale.to_y(store.get_value(eq_gain_param(band)));

    // Press on the handle, drag up to about +9 dB, release.
    const float target_db = 9.0f;
    eq->on_mouse_down({hx, hy});
    eq->on_mouse_drag({hx, gscale.to_y(target_db)});
    eq->on_mouse_up({hx, gscale.to_y(target_db)});

    // The store now holds the dragged gain — the value the processor will read.
    REQUIRE_THAT(store.get_value(eq_gain_param(band)), WithinAbs(target_db, 0.5));
    // And the other bands were left alone.
    REQUIRE_THAT(store.get_value(eq_gain_param(0)), WithinAbs(0.0, 1e-4));
}

TEST_CASE("processed audio matches the drawn curve", "[eq-curve-demo]") {
    // The whole promise of the plugin: measure the actual output of process()
    // and confirm it equals the curve the editor draws, at several frequencies.
    EqCurveDemoProcessor proc;
    state::StateStore store;
    proc.set_state_store(&store);
    proc.define_parameters(store);

    std::vector<float> freqs{100, 300, 800, 2200, 5500, 11000};
    std::vector<float> gains{5, -8, 3, 7, -4, 6};
    std::vector<float> qs{0.707f, 4.0f, 1.5f, 2.5f, 2.0f, 0.707f};
    for (int b = 0; b < kEqBandCount; ++b) {
        store.set_value(eq_freq_param(b), freqs[b]);
        store.set_value(eq_gain_param(b), gains[b]);
        store.set_value(eq_q_param(b), qs[b]);
    }

    format::PrepareContext prep;
    prep.sample_rate = sr;
    prep.max_buffer_size = 512;
    proc.prepare(prep);

    for (float test_hz : {200.0f, 800.0f, 3000.0f, 12000.0f}) {
        // Render a mono sine through the stereo processor and measure the
        // steady-state peak of channel 0.
        const int total = 48000;
        const int block = 512;
        std::vector<float> in_l(block), in_r(block), out_l(block), out_r(block);

        // Reset state between frequencies.
        proc.prepare(prep);
        float phase = 0.0f;
        const float dphi = 2.0f * static_cast<float>(M_PI) * test_hz / sr;
        float peak = 0.0f;
        for (int off = 0; off < total; off += block) {
            for (int i = 0; i < block; ++i) {
                in_l[i] = in_r[i] = std::sin(phase);
                phase += dphi;
            }
            float* ins[2] = {in_l.data(), in_r.data()};
            float* outs[2] = {out_l.data(), out_r.data()};
            audio::BufferView<const float> input(ins, 2, block);
            audio::BufferView<float> output(outs, 2, block);
            midi::MidiBuffer m_in, m_out;
            format::ProcessContext ctx;
            ctx.sample_rate = sr;
            ctx.num_samples = block;
            proc.process(output, input, m_in, m_out, ctx);
            if (off > total / 2)
                for (int i = 0; i < block; ++i) peak = std::max(peak, std::abs(out_l[i]));
        }
        const float measured_db = 20.0f * std::log10(peak);
        const float predicted_db = curve_db_at(freqs, gains, qs, test_hz);
        REQUIRE_THAT(measured_db, WithinAbs(predicted_db, 0.3));
    }
}
