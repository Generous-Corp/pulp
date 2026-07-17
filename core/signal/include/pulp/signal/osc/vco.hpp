#pragma once

/// @file vco.hpp
/// OSC-VCO: the deterministic signal path of a circuit-flavored analog voltage-
/// controlled oscillator, built by composing the bandlimited core (`VaOscillator`
/// — itself `PhaseAccumulator` + the BLEP/BLAMP kernels) with the memoryless and
/// LTI stages that give an analog VCO its character.
///
/// ── What this is, and what it is NOT ──────────────────────────────────────
///
/// This is the *deterministic* front-to-back path only: a pitch-control front
/// end, the bandlimited core shape, an integrator-leak "bow" on the ramp, a
/// per-shape lumped waveshaper, a level-vs-pitch curve, and an output DC-blocking
/// (AC-coupling) stage. Every one of those is a hand-set parameter with a
/// physically-motivated meaning, and every one DEFAULTS TO ITS NEUTRAL VALUE, so
/// a default-constructed OSC-VCO is bit-for-bit a `VaOscillator` (see the null in
/// `test_osc_vco.cpp`).
///
/// It is NOT the whole VCO. **Drift and jitter are deliberately absent as
/// behavior**: the per-sample `increment` argument to `next()` is where a pitch-
/// noise source modulates the clock, and `set_drift_depth`/`set_jitter_depth`
/// reserve the knobs so the API need not change — but no stochastic source is
/// wired here, because separating drift from jitter needs the time-domain
/// pitch/drift analyzer that does not exist yet. The depths are inert until then,
/// and a test pins that they are (a reserved knob that silently half-worked would
/// be worse than an absent one). The offline fitting tool and data-driven
/// profiles are also out of scope: this is the ENGINE that has the deterministic
/// parameters, hand-set, not the estimator that recovers them.
///
/// ── Circuit-accuracy of the correction, honestly ─────────────────────────
///
/// The bow and the waveshaper are applied as memoryless maps of the core's
/// already-bandlimited output. That is the standard analog-modeling composition,
/// and it is honest about one thing: a memoryless nonlinearity on a bandlimited
/// signal reintroduces some aliasing (the analog stages alias a little too), and
/// the bow's reshape breaks the ramp's slope at the wrap without a fresh BLAMP.
/// The core's own discontinuity correction is intact and measured with the
/// character neutral; the bow is therefore gated for level/DC correctness, not
/// for alias rejection. A circuit-accurate wrap correction for a bowed ramp would
/// need the core's correction loop to accept a caller-supplied shape (value +
/// slope + internal thresholds) rather than its four fixed shapes — an extraction
/// worth doing if a later increment needs it, and called out rather than done by
/// copying the loop here.
///
/// RT contract: no allocation, no locks, no I/O on the per-sample path. `double`
/// throughout, matching the core; a `float` caller narrows once on store. The
/// bow's `exp` and the waveshaper's `tanh` are libcalls, like the core's `sin` —
/// none of them allocates, locks, or blocks.

#include "blep.hpp"
#include "phase.hpp"
#include "va.hpp"

#include <pulp/signal/dc_blocker.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace pulp::signal::osc {

/// The analog exponential pitch converter, as a pure control-voltage → frequency
/// map. 1 V/octave nominal; the parameters are the ways a real converter departs
/// from ideal, all hand-set (a fitting tool would recover them from a pitch-vs-
/// note sweep — see the identifiability table — but nothing here optimizes them).
///
/// Every field is neutral by default, so a default `VcoTuning` is exactly
/// `reference_hz * 2^(control_volts / volts_per_octave)`.
struct VcoTuning {
    /// Frequency produced at 0 V with no calibration offset.
    double reference_hz = 261.625565; // C4.
    /// Volts spanning one octave (the converter's nominal sensitivity).
    double volts_per_octave = 1.0;
    /// Per-oscillator calibration offset, in cents. Shifts every note equally.
    double tune_offset_cents = 0.0;
    /// V/oct scaling error: the realized octave span as a multiple of the ideal.
    /// 1.0 tracks perfectly; 1.002 makes every octave 0.2% wide, so pitch error
    /// grows with distance from the reference.
    double scale_error = 1.0;
    /// High-end tracking compression: above `hf_knee_octaves` the converter
    /// tracks flat by this many octaves per octave of further rise (the bulk-
    /// resistance term of the exponential converter). 0 disables it.
    double hf_compression = 0.0;
    double hf_knee_octaves = 3.0;

