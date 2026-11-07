#pragma once

#include <pulp/signal/denormal.hpp>

#include <cmath>
#include <cstddef>

namespace pulp::signal {

// State Variable Filter — Topology Preserving Transform (TPT) design.
// RT contract: setters recompute scalar coefficients; process/reset allocate no
// memory. Retune at block boundaries if coefficient continuity matters.
// One pass yields every classic state-variable response: lowpass, highpass,
// constant-skirt bandpass, unity-gain bandpass, notch, allpass, peak and
// band-shelf (see Outputs and process_all()). Numerically stable at all
// frequencies, no cramping at Nyquist.
//
// Reference: Vadim Zavalishin, "The Art of VA Filter Design", chapter 4
// (state-variable filter and its multimode outputs). With resonance Q and
// damping k = 1/Q, and v1 / v2 the bandpass and lowpass integrator outputs:
//
//   lowpass      LP  = v2
//   bandpass     BP  = v1                 peak gain Q at the cutoff
//   unity BP     UBP = k * v1             gain 1 at the cutoff
//   highpass     HP  = in - k v1 - v2
//   notch        N   = LP + HP = in - k v1
//   allpass      AP  = in - 2 k v1        unit magnitude at every frequency
//   peak         PK  = LP - HP
//   band-shelf   BSH = in + (G - 1) k v1  gain G at the cutoff, unity far away
//
// Notch plus unity bandpass reconstructs the input exactly.
template <typename SampleType = float>
class SvfT {
public:
    // The first four values are the original API; the rest are appended so
    // existing numeric values never change.
    enum class Mode {
        lowpass,
        highpass,
        bandpass,
        notch,
        unity_bandpass,
        allpass,
        peak,
        band_shelf
    };

    /// Every output of one sample step.
    struct Outputs {
        SampleType lowpass, highpass, bandpass, unity_bandpass, notch, allpass, peak, band_shelf;
    };

    void set_sample_rate(SampleType sr) { sample_rate_ = sr; update(); }
    void set_frequency(SampleType hz) { freq_ = hz; update(); }
    void set_resonance(SampleType q) { q_ = q; update(); }
    void set_mode(Mode m) { mode_ = m; }

    /// Linear amplitude gain at the cutoff for the band_shelf output (1 is
    /// transparent). Takes effect on the next sample; bandwidth follows the
    /// resonance setting.
    void set_shelf_gain(SampleType linear_gain) {
        shelf_gain_ = std::isfinite(static_cast<double>(linear_gain)) && linear_gain > SampleType{0}
                          ? linear_gain
                          : SampleType{1};
    }
    void set_shelf_gain_db(SampleType db) {
        set_shelf_gain(static_cast<SampleType>(std::pow(10.0, static_cast<double>(db) / 20.0)));
    }

    /// Install a coherent coefficient set in one prewarp. Use this when sample
    /// rate and cutoff change together so neither setter temporarily evaluates
    /// the new value against the old value's Nyquist limit.
    void configure(Mode mode, SampleType sample_rate, SampleType frequency,
                   SampleType resonance) {
        mode_ = mode;
        sample_rate_ = sample_rate;
        freq_ = frequency;
        q_ = resonance;
        update();
    }

    /// The TPT state-variable coefficients for a sample rate, cutoff and
    /// resonance (Q): g = tan(pi fc / fs), k = 1 / Q, a1 = 1 / (1 + g (g + k)),
    /// a2 = g a1, a3 = g a2.
    struct Coefficients {
        SampleType g, k, a1, a2, a3;
    };

    static Coefficients design(SampleType sample_rate, SampleType frequency, SampleType resonance) {
        Coefficients c{};
        c.g = std::tan(SampleType{3.14159265f} * frequency / sample_rate);
        c.k = SampleType{1.0f} / resonance;
        c.a1 = SampleType{1.0f} / (SampleType{1.0f} + c.g * (c.g + c.k));
        c.a2 = c.g * c.a1;
        c.a3 = c.g * c.a2;
        return c;
    }

    SampleType process(SampleType input) {
        SampleType v3 = input - ic2_;
        SampleType v1 = a1_ * ic1_ + a2_ * v3;
        SampleType v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        // Snap the two integrator states where subnormals are slow (see
        // snap_recursion_state()). No-op above 1e-15.
        ic1_ = snap_recursion_state(SampleType{2.0f} * v1 - ic1_);
        ic2_ = snap_recursion_state(SampleType{2.0f} * v2 - ic2_);

        switch (mode_) {
            case Mode::lowpass:        return pick<Mode::lowpass>(input, v1, v2);
            case Mode::highpass:       return pick<Mode::highpass>(input, v1, v2);
            case Mode::bandpass:       return pick<Mode::bandpass>(input, v1, v2);
            case Mode::notch:          return pick<Mode::notch>(input, v1, v2);
            case Mode::unity_bandpass: return pick<Mode::unity_bandpass>(input, v1, v2);
            case Mode::allpass:        return pick<Mode::allpass>(input, v1, v2);
            case Mode::peak:           return pick<Mode::peak>(input, v1, v2);
            case Mode::band_shelf:     return pick<Mode::band_shelf>(input, v1, v2);
        }
        return v2;
    }

