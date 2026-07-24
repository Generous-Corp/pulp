#pragma once

#include <pulp/signal/denormal.hpp>

#include <array>
#include <cmath>
#include <cstddef>

namespace pulp::signal {

/// A pair of allpass chains whose outputs are 90 degrees apart across the audio
/// band — an analytic-signal splitter.
///
/// Neither output is the input: both are allpass, so both keep the input's
/// magnitude spectrum and only its phase is moved. What matters is the
/// *difference* between them, which holds near a quarter cycle from roughly
/// 20 Hz to nearly Nyquist. That quadrature pair is what makes single-sideband
/// processing possible; a single filter cannot produce it.
///
/// Each branch is a cascade of second-order allpass sections of the form
///   y[n] = c*(x[n] + y[n-2]) - x[n-2]
/// and the second branch is additionally delayed by one sample, which is what
/// puts the two a quarter cycle apart rather than a half. The coefficients are
/// a published, widely-reproduced design for this structure; they are constants
/// of the filter design rather than anything measured or fitted.
///
/// RT contract: every member allocates nothing and takes no locks.
template <typename SampleType = double>
class HilbertPairT {
public:
    struct Outputs {
        SampleType in_phase = 0;   ///< the reference branch
        SampleType quadrature = 0; ///< a quarter cycle behind it
    };

    void reset() {
        for (auto& s : a_) s = Section{};
        for (auto& s : b_) s = Section{};
        delayed_ = 0;
    }

    Outputs process(SampleType x) {
        SampleType branch_a = x;
        for (auto& s : a_) branch_a = s.process(branch_a);

        SampleType branch_b = x;
        for (auto& s : b_) branch_b = s.process(branch_b);

        // The one-sample delay on the reference branch is half of what puts the
        // two in quadrature; the coefficient sets are the other half.
        const Outputs out{delayed_, branch_b};
        delayed_ = branch_a;
        return out;
    }

private:
    struct Section {
        SampleType coefficient = 0;
        SampleType x1 = 0, x2 = 0, y1 = 0, y2 = 0;

        SampleType process(SampleType x) {
            const SampleType y = coefficient * (x + y2) - x2;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = snap_to_zero(y);
            return y;
        }
    };

    static constexpr std::size_t kSections = 4;

    static constexpr double kBranchA[kSections] = {0.6923878, 0.9360654322959,
                                                   0.9882295226860, 0.9987488452737};
    static constexpr double kBranchB[kSections] = {0.4021921162426, 0.8561710882420,
                                                   0.9722909545651, 0.9952884791278};

    std::array<Section, kSections> a_ = make_branch(kBranchA);
    std::array<Section, kSections> b_ = make_branch(kBranchB);
    SampleType delayed_ = 0;

    static std::array<Section, kSections> make_branch(const double (&coefficients)[kSections]) {
        std::array<Section, kSections> branch{};
        for (std::size_t i = 0; i < kSections; ++i) {
            // The published constants are the pole positions; the section's
            // coefficient is their square.
            branch[i].coefficient =
                static_cast<SampleType>(coefficients[i] * coefficients[i]);
        }
        return branch;
    }
};

/// Shifts every frequency in a signal by the same number of Hz.
///
/// This is not a pitch shift and the difference is the entire point. A pitch
/// shift multiplies every partial by a ratio, so a harmonic series stays
/// harmonic and the sound keeps its identity at a new pitch. A frequency shift
/// *adds* a constant, so a series at 100, 200, 300 Hz becomes 130, 230, 330 Hz
/// — no longer integer multiples of anything. The result has no fundamental and
/// no pitch, which is exactly what is wanted when a bank of resonators is
/// ringing on a chord and the chord is the problem: a shift of a few tens of
/// hertz turns tuned metal into a cymbal.
///
/// Implemented by single-sideband modulation: split the input into a quadrature
/// pair, modulate each by a sine and cosine at the shift frequency, and
/// subtract. Adding instead of subtracting selects the other sideband, which is
/// why the shift can go down as well as up.
///
/// RT contract: `set_sample_rate` and `set_shift_hz` are scalar updates.
/// `process()` and `reset()` allocate nothing and take no locks.
template <typename SampleType = float>
class FrequencyShifterT {
public:
    void set_sample_rate(double sr) {
        sample_rate_ = sr > 0.0 ? sr : sample_rate_;
        update();
    }

    /// Amount to add to every frequency, in Hz. Negative shifts downward.
    void set_shift_hz(double hz) {
        shift_hz_ = hz;
        update();
    }

    double shift_hz() const { return shift_hz_; }

    void reset() {
        hilbert_.reset();
        phase_ = 0.0;
    }

    SampleType process(SampleType input) {
        const auto pair = hilbert_.process(static_cast<double>(input));

        phase_ += increment_;
        if (phase_ >= 1.0) phase_ -= std::floor(phase_);
        if (phase_ < 0.0) phase_ -= std::floor(phase_);

        const double angle = 2.0 * 3.14159265358979323846 * phase_;
        return static_cast<SampleType>(pair.in_phase * std::cos(angle) -
                                       pair.quadrature * std::sin(angle));
    }

private:
    void update() { increment_ = shift_hz_ / sample_rate_; }

    HilbertPairT<double> hilbert_;
    double sample_rate_ = 44100.0;
    double shift_hz_ = 0.0;
    double increment_ = 0.0;
    double phase_ = 0.0;
};

using HilbertPair = HilbertPairT<double>;
using FrequencyShifter = FrequencyShifterT<float>;
using FrequencyShifter64 = FrequencyShifterT<double>;

}  // namespace pulp::signal