    /// Frequency for a control voltage, in Hz. Pure; allocates nothing.
    double frequency_hz(double control_volts) const noexcept {
        double octaves = (control_volts / volts_per_octave) * scale_error
                       + tune_offset_cents / 1200.0;
        if (hf_compression > 0.0 && octaves > hf_knee_octaves)
            octaves -= hf_compression * (octaves - hf_knee_octaves);
        return reference_hz * std::exp2(octaves);
    }

    /// Per-sample phase increment (cycles) for a control voltage at `sample_rate`.
    double phase_increment(double control_volts, double sample_rate) const noexcept {
        return frequency_hz(control_volts) / sample_rate;
    }
};

/// A per-shape lumped memoryless nonlinearity — the "analog character" the output
/// stage imprints. One curve stands in for the whole waveshaper/soft-clip chain
/// of a shape (the identifiability table's "output soft clip vs shaper residual"
/// pair is observationally one nonlinearity per shape at a fixed level, so this
/// increment fits one lumped curve, not the split).
///
/// Neutral by default (`amount == 0` → identity, applied as an exact bypass).
struct WaveshaperParams {
    /// Blend from clean (0) to fully shaped (1). Exactly 0 is a bit-exact bypass.
    double amount = 0.0;
    /// Pre-gain into the saturator. Higher drive = harder curve = more harmonics.
    double drive = 1.0;
    /// Even-harmonic bias. 0 is symmetric (odd harmonics only); non-zero pulls
    /// the curve off-center and introduces even harmonics.
    double asymmetry = 0.0;
};

/// Virtual-analog VCO over the static shapes, with a circuit-flavored
/// deterministic path. `prepare(sample_rate)` before the frequency-dependent
/// stages (level-vs-pitch, AC coupling) are meaningful; the neutral defaults need
/// no sample rate.
class VcoOscillator {
public:
    void prepare(double sample_rate) noexcept {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        update_ac_pole();
    }

    void set_shape(VaShape shape) noexcept { core_.set_shape(shape); }
    VaShape shape() const noexcept { return core_.shape(); }

    void set_pulse_width(double width) noexcept { core_.set_pulse_width(width); }
    double pulse_width() const noexcept { return core_.pulse_width(); }

    // ── Integrator-leak bow ────────────────────────────────────────────────
    /// Curvature of the saw/ramp bow. 0 is a perfectly linear ramp (exact bypass);
    /// larger values bow the rising ramp harder (a leaky integrator charges fast
    /// then flattens). The reshape is a pure function of phase, so its level is
    /// pitch-independent by construction — the leaky-integrator level defect the
    /// suite guards against never appears. Applies to the saw shape only; the
    /// other shapes' character comes from stages this increment does not model.
    void set_bow(double curvature) noexcept { bow_ = curvature > 0.0 ? curvature : 0.0; }
    double bow() const noexcept { return bow_; }

    // ── Per-shape waveshaper ───────────────────────────────────────────────
    void set_waveshaper(VaShape shape, const WaveshaperParams& params) noexcept {
        shapers_[static_cast<std::size_t>(shape)] = params;
    }
    WaveshaperParams waveshaper(VaShape shape) const noexcept {
        return shapers_[static_cast<std::size_t>(shape)];
    }

