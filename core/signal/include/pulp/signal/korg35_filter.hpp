#pragma once

/// @file korg35_filter.hpp
/// Two-pole lowpass and highpass filter in the Sallen-Key family used by the
/// Korg35 chip, with a saturating feedback path and zero-delay feedback.
///
/// Topology (normalized so the cutoff is 1 rad/s, `L = 1/(1+s)`, `H = s/(1+s)`
/// one-pole lowpass and highpass sections, `k` the feedback amount):
///
/// ```
///   lowpass :  x -> L -> (+) -> L -> y       feedback  k * H(y)  into (+)
///   highpass:  x -> H -> (+) -> H -> y       feedback  k * L(y)  into (+)
/// ```
///
/// which gives the linear responses
///
/// ```
///   lowpass : 1 / (s^2 + (2 - k) s + 1)
///   highpass: s^2 / (s^2 + (2 - k) s + 1)
/// ```
///
/// The damping `2 - k` reaches zero at `k = 2`: the poles sit on the imaginary
/// axis at the cutoff, so `k < 2` is a stable resonant filter whose peak
/// approaches infinity at the cutoff, and `k >= 2` self-oscillates at the
/// cutoff when the feedback saturates. `set_resonance(r)` maps `r` to `k = 2 r`,
/// so `r = 1` is exactly the self-oscillation threshold. Because the saturator
/// has unit slope at the origin, the linear threshold is also where the
/// nonlinear filter starts to oscillate, and amplitude then grows until the
/// saturator limits it; to get a sustained tone use `r` slightly above 1.
///
/// Discretization: each one-pole section is a topology-preserving transform
/// integrator (trapezoidal), so the cutoff is exact at every frequency after
/// prewarping, and the instantaneous feedback loop is solved without a unit
/// delay. With the saturator `T(x) = tanh(d x) / d` in the loop the unknown
/// reduces to one scalar equation `h = c + a T(h)` with `0 <= a < 0.625`,
/// solved by a fixed number of Newton steps (the function is monotonic, so the
/// iteration converges from the closed-form linear guess). `d = 0` selects the
/// linear loop and the exact closed form.
///
/// References: Vadim Zavalishin, "The Art of VA Filter Design", chapters 3
/// (zero-delay feedback), 5 and 6 (nonlinear TPT filters); Will Pirkle,
/// "Designing Software Synthesizer Plug-Ins in C++", 2nd ed., the Korg35
/// virtual-analog filter chapter.
///
/// RT contract: setters recompute scalar coefficients; `process()` and
/// `reset()` allocate nothing and run a fixed, bounded amount of work.

#include <pulp/signal/denormal.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pulp::signal {

