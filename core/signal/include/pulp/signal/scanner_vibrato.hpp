#pragma once

#include <pulp/signal/dc_blocker.hpp>
#include <pulp/signal/delay_line.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/detail/leslie_common.hpp>
#include <pulp/signal/interpolator.hpp>
#include <pulp/signal/lfo.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

// ═══════════════════════════════════════════════════════════════════════════
//  Hammond scanner vibrato / chorus
// ═══════════════════════════════════════════════════════════════════════════

/// The console's six-position vibrato/chorus switch, plus off.
enum class ScannerMode : std::uint8_t {
    off,  ///< Bit-exact dry passthrough.
    v1,   ///< Shallow vibrato — scanner output only.
    v2,   ///< Medium vibrato.
    v3,   ///< Full vibrato.
    c1,   ///< Shallow chorus — scanner plus dry.
    c2,   ///< Medium chorus.
    c3,   ///< Full chorus. The lush one.
};

/// The motor-scanned tapped delay line.
template <typename SampleType = float>
class ScannerVibratoT {
public:
    using Mode = ScannerMode;

    /// [design parameter] scanner rate 6.9 Hz, range 6.0 .. 7.5 Hz.
    ///
    /// Honest-gap note: the documented Hammond vibrato sweeps at roughly 7 Hz
    /// and the scanner is geared off the synchronous motor, so the rate is
    /// mains-locked rather than free. The exact figure is model- and
    /// mains-dependent and was not pinned to a verified primary source, so this
    /// is a calibration constant, not a citation.
    static constexpr double kScanHz = 6.9;

    /// [design parameter] the 50 Hz-mains rate, 5.75 Hz, range 4.5 .. 6.5 Hz.
    /// Derived rather than asserted: `kScanHz · 50/60`, the mains-ratio scaling
    /// of the same motor gearing, because the European rate was not
    /// independently verified either and inventing a second number would be
    /// worse than scaling the first.
    static constexpr double kScanHz50 = kScanHz * 50.0 / 60.0;

    /// [design parameter] total line delay 1.0 ms, range 0.6 .. 1.4 ms.
    /// Documented line-box delay is about a millisecond.
    static constexpr double kLineDelayMs = 1.0;

    /// [design parameter] 18 taps, range 12 .. 24.
    ///
    /// This is a build-time calibration constant and deliberately NOT a live
    /// parameter, because it does not appear in the shipped arithmetic at all —
    /// see `process` for why. It records how many taps the physical line box is
    /// being taken to have; the range is bounded by "enough that the tap
    /// crossfade is indistinguishable from a continuous sweep".
    static constexpr int kScannerTaps = 18;

    /// [design parameter] the three vibrato depths, as fractions of the line.
    /// The ORDERING is documented behaviour; the values are engineering
    /// defaults.
    static constexpr double kV1 = 0.33;
    static constexpr double kV2 = 0.66;
    static constexpr double kV3 = 1.00;

    /// [design parameter] chorus dry blend 0.5, range 0 .. 1. That chorus is
    /// "vibrato plus dry" is documented; the equal blend is a default.
    static constexpr double kChorusMix = 0.5;

    /// Parameter-range maxima used for buffer sizing.
    static constexpr double kMaxLineDelayMs = 1.4;

    /// The measured constructive-sum bound, in linear gain. Feedforward, so
    /// this is dry + wet peaking together, not a loop gain. The suite's sweep
    /// measures 1.16 across the whole mode and parameter space; this is that
    /// maximum rounded up, with the headroom covering grid coarseness rather
    /// than covering a guess.
    /// [design parameter] default 1.5 (+3.5 dB), range 1.2 .. 4.0.
    static constexpr double kWorstCaseGain = 1.5;