    // ── Level vs pitch ─────────────────────────────────────────────────────
    /// Output level tilt, in dB per octave relative to `kLevelReferenceHz`.
    /// 0 is flat (exact bypass); negative rolls the top off, as an analog VCO's
    /// output buffer does. Independent of the core-reset sag below.
    void set_level_tilt(double db_per_octave) noexcept { level_tilt_db_per_octave_ = db_per_octave; }
    double level_tilt() const noexcept { return level_tilt_db_per_octave_; }

    /// Finite core-reset time, in seconds. The reset edge consumes a fixed slice
    /// of every period, so it eats a growing FRACTION of shorter (higher-pitch)
    /// periods — a level sag proportional to frequency. 0 disables it (exact
    /// bypass). This is the level-vs-pitch contribution the identifiability table
    /// corroborates against edge width and HF tilt; here it is the level term.
    void set_core_reset_time(double seconds) noexcept {
        core_reset_seconds_ = seconds > 0.0 ? seconds : 0.0;
    }
    double core_reset_time() const noexcept { return core_reset_seconds_; }

    // ── AC coupling (output DC blocker) ────────────────────────────────────
    /// Output DC-blocking corner, in Hz. 0 disables the stage (exact bypass, DC
    /// passes). When enabled the output is high-passed by a one-pole DC blocker
    /// whose pole is `exp(-2*pi*corner/sample_rate)`, so DC is fully removed and
    /// the -3 dB point sits at the corner. Call `prepare()` first.
    void set_ac_coupling(double corner_hz) noexcept {
        ac_corner_hz_ = corner_hz > 0.0 ? corner_hz : 0.0;
        update_ac_pole();
    }
    double ac_coupling() const noexcept { return ac_corner_hz_; }
    /// The DC blocker's realized pole for the current corner and sample rate;
    /// 1.0 when the stage is bypassed. Exposed so a test can reproduce the exact
    /// LTI response the VCO applies.
    double ac_pole() const noexcept { return ac_enabled_ ? ac_pole_ : 1.0; }

    // ── Reserved: drift / jitter ───────────────────────────────────────────
    /// Reserved pitch-noise depths. They default to 0 and are INERT: no
    /// stochastic source is wired in this increment, so setting them changes
    /// nothing. The injection point is the `increment` handed to `next()` (a
    /// source will multiply it here); the knobs exist so wiring that source later
    /// is not an API break. Separating drift from jitter needs the time-domain
    /// analyzer the suite does not have yet — until then these stay unimplemented
    /// on purpose, and `test_osc_vco.cpp` pins that they do nothing.
    void set_drift_depth(double depth) noexcept { drift_depth_ = depth; }
    double drift_depth() const noexcept { return drift_depth_; }
    void set_jitter_depth(double depth) noexcept { jitter_depth_ = depth; }
    double jitter_depth() const noexcept { return jitter_depth_; }

    /// Reset the phase (and the AC-coupling and correction state) to a known point.
    void reset(double phase = 0.0) noexcept {
        core_.reset(phase);
        dc_blocker_.reset();
    }

    double phase() const noexcept { return core_.phase(); }

    /// Generate one sample and advance by `increment` cycles.
    ///
    /// `increment` is the per-sample phase step (frequency ÷ sample rate) and is
    /// read every call, so FM and per-sample pitch modulation — and, later, a
    /// drift/jitter source at this site — compose without an API change.
    double next(double increment) noexcept {
        const double modulated = increment * pitch_noise_factor();
        double v = core_.next(modulated);
        v = apply_bow(v);
        v = apply_waveshaper(v);
        v *= level_for(modulated);
        return ac_enabled_ ? dc_blocker_.process(v) : v;
    }