    /// One sample step returning every output at once. Same state update as
    /// process(); use it when several responses of one filter are needed.
    Outputs process_all(SampleType input) {
        SampleType v3 = input - ic2_;
        SampleType v1 = a1_ * ic1_ + a2_ * v3;
        SampleType v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = snap_recursion_state(SampleType{2.0f} * v1 - ic1_);
        ic2_ = snap_recursion_state(SampleType{2.0f} * v2 - ic2_);
        return outputs(input, v1, v2);
    }

    /// Process a buffer in place.
    ///
    /// Same arithmetic as the per-sample path, with the mode resolved once per
    /// call and the coefficients and integrator states held in registers. The
    /// denormal snap runs once per kSnapChunkSamples chunk (snap_chunks());
    /// it only alters states below 1e-15, so the output differs from the
    /// per-sample path only once a tail is already below that level.
    void process(SampleType* buffer, int num_samples) {
        switch (mode_) {
        case Mode::lowpass:        run_block<Mode::lowpass>(buffer, num_samples); return;
        case Mode::highpass:       run_block<Mode::highpass>(buffer, num_samples); return;
        case Mode::bandpass:       run_block<Mode::bandpass>(buffer, num_samples); return;
        case Mode::notch:          run_block<Mode::notch>(buffer, num_samples); return;
        case Mode::unity_bandpass: run_block<Mode::unity_bandpass>(buffer, num_samples); return;
        case Mode::allpass:        run_block<Mode::allpass>(buffer, num_samples); return;
        case Mode::peak:           run_block<Mode::peak>(buffer, num_samples); return;
        case Mode::band_shelf:     run_block<Mode::band_shelf>(buffer, num_samples); return;
        }
        run_block<Mode::lowpass>(buffer, num_samples);
    }

    void reset() { ic1_ = 0; ic2_ = 0; }

private:
    SampleType sample_rate_ = SampleType{44100.0f};
    SampleType freq_ = SampleType{1000.0f};
    SampleType q_ = SampleType{0.707f};
    Mode mode_ = Mode::lowpass;

    SampleType g_ = 0, k_ = 0;
    SampleType a1_ = 0, a2_ = 0, a3_ = 0;
    SampleType ic1_ = 0, ic2_ = 0;
    SampleType shelf_gain_ = SampleType{1};

    template <Mode M>
    static SampleType pick(SampleType input, SampleType v1, SampleType v2, SampleType k,
                           SampleType shelf) {
        if constexpr (M == Mode::lowpass)
            return v2;
        else if constexpr (M == Mode::highpass)
            return input - k * v1 - v2;
        else if constexpr (M == Mode::bandpass)
            return v1;
        else if constexpr (M == Mode::notch)
            return input - k * v1;
        else if constexpr (M == Mode::unity_bandpass)
            return k * v1;
        else if constexpr (M == Mode::allpass)
            return input - SampleType{2} * k * v1;
        else if constexpr (M == Mode::peak)
            return SampleType{2} * v2 + k * v1 - input;
        else
            return input + (shelf - SampleType{1}) * k * v1;
    }
    template <Mode M> SampleType pick(SampleType input, SampleType v1, SampleType v2) const {
        return pick<M>(input, v1, v2, k_, shelf_gain_);
    }

    Outputs outputs(SampleType input, SampleType v1, SampleType v2) const {
        return {pick<Mode::lowpass>(input, v1, v2),        pick<Mode::highpass>(input, v1, v2),
                pick<Mode::bandpass>(input, v1, v2),       pick<Mode::unity_bandpass>(input, v1, v2),
                pick<Mode::notch>(input, v1, v2),          pick<Mode::allpass>(input, v1, v2),
                pick<Mode::peak>(input, v1, v2),           pick<Mode::band_shelf>(input, v1, v2)};
    }

    template <Mode M> void run_block(SampleType* buffer, int num_samples) {
        const SampleType a1 = a1_, a2 = a2_, a3 = a3_, k = k_, shelf = shelf_gain_;
        SampleType ic1 = ic1_, ic2 = ic2_;
        if (num_samples <= 0)
            return;
        for (const auto [begin, end] : snap_chunks(static_cast<std::size_t>(num_samples))) {
            for (std::size_t i = begin; i < end; ++i) {
                const SampleType input = buffer[i];
                const SampleType v3 = input - ic2;
                const SampleType v1 = a1 * ic1 + a2 * v3;
                const SampleType v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = SampleType{2.0f} * v1 - ic1;
                ic2 = SampleType{2.0f} * v2 - ic2;
                buffer[i] = pick<M>(input, v1, v2, k, shelf);
            }
            ic1 = snap_to_zero(ic1);
            ic2 = snap_to_zero(ic2);
        }
        ic1_ = ic1;
        ic2_ = ic2;
    }

    void update() {
        const Coefficients c = design(sample_rate_, freq_, q_);
        g_ = c.g;
        k_ = c.k;
        a1_ = c.a1;
        a2_ = c.a2;
        a3_ = c.a3;
    }
};

using Svf = SvfT<float>;
using Svf64 = SvfT<double>;

} // namespace pulp::signal
