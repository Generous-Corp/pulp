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
/// Cutoff convention: `set_frequency()` means what it means on `LadderFilter`
/// and `Svf`. Those two define their cutoff as the filter's characteristic
/// frequency, where the resonance peak sits and where the filter self-oscillates
/// (the transistor ladder's -3 dB point at zero resonance is 0.435 of it, not
/// at it). Here too it is the self-oscillation / resonance-peak frequency.
///
/// The unresonant -3 dB corner is far below it. In the analog prototype it is
/// kUnresonantCornerRatio (about 0.106) of the oscillation frequency, lower
/// than the transistor ladder's 0.435 because the four diode-coupled poles are
/// spread wide. Only the oscillation frequency is prewarped, so in the digital
/// filter the ratio holds at low cutoffs and rises with the cutoff: at 48 kHz
/// the corner is 0.106 of a 1 kHz cutoff, 0.110 at 5 kHz, 0.124 at 10 kHz,
/// 0.16 at 15 kHz, 0.29 at 20 kHz and 0.83 at the 0.49 fs clamp (a 19.6 kHz
/// corner). corner_frequency_hz() and frequency_for_corner_hz() convert
/// exactly in both directions, and set_cutoff_reference(CutoffReference::corner)
/// makes set_frequency() name the corner instead.
///
/// Two consequences worth knowing. At zero resonance and the same setting the
/// corner sits about two octaves below `LadderFilter`'s and three below a
/// Butterworth-damped `Svf`'s, and
/// a hardware TB-303 cutoff control is usually described by its corner, so a
/// knob that should track brightness reads better in the corner reference.
/// Texts that define the diode ladder by its one-pole coupling rate `fc`
/// (Zavalishin, Pirkle) have it oscillate at `fc / sqrt(2)` (0.7071 fc) with
/// the corner at 0.0747 fc; to reproduce a setting quoted in that convention,
/// call `set_frequency(fc / sqrt(2))`.
///
/// The chain reaches the stability edge at `w = sqrt(2)` in the units above, so
/// the integrator rate is the cutoff over `sqrt(2)`. `set_resonance(r)` maps `r`
/// to `k = 17 r`, so `r = 1` is the oscillation threshold; above it the
/// saturator `T(x) = tanh(d x) / d` limits the amplitude.
///
/// Discretization: topology-preserving transform (trapezoidal) integrators.
/// With `g` the integrator gain after prewarping, the four stage outputs are
/// `y = (I - g A)^-1 (s + g b u)` with the inverse rebuilt only when the cutoff
/// or sample rate changes (resonance, drive and bass compensation update two
/// scalars). Per-sample cutoff modulation is supported and safe; each cutoff
/// change costs one 4x4 inversion (a few hundred flops), so control-rate
/// cutoff updates are cheaper. The resonance loop is solved without a unit delay, and with the saturator
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
    /// It holds for the analog prototype and for low cutoffs; the digital
    /// ratio rises with the cutoff (see the file comment and
    /// corner_frequency_hz()).
    static constexpr double kUnresonantCornerOmega = 0.14935631160558277;
    static constexpr double kUnresonantCornerRatio = kUnresonantCornerOmega / kOscillationOmega;
    /// What set_frequency() names: the self-oscillation / resonance-peak
    /// frequency (the default, matching LadderFilter and Svf) or the
    /// unresonant -3 dB corner, converted with frequency_for_corner_hz().
    enum class CutoffReference { oscillation, corner };

    /// Largest set_frequency() value, as a fraction of the sample rate.
    static constexpr double kMaxFrequencyRatio = 0.49;

    /// The unresonant -3 dB corner, in Hz, of the digital filter whose
    /// self-oscillation frequency is `frequency_hz`. Exact (the bilinear warp
    /// of the analog corner), and clamped like set_frequency() to 1 Hz ..
    /// 0.49 of the sample rate.
    static double corner_frequency_hz(double sample_rate, double frequency_hz) {
        if (!(sample_rate > 0.0) || !std::isfinite(frequency_hz))
            return 0.0;
        const double hz = std::clamp(frequency_hz, 1.0, kMaxFrequencyRatio * sample_rate);
        const double g = std::tan(kPi * hz / sample_rate) / kOscillationOmega;
        return std::atan(kUnresonantCornerOmega * g) * sample_rate / kPi;
    }
    /// The self-oscillation frequency that puts the unresonant -3 dB corner at
    /// `corner_hz`: the inverse of corner_frequency_hz(). The highest corner
    /// reachable is corner_frequency_hz() at the 0.49 fs clamp (about 0.41 of
    /// the sample rate); a higher request returns the clamp.
    static double frequency_for_corner_hz(double sample_rate, double corner_hz) {
        if (!(sample_rate > 0.0) || !std::isfinite(corner_hz))
            return 0.0;
        const double limit = kMaxFrequencyRatio * sample_rate;
        const double c = std::clamp(corner_hz, 0.0, 0.4999 * sample_rate);
        const double g = std::tan(kPi * c / sample_rate) / kUnresonantCornerOmega;
        const double hz = std::atan(kOscillationOmega * g) * sample_rate / kPi;
        return std::min(hz, limit);
    }

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
    /// The self-oscillation frequency in Hz by default (prewarped; the
    /// unresonant -3 dB corner is far below it, see corner_frequency_hz()), or
    /// the corner itself in CutoffReference::corner.
    void set_frequency(SampleType hz) {
        if (std::isfinite(static_cast<double>(hz)))
            freq_ = hz;
        update();
    }
    /// Choose what set_frequency() names. The stored frequency is
    /// reinterpreted, and kept through later sample-rate changes.
    void set_cutoff_reference(CutoffReference reference) {
        reference_ = reference;
        update();
    }
    CutoffReference cutoff_reference() const {
        return reference_;
    }
    /// The self-oscillation frequency in force, in Hz, after the reference
    /// conversion and clamping.
    double oscillation_frequency_hz() const {
        return oscillation_hz_;
    }
    /// 0 .. 1.25; 1 is the oscillation threshold (feedback k = 17).
    void set_resonance(SampleType r) {
        if (std::isfinite(static_cast<double>(r)))
            resonance_ = r;
        update_feedback();
    }
    /// Feedback saturator `tanh(d x) / d`; 0 is a linear loop.
    void set_drive(SampleType d) {
        if (std::isfinite(static_cast<double>(d)))
            drive_ = std::clamp(d, SampleType{0}, kMaxDrive);
        update_feedback();
    }
    /// 0 .. 1: scales the input by `1 + amount * k`, so 1 keeps the passband
    /// gain at unity for every resonance (the hardware does not do this).
    void set_bass_compensation(SampleType amount) {
        if (std::isfinite(static_cast<double>(amount)))
            compensation_ = std::clamp(amount, SampleType{0}, SampleType{1});
        update_feedback();
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

    static constexpr double kPi = 3.14159265358979323846;

    void update() {
        update_matrix();
        update_feedback();
    }

    /// The parts that depend on the cutoff and sample rate: the inverse of
    /// `I - g A` and the input column.
    void update_matrix() {
        const double fs = static_cast<double>(sample_rate_);
        const double requested = reference_ == CutoffReference::corner
                                     ? frequency_for_corner_hz(fs, static_cast<double>(freq_))
                                     : static_cast<double>(freq_);
        const double hz = std::clamp(requested, 1.0, kMaxFrequencyRatio * fs);
        oscillation_hz_ = hz;
        const double analog = std::tan(kPi * hz / fs);
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
    }

    /// The parts that depend on resonance, drive and bass compensation.
    void update_feedback() {
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
    CutoffReference reference_ = CutoffReference::oscillation;
    double oscillation_hz_ = 1000.0;

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