    /// Reference level frequency for the tilt curve.
    static constexpr double kLevelReferenceHz = 1000.0;

private:
    /// The bow reshapes the ramp from linear to a leaky-integrator charge curve,
    /// as a memoryless map of the saw value. Exact bypass at bow 0.
    double apply_bow(double v) const noexcept {
        if (bow_ == 0.0 || core_.shape() != VaShape::saw) return v;
        // Saw value in [-1, 1] ↔ phase in [0, 1); clamp the tiny polyBLEP
        // overshoot so the charge curve stays monotone and bounded.
        double p = 0.5 * (v + 1.0);
        p = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
        // Normalized capacitor charge (1 - e^{-k p}) / (1 - e^{-k}); k → 0 is the
        // linear ramp, guarded above by the exact bypass.
        const double charged = (1.0 - std::exp(-bow_ * p)) / (1.0 - std::exp(-bow_));
        return 2.0 * charged - 1.0;
    }

    /// The active shape's lumped nonlinearity. Exact bypass at amount 0.
    double apply_waveshaper(double v) const noexcept {
        const WaveshaperParams& w = shapers_[static_cast<std::size_t>(core_.shape())];
        if (w.amount == 0.0) return v;
        // A tanh saturator biased by `asymmetry`, mapped so the full-scale ends
        // ±1 land exactly on ±1 and the curve is DC-free (its midpoint removed).
        // Anchoring on the two ends rather than a single-point norm is what keeps
        // the shaped signal inside [-1, 1] for any bias — a monotone curve pinned
        // at both endpoints cannot overshoot between them.
        const double lo = std::tanh(-w.drive + w.asymmetry);
        const double hi = std::tanh(w.drive + w.asymmetry);
        const double mid = 0.5 * (hi + lo);   // the DC the bias introduces.
        const double half = 0.5 * (hi - lo);  // > 0 for drive > 0.
        const double shaped =
            half != 0.0 ? (std::tanh(w.drive * v + w.asymmetry) - mid) / half : v;
        return (1.0 - w.amount) * v + w.amount * shaped;
    }

    /// Level-vs-pitch gain for a modulated increment. Exact bypass (1.0) when both
    /// contributors are neutral, which also skips the `log2` a zero increment
    /// would send to -inf.
    double level_for(double increment) const noexcept {
        if (level_tilt_db_per_octave_ == 0.0 && core_reset_seconds_ == 0.0) return 1.0;
        const double frequency = std::fabs(increment) * sample_rate_;
        if (!(frequency > 0.0)) return 1.0;

        double gain = 1.0;
        if (level_tilt_db_per_octave_ != 0.0) {
            const double octaves = std::log2(frequency / kLevelReferenceHz);
            gain *= std::pow(10.0, -level_tilt_db_per_octave_ * octaves / 20.0);
        }
        if (core_reset_seconds_ != 0.0) {
            const double eaten = core_reset_seconds_ * frequency; // fraction of the period.
            gain *= eaten < 1.0 ? (1.0 - eaten) : 0.0;
        }
        return gain;
    }

    /// The clock multiplier for the reserved drift/jitter site. Exactly 1.0 until
    /// a source is wired — see `set_drift_depth`.
    double pitch_noise_factor() const noexcept {
        // No stochastic source in this increment; the depths are inert. Kept as a
        // named site so the source lands here without touching `next()`.
        return 1.0;
    }

    void update_ac_pole() noexcept {
        ac_enabled_ = ac_corner_hz_ > 0.0;
        if (ac_enabled_) {
            ac_pole_ = std::exp(-2.0 * std::numbers::pi * ac_corner_hz_ / sample_rate_);
            dc_blocker_.set_pole(ac_pole_);
        } else {
            ac_pole_ = 1.0;
        }
    }

    VaOscillator core_;
    double sample_rate_ = 48000.0;

    double bow_ = 0.0;
    std::array<WaveshaperParams, 4> shapers_{};

    double level_tilt_db_per_octave_ = 0.0;
    double core_reset_seconds_ = 0.0;

    double ac_corner_hz_ = 0.0;
    double ac_pole_ = 1.0;
    bool ac_enabled_ = false;
    pulp::signal::DcBlocker<double> dc_blocker_;

    double drift_depth_ = 0.0;
    double jitter_depth_ = 0.0;
};

} // namespace pulp::signal::osc
