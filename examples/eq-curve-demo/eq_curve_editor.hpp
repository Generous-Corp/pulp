#pragma once

// Editor for EqCurveDemo: mount an EqCurveView, seed it from the parameters,
// and write parameters back as the user drags the curve. The view and the
// audio path share one description of the filter (the four bands here map
// one-to-one onto the four biquad sections in eq_curve_demo.hpp), so what you
// see is what you hear.

#include "eq_curve_demo.hpp"

#include <pulp/view/eq_curve_view.hpp>

#include <memory>

namespace pulp::examples {

// The editor's curve types are named independently of the DSP enum; map the
// slots this demo uses.
inline view::EqCurveView::FilterType editor_type(signal::Biquad::Type t) {
    using S = signal::Biquad::Type;
    using E = view::EqCurveView::FilterType;
    switch (t) {
        case S::low_shelf: return E::low_shelf;
        case S::high_shelf: return E::high_shelf;
        case S::lowpass: return E::low_pass;
        case S::highpass: return E::high_pass;
        case S::bandpass: return E::band_pass;
        case S::notch: return E::notch;
        default: return E::peak;
    }
}

inline std::unique_ptr<view::View> build_eq_curve_editor(state::StateStore& store,
                                                         float sample_rate) {
    auto eq = std::make_unique<view::EqCurveView>();
    eq->set_sample_rate(sample_rate);
    eq->set_frequency_range(20.0f, 20000.0f);
    eq->set_gain_range(-18.0f, 18.0f);

    // Seed the handles from the current parameter values.
    std::vector<view::EqCurveView::Band> bands;
    bands.reserve(kEqBandCount);
    for (int b = 0; b < kEqBandCount; ++b) {
        view::EqCurveView::Band band;
        band.frequency = store.get_value(eq_freq_param(b));
        band.gain_db = store.get_value(eq_gain_param(b));
        band.q = store.get_value(eq_q_param(b));
        band.type = editor_type(kEqBands[b].type);
        band.enabled = true;
        bands.push_back(band);
    }
    eq->set_bands(std::move(bands));

    // Drag → parameters. The processor recomputes its coefficients from these
    // on the next block, so the audio follows the curve. (Host-side automation
    // of these params is not yet reflected back into the handles — that needs a
    // poll pump, a known SDK follow-up for native editor bindings.)
    eq->on_band_changed = [&store](std::size_t index, view::EqCurveView::Band band) {
        const int b = static_cast<int>(index);
        store.set_value(eq_freq_param(b), band.frequency);
        store.set_value(eq_gain_param(b), band.gain_db);
        store.set_value(eq_q_param(b), band.q);
    };

    return eq;
}

} // namespace pulp::examples
