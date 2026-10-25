#pragma once

#include <pulp/signal/denormal.hpp>

#include <cmath>
#include <cstddef>

namespace pulp::signal {

// State Variable Filter — Topology Preserving Transform (TPT) design.
// RT contract: setters recompute scalar coefficients; process/reset allocate no
// memory. Retune at block boundaries if coefficient continuity matters.
// Provides simultaneous lowpass, highpass, bandpass, and notch outputs.
// Numerically stable at all frequencies, no cramping at Nyquist.
template <typename SampleType = float>
class SvfT {
public:
    enum class Mode { lowpass, highpass, bandpass, notch };

    void set_sample_rate(SampleType sr) { sample_rate_ = sr; update(); }
    void set_frequency(SampleType hz) { freq_ = hz; update(); }
    void set_resonance(SampleType q) { q_ = q; update(); }
    void set_mode(Mode m) { mode_ = m; }

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
        // Snap the two integrator states to keep denormals out of the TPT
        // feedback path where no FTZ guard applies. No-op above 1e-15.
        ic1_ = snap_to_zero(SampleType{2.0f} * v1 - ic1_);
        ic2_ = snap_to_zero(SampleType{2.0f} * v2 - ic2_);

        switch (mode_) {
            case Mode::lowpass:  return v2;
            case Mode::highpass: return input - k_ * v1 - v2;
            case Mode::bandpass: return v1;
            case Mode::notch:    return input - k_ * v1;
        }
        return v2;
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
        case Mode::lowpass:
            run_block<Mode::lowpass>(buffer, num_samples);
            return;
        case Mode::highpass:
            run_block<Mode::highpass>(buffer, num_samples);
            return;
        case Mode::bandpass:
            run_block<Mode::bandpass>(buffer, num_samples);
            return;
        case Mode::notch:
            run_block<Mode::notch>(buffer, num_samples);
            return;
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

    template <Mode M> void run_block(SampleType* buffer, int num_samples) {
        const SampleType a1 = a1_, a2 = a2_, a3 = a3_, k = k_;
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
                if constexpr (M == Mode::lowpass)
                    buffer[i] = v2;
                else if constexpr (M == Mode::highpass)
                    buffer[i] = input - k * v1 - v2;
                else if constexpr (M == Mode::bandpass)
                    buffer[i] = v1;
                else
                    buffer[i] = input - k * v1;
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