    ScannerVibratoT() { update(); }

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        line_.prepare(worst_case_delay_samples(sample_rate_));
        scanner_.prepare(sample_rate_);
        scanner_.set_wave(LfoWave::triangle);
        blocker_.set_pole(static_cast<SampleType>(
            1.0 - kDcCornerHz * leslie_detail::kTwoPi / sample_rate_));
        update();
        reset();
    }

    void reset() {
        line_.reset();
        scanner_.reset();
        blocker_.reset();
    }

    void discard_history() noexcept {
        line_.discard_history();
        scanner_.reset();
        blocker_.reset();
    }

    void set_mode(Mode mode) {
        mode_ = mode;
        update();
    }

    Mode mode() const { return mode_; }

    void set_scan_hz(double hz) {
        if (!std::isfinite(hz)) return;
        scan_hz_ = std::clamp(hz, 0.0, 100.0);
        update();
    }

    void set_line_ms(double ms) {
        if (!std::isfinite(ms)) return;
        line_ms_ = std::clamp(ms, 0.0, kMaxLineDelayMs);
        update();
    }

    void set_v1_frac(double frac) {
        if (!std::isfinite(frac)) return;
        v1_ = std::clamp(frac, 0.0, 1.0);
        update();
    }
    void set_v2_frac(double frac) {
        if (!std::isfinite(frac)) return;
        v2_ = std::clamp(frac, 0.0, 1.0);
        update();
    }
    void set_v3_frac(double frac) {
        if (!std::isfinite(frac)) return;
        v3_ = std::clamp(frac, 0.0, 1.0);
        update();
    }

    void set_chorus_mix(double mix01) {
        if (!std::isfinite(mix01)) return;
        chorus_mix_ = std::clamp(mix01, 0.0, 1.0);
        update();
    }

    // ── Observables ───────────────────────────────────────────────────────

    /// The fraction of the line the scanner currently traverses.
    double depth_fraction() const { return depth_frac_; }

    /// How much dry is blended back. Zero in every V mode — that is what makes
    /// V and C different effects rather than two depths of one effect.
    double dry_mix() const { return dry_mix_; }

    /// The peak fractional pitch shift this setting produces, `|Δf/f|`.
    ///
    /// A linearly ramping delay is a CONSTANT pitch shift, so the depth follows
    /// from the slope alone: the scanner traverses `depth·line` seconds of
    /// delay in each half period, giving `Δf/f = depth·line / (1/(2·rate))`.
    /// Published here so the acceptance test can predict the shift from the
    /// shipped constants instead of restating a number.
    double peak_pitch_shift_ratio() const {
        return depth_frac_ * line_ms_ * 0.001 * 2.0 * scan_hz_;
    }

    /// Zero, for the same reason as the Leslie: the line box's delay is the
    /// modelled electrical path, not processing latency.
    static constexpr int latency_samples() { return 0; }

    SampleType process(SampleType input) {
        if (!std::isfinite(static_cast<double>(input))) {
            discard_history();
            return SampleType{0};
        }
        if (mode_ == Mode::off) return input;

        const double dry = static_cast<double>(input);
        const double x = static_cast<double>(blocker_.process(input));

        // The scanner position, `[0, 1]`, from the shared LFO in triangle mode.
        // The triangle is the point, not a stand-in for a sine: a constant-slope
        // ramp is a constant pitch shift with a sign flip at each turnaround,
        // which is the boxier motion that keeps this from sounding like every
        // sine-LFO chorus. Do not smooth it.
        const double s = static_cast<double>(scanner_.next_unipolar());

        // Physically the pickup sits between two taps and the scanner
        // crossfades them:
        //
        //     tap_k delay = (k / (N−1)) · line_ms
        //     pos = depth · s · (N−1);  k = floor(pos);  f = pos − k
        //     wet = (1−f)·read(tap_k) + f·read(tap_{k+1})
        //
        // For an equally spaced line that linear crossfade between two taps IS
        // a linear interpolation of one continuous delay, so reading the single
        // fractional delay `depth·s·line_ms` directly is the same signal for
        // any tap count — and reading it with the cubic kernel is strictly
        // better than the two-tap linear blend. The tap count survives as
        // `kScannerTaps`, documenting what is being modelled, rather than as
        // arithmetic that would only coarsen the result.
        const double delay_samples = depth_frac_ * s * line_ms_ * 0.001 * sample_rate_;

        line_.push(static_cast<SampleType>(x));
        const int index = static_cast<int>(std::floor(delay_samples));
        const double frac = delay_samples - static_cast<double>(index);
        const double wet = Interpolator::lagrange(frac,
                                                  static_cast<double>(line_.read(index - 1)),
                                                  static_cast<double>(line_.read(index)),
                                                  static_cast<double>(line_.read(index + 1)),
                                                  static_cast<double>(line_.read(index + 2)));

        // V modes are pure pitch vibrato: the pitch moves, the level does not.
        // C modes add the dry back, so a stationary-pitch copy beats against a
        // moving-pitch one and the result is a sweeping comb — shimmer and
        // thickness rather than wobble. They are DIFFERENT EFFECTS, not two
        // points on a depth control.
        return static_cast<SampleType>(snap_to_zero(dry_mix_ * dry + (1.0 - dry_mix_) * wet));
    }

    void process_block(const SampleType* in, SampleType* out, int n) {
        for (int i = 0; i < n; ++i) out[i] = process(in[i]);
    }

    static int worst_case_delay_samples(double sample_rate) {
        return static_cast<int>(std::ceil(kMaxLineDelayMs * 0.001 * sample_rate)) +
               leslie_detail::kInterpolatorMargin;
    }

private:
    /// [design parameter] default 5 Hz, range 1 .. 20 Hz. Only has to clear
    /// offset ahead of the line; well below anything musical.
    static constexpr double kDcCornerHz = 5.0;

    void update() {
        switch (mode_) {
            case Mode::off:
                depth_frac_ = 0.0;
                dry_mix_ = 1.0;
                break;
            case Mode::v1:
                depth_frac_ = v1_;
                dry_mix_ = 0.0;
                break;
            case Mode::v2:
                depth_frac_ = v2_;
                dry_mix_ = 0.0;
                break;
            case Mode::v3:
                depth_frac_ = v3_;
                dry_mix_ = 0.0;
                break;
            case Mode::c1:
                depth_frac_ = v1_;
                dry_mix_ = chorus_mix_;
                break;
            case Mode::c2:
                depth_frac_ = v2_;
                dry_mix_ = chorus_mix_;
                break;
            case Mode::c3:
                depth_frac_ = v3_;
                dry_mix_ = chorus_mix_;
                break;
        }
        scanner_.set_rate_hz(scan_hz_);
    }

    double sample_rate_ = 48000.0;
    Mode mode_ = Mode::off;
    double scan_hz_ = kScanHz;
    double line_ms_ = kLineDelayMs;
    double v1_ = kV1;
    double v2_ = kV2;
    double v3_ = kV3;
    double chorus_mix_ = kChorusMix;
    double depth_frac_ = 0.0;
    double dry_mix_ = 1.0;

    DelayLineT<SampleType> line_{};
    EffectLfoT<double> scanner_{};
    DcBlocker<SampleType> blocker_{};
};

using ScannerVibrato = ScannerVibratoT<float>;
using ScannerVibrato64 = ScannerVibratoT<double>;

}  // namespace pulp::signal
