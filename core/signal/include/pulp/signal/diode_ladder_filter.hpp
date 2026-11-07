#pragma once

/// @file diode_ladder_filter.hpp
/// Four-stage diode-ladder lowpass (the TB-303 style filter) with zero-delay
/// feedback and a saturating resonance path.
///
/// Unlike the transistor ladder, where each stage only loads the previous one,
/// the diode ladder couples every capacitor to both of its neighbours through
/// the diode pairs. In normalized form (every integrator `y' = v` with the
/// coupling rates below, `u` the input after resonance feedback):
///
/// ```
///   y1' = (u  - y1) + (y2 - y1)
///   y2' = (y1 - y2) + (y3 - y2)
///   y3' = (y2 - y3) + (y4 - y3)
///   y4' = 2 (y3 - y4)
///   u   = x - k T(y4)
/// ```
///
/// The last stage has no neighbour on its output side, hence the factor 2.
/// This coupled chain has four real poles that are spread apart rather than
/// coincident, so the response bends more gently around the cutoff than a
/// transistor ladder does, and the filter self-oscillates at the feedback
/// `k = 17` (the closed-loop poles reach the imaginary axis there). The DC gain
/// is `1 / (1 + k)`: bass is lost as resonance rises, as on the hardware;
/// `set_bass_compensation()` restores it.
///
/// `set_frequency()` is the frequency the filter self-oscillates at (the chain
/// reaches the stability edge at `w = sqrt(2)` in the units above, so the
/// integrator rate is the cutoff over `sqrt(2)`). The unresonant -3 dB corner
/// lies far lower, at about 0.106 of that frequency before warping, because the
/// four poles are spread over a wide range; kUnresonantCornerRatio gives the
/// number. `set_resonance(r)` maps `r` to `k = 17 r`, so `r = 1` is the oscillation
/// threshold; above it the saturator `T(x) = tanh(d x) / d` limits the
/// amplitude.
///
/// Discretization: topology-preserving transform (trapezoidal) integrators.
/// With `g` the integrator gain after prewarping, the four stage outputs are
/// `y = (I - g A)^-1 (s + g b u)` with the inverse built once per cutoff change,
/// the resonance loop is solved without a unit delay, and with the saturator
/// the scalar loop equation `y4 + c k T(y4) = d` is solved by safeguarded
/// Newton iteration (the left side is monotonic, so a bracket always exists).
///
/// References: Vadim Zavalishin, "The Art of VA Filter Design", chapter 6
/// (the diode ladder filter); Will Pirkle, "Designing Software Synthesizer
/// Plug-Ins in C++", 2nd ed., the diode ladder chapter.
///
/// RT contract: setters recompute scalar and fixed-size array coefficients;
/// `process()` and `reset()` allocate nothing and do bounded work.

#include <pulp/signal/denormal.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace pulp::signal {