template <typename SampleType = float> class Korg35FilterT {
  public:
    enum class Mode { lowpass, highpass };

    /// Feedback amount at which the linear filter self-oscillates.
    static constexpr SampleType kSelfOscillationFeedback = SampleType{2};
    /// Largest `set_resonance()` value. Above 1 the filter self-oscillates and
    /// the saturator sets the amplitude.
    static constexpr SampleType kMaxResonance = SampleType{1.25};
    /// Largest resonance when the feedback loop is linear (drive 0), kept just
    /// below the stability edge.
    static constexpr SampleType kMaxLinearResonance = SampleType{0.9999};
    static constexpr SampleType kMaxDrive = SampleType{16};

    Korg35FilterT() {
        update();
    }

    void set_mode(Mode mode) {
        mode_ = mode;
    }
    void set_sample_rate(SampleType sr) {
        if (sr > SampleType{0})
            sample_rate_ = sr;
        update();
    }
    void set_frequency(SampleType hz) {
        if (std::isfinite(static_cast<double>(hz)))
            freq_ = hz;
        update();
    }
    /// 0 .. 1.25; 1 is the self-oscillation threshold (feedback k = 2).
    void set_resonance(SampleType r) {
        if (std::isfinite(static_cast<double>(r)))
            resonance_ = r;
        update();
    }
    /// Saturator drive `d` in the feedback path, `T(x) = tanh(d x) / d`. The
    /// small-signal gain is 1 for every `d`; 0 makes the loop linear.
    void set_drive(SampleType d) {
        if (std::isfinite(static_cast<double>(d)))
            drive_ = std::clamp(d, SampleType{0}, kMaxDrive);
        update();
    }

    /// Install a coherent set in one prewarp.
    void configure(Mode mode, SampleType sample_rate, SampleType frequency, SampleType resonance,
                   SampleType drive = SampleType{1}) {
        mode_ = mode;
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

    Mode mode() const {
        return mode_;
    }
    /// The feedback amount `k` in force after clamping.
    SampleType feedback() const {
        return k_;
    }

    /// Magnitude of the linear (unsaturated) prototype at `omega`, the
    /// frequency in units of the cutoff, for feedback amount `k`.
    static double prototype_magnitude(Mode mode, double k, double omega) {
        const double re = 1.0 - omega * omega;
        const double im = (2.0 - k) * omega;
        const double den = std::sqrt(re * re + im * im);
        return (mode == Mode::lowpass ? 1.0 : omega * omega) / den;
    }

    SampleType process(SampleType x) {
        // A non-finite sample would poison the integrator memories for good.
        if (!std::isfinite(static_cast<double>(x))) {
            reset();
            return SampleType{0};
        }
        const SampleType G = G_;
        const SampleType Gc = SampleType{1} - G;
        const SampleType S1 = Gc * s1_;
        const SampleType S2 = Gc * s2_;
        const SampleType S3 = Gc * s3_;
        const SampleType lp1 = G * x + S1;

        SampleType y, lp3;
        if (mode_ == Mode::lowpass) {
            // h = highpass of y in the feedback section: h = c + a T(h).
            const SampleType c = Gc * (G * lp1 + S2) - S3;
            const SampleType h = solve(c);
            const SampleType w = lp1 + k_ * saturate(h);
            y = G * w + S2;
            lp3 = G * y + S3;
        } else {
            // h = lowpass of y in the feedback section.
            const SampleType hp1 = x - lp1;
            const SampleType c = G * (Gc * hp1 - S2) + S3;
            const SampleType h = solve(c);
            const SampleType w = hp1 + k_ * saturate(h);
            const SampleType lp2 = G * w + S2;
            y = w - lp2;
            lp3 = h;
            s2_ = snap_recursion_state(SampleType{2} * lp2 - s2_);
            s1_ = snap_recursion_state(SampleType{2} * lp1 - s1_);
            s3_ = snap_recursion_state(SampleType{2} * lp3 - s3_);
            return y;
        }
        s1_ = snap_recursion_state(SampleType{2} * lp1 - s1_);
        s2_ = snap_recursion_state(SampleType{2} * y - s2_);
        s3_ = snap_recursion_state(SampleType{2} * lp3 - s3_);
        return y;
    }

    void process(SampleType* buffer, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

    void reset() {
        s1_ = s2_ = s3_ = SampleType{0};
    }

    /// Solves `h = c + a T(h)` with `T(x) = tanh(drive x) / drive` (identity at
    /// drive 0). `F(h) = h - c - a T(h)` is monotonic for `a < 1`; Newton from
    /// the one-step fixed-point guess converges in a few iterations.
    static SampleType solve_loop(SampleType c, SampleType a, SampleType drive) {
        if (drive <= SampleType{0})
            return c / (SampleType{1} - a);
        SampleType h = c + a * (std::tanh(drive * c) / drive);
        for (int i = 0; i < kNewtonSteps; ++i) {
            const SampleType t = std::tanh(drive * h);
            const SampleType f = h - c - a * (t / drive);
            const SampleType df = SampleType{1} - a * (SampleType{1} - t * t);
            h -= f / df;
        }
        return h;
    }

  private:
    static constexpr int kNewtonSteps = 4;

    SampleType saturate(SampleType x) const {
        if (drive_ <= SampleType{0})
            return x;
        return std::tanh(drive_ * x) / drive_;
    }

    SampleType solve(SampleType c) const {
        return solve_loop(c, a_, drive_);
    }

    void update() {
        const double nyquist_limit = 0.49 * static_cast<double>(sample_rate_);
        const double hz = std::clamp(static_cast<double>(freq_), 1.0, nyquist_limit);
        const double g = std::tan(3.14159265358979323846 * hz / static_cast<double>(sample_rate_));
        G_ = static_cast<SampleType>(g / (1.0 + g));
        const SampleType r_max = drive_ > SampleType{0} ? kMaxResonance : kMaxLinearResonance;
        k_ = kSelfOscillationFeedback * std::clamp(resonance_, SampleType{0}, r_max);
        a_ = k_ * G_ * (SampleType{1} - G_);
    }

    Mode mode_ = Mode::lowpass;
    SampleType sample_rate_ = SampleType{44100};
    SampleType freq_ = SampleType{1000};
    SampleType resonance_ = SampleType{0};
    SampleType drive_ = SampleType{1};
    SampleType G_ = 0;
    SampleType k_ = 0;
    SampleType a_ = 0;
    SampleType s1_ = 0, s2_ = 0, s3_ = 0;
};

using Korg35Filter = Korg35FilterT<float>;
using Korg35Filter64 = Korg35FilterT<double>;

} // namespace pulp::signal