template <typename SampleType = float> class DiodeLadderFilterT {
  public:
    /// Feedback amount at which the linear filter reaches the stability edge.
    static constexpr double kSelfOscillationFeedback = 17.0;
    /// Frequency at which the closed loop reaches the stability edge, in
    /// rad per unit coupling rate: the open-loop phase is -180 degrees there
    /// and the open-loop gain is 1/17.
    static constexpr double kOscillationOmega = 1.4142135623730951;
    /// The -3 dB corner of the unresonant response in the same units, found by
    /// solving `|H(jw)|^2 = 1/2`, as a fraction of the oscillation frequency.
    static constexpr double kUnresonantCornerRatio = 0.14935631160558277 / kOscillationOmega;
    static constexpr SampleType kMaxResonance = SampleType{1.25};
    static constexpr SampleType kMaxLinearResonance = SampleType{0.9999};
    static constexpr SampleType kMaxDrive = SampleType{16};

    DiodeLadderFilterT() {
        update();
    }

    void set_sample_rate(SampleType sr) {
        if (sr > SampleType{0})
            sample_rate_ = sr;
        update();
    }
    /// Self-oscillation frequency in Hz (prewarped; the unresonant -3 dB corner
    /// is far below it, see kUnresonantCornerRatio).
    void set_frequency(SampleType hz) {
        if (std::isfinite(static_cast<double>(hz)))
            freq_ = hz;
        update();
    }
    /// 0 .. 1.25; 1 is the oscillation threshold (feedback k = 17).
    void set_resonance(SampleType r) {
        if (std::isfinite(static_cast<double>(r)))
            resonance_ = r;
        update();
    }
    /// Feedback saturator `tanh(d x) / d`; 0 is a linear loop.
    void set_drive(SampleType d) {
        if (std::isfinite(static_cast<double>(d)))
            drive_ = std::clamp(d, SampleType{0}, kMaxDrive);
        update();
    }
    /// 0 .. 1: scales the input by `1 + amount * k`, so 1 keeps the passband
    /// gain at unity for every resonance (the hardware does not do this).
    void set_bass_compensation(SampleType amount) {
        if (std::isfinite(static_cast<double>(amount)))
            compensation_ = std::clamp(amount, SampleType{0}, SampleType{1});
        update();
    }

    void configure(SampleType sample_rate, SampleType frequency, SampleType resonance,
                   SampleType drive = SampleType{1}) {
        if (sample_rate > SampleType{0})
            sample_rate_ = sample_rate;
        if (std::isfinite(static_cast<double>(frequency)))
            freq_ = frequency;
        if (std::isfinite(static_cast<double>(resonance)))
            resonance_ = resonance;
        if (std::isfinite(static_cast<double>(drive)))
            drive_ = std::clamp(drive, SampleType{0}, kMaxDrive);
        update();
    }

    /// The feedback amount `k` in force after clamping.
    SampleType feedback() const {
        return k_;
    }

    SampleType process(SampleType x) {
        // A non-finite sample would poison the stage memories for good.
        if (!std::isfinite(static_cast<double>(x))) {
            reset();
            return SampleType{0};
        }
        // Contribution of the stored state to every stage output.
        std::array<SampleType, 4> ms{};
        for (int i = 0; i < 4; ++i) {
            SampleType acc = 0;
            for (int j = 0; j < 4; ++j)
                acc += minv_[i][j] * s_[j];
            ms[i] = acc;
        }
        const SampleType in = x * input_gain_;
        const SampleType d = ms[3] + c_ * in;
        const SampleType y4 = solve_loop(d, c_ * k_, drive_);
        const SampleType u = in - k_ * saturate(y4, drive_);
        for (int i = 0; i < 4; ++i) {
            const SampleType y = ms[i] + gm_[i] * u;
            s_[i] = snap_recursion_state(SampleType{2} * y - s_[i]);
        }
        return y4;
    }

    void process(SampleType* buffer, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

    void reset() {
        s_.fill(SampleType{0});
    }

    /// Solves `y + ck T(y) = d` for the loop output, where `T(x) = tanh(d x)/d`
    /// is the feedback saturator (`drive` 0 means identity). Safeguarded Newton:
    /// the root lies in `[d - ck/drive, d + ck/drive]` because `|T| <= 1/drive`.
    static SampleType solve_loop(SampleType d, SampleType ck, SampleType drive) {
        if (drive <= SampleType{0} || ck <= SampleType{0})
            return d / (SampleType{1} + ck);
        const SampleType reach = ck / drive;
        SampleType lo = d - reach, hi = d + reach;
        // Start in the right regime: inside the steep part (|y| < 1/drive) the
        // slope is 1 + ck, outside it the saturator contributes a constant.
        SampleType y = d / (SampleType{1} + ck);
        if (std::abs(y) * drive >= SampleType{1})
            y = d - std::copysign(reach, d);
        y = std::clamp(y, lo, hi);
        for (int i = 0; i < kNewtonSteps; ++i) {
            const SampleType t = std::tanh(drive * y);
            const SampleType f = y + ck * (t / drive) - d;
            if (f == SampleType{0})
                break;
            if (f > 0)
                hi = y;
            else
                lo = y;
            const SampleType df = SampleType{1} + ck * (SampleType{1} - t * t);
            SampleType next = y - f / df;
            if (!(next >= lo && next <= hi))
                next = SampleType{0.5} * (lo + hi);
            if (std::abs(next - y) <= kSolveTolerance * (SampleType{1} + std::abs(y))) {
                y = next;
                break;
            }
            y = next;
        }
        return y;
    }

  private:
    static constexpr int kNewtonSteps = 12;
    static constexpr SampleType kSolveTolerance =
        SampleType{16} * std::numeric_limits<SampleType>::epsilon();

    static SampleType saturate(SampleType x, SampleType drive) {
        if (drive <= SampleType{0})
            return x;
        return std::tanh(drive * x) / drive;
    }

    void update() {
        const double nyquist_limit = 0.49 * static_cast<double>(sample_rate_);
        const double hz = std::clamp(static_cast<double>(freq_), 1.0, nyquist_limit);
        const double analog =
            std::tan(3.14159265358979323846 * hz / static_cast<double>(sample_rate_));
        const double g = analog / kOscillationOmega;

        // M = I - g A, A the coupling matrix of the chain above.
        double m[4][8] = {};
        const double a[4][4] = {{-2, 1, 0, 0}, {1, -2, 1, 0}, {0, 1, -2, 1}, {0, 0, 2, -2}};
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j)
                m[i][j] = (i == j ? 1.0 : 0.0) - g * a[i][j];
            m[i][4 + i] = 1.0;
        }
        // Gauss-Jordan; M is a diagonally dominant M-matrix, so no pivoting is needed.
        for (int i = 0; i < 4; ++i) {
            const double pivot = m[i][i];
            for (int j = 0; j < 8; ++j)
                m[i][j] /= pivot;
            for (int r = 0; r < 4; ++r) {
                if (r == i)
                    continue;
                const double f = m[r][i];
                for (int j = 0; j < 8; ++j)
                    m[r][j] -= f * m[i][j];
            }
        }
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j)
                minv_[i][j] = static_cast<SampleType>(m[i][4 + j]);
            gm_[i] = static_cast<SampleType>(g * m[i][4]);
        }
        c_ = gm_[3];

        const SampleType r_max = drive_ > SampleType{0} ? kMaxResonance : kMaxLinearResonance;
        k_ = static_cast<SampleType>(kSelfOscillationFeedback) *
             std::clamp(resonance_, SampleType{0}, r_max);
        input_gain_ = SampleType{1} + compensation_ * k_;
    }

    SampleType sample_rate_ = SampleType{44100};
    SampleType freq_ = SampleType{1000};
    SampleType resonance_ = SampleType{0};
    SampleType drive_ = SampleType{1};
    SampleType compensation_ = SampleType{0};

    std::array<std::array<SampleType, 4>, 4> minv_{};
    std::array<SampleType, 4> gm_{}; // g * (inverse column 0): effect of u on each stage
    SampleType c_ = 0;               // the same for stage 4
    SampleType k_ = 0;
    SampleType input_gain_ = SampleType{1};
    std::array<SampleType, 4> s_{};
};

using DiodeLadderFilter = DiodeLadderFilterT<float>;
using DiodeLadderFilter64 = DiodeLadderFilterT<double>;

} // namespace pulp::signal
