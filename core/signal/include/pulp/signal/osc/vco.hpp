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
/// It also carries the VCO's two stochastic pitch sources — **drift** and
/// **jitter** — as a seeded frequency modulation of the per-sample `increment`
/// before it reaches the bandlimited core. They are two distinct processes:
///
///   * **Drift** is a slow, bandlimited random walk of the pitch: white noise
///     colored by a one-pole (leaky-integrator / Ornstein–Uhlenbeck) filter
///     whose corner (`set_drift_rate_hz`, default `kDefaultDriftRateHz`) sets a
///     wander that evolves over hundreds of milliseconds, not per sample. Its
///     stationary output has unit variance, so `drift_depth` reads directly as
///     the RMS pitch excursion **in cents**.
///   * **Jitter** is fast, near-white cycle-to-cycle frequency noise:
///     independent per sample, so `jitter_depth` is the RMS per-sample frequency
///     deviation **in cents**. In the time domain the two separate by their
///     autocorrelation (drift is coherent frame-to-frame, jitter is not) — the
///     Allan-slope regime split (drift ∝ τ^{+1/2}, jitter ∝ τ^{-1/2}) that names
///     them rigorously is a later offline (Python Quality-Lab) increment, not
///     computed here.
///
/// Both are converted from cents to a frequency multiplier by `2^(cents/1200)`
/// and injected at ONE site, `pitch_noise_factor()`, which multiplies the
/// increment. **When both depths are 0 that factor is exactly 1.0 (an early-out
/// that advances no noise state), so a default-constructed OSC-VCO stays bit-for-
/// bit a `VaOscillator`** (see the master null in `test_osc_vco.cpp`). The source
/// is deterministic and seed-reproducible (`set_seed`; no `random_device`, no
/// clock) — the same seed and inputs yield bit-identical output, which the suite
/// gates. The offline fitting tool and data-driven profiles remain out of scope:
/// this is the ENGINE that has the parameters, hand-set, not the estimator that
/// recovers them.
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
#include <algorithm>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
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
    /// Seed the drift filter coefficients for the default sample rate so a
    /// default-constructed oscillator is valid before `prepare()`; `prepare()`
    /// recomputes them for the real rate.
    VcoOscillator() noexcept { update_drift_coeffs(); }

    void prepare(double sample_rate) noexcept {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        update_ac_pole();
        update_drift_coeffs();
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
        // `amount` is a blend, so it is clamped to [0, 1] like every other
        // unit-range control here: outside that range it extrapolates the blend
        // (amount 2 reaches 3.0 on a full-scale shape) and breaks the [-1, 1]
        // output contract the boundedness gate relies on.
        WaveshaperParams p = params;
        p.amount = std::clamp(p.amount, 0.0, 1.0);
        shapers_[static_cast<std::size_t>(shape)] = p;
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

    // ── Drift / jitter (seeded pitch noise) ─────────────────────────────────
    /// RMS of the slow drift wander, in cents. 0 disables drift (its stream does
    /// not advance). The drift is a one-pole-colored random walk whose stationary
    /// output has unit variance, so this depth is the RMS pitch excursion the
    /// wander reaches; its timescale is `set_drift_rate_hz`. Off by default, so a
    /// neutral VCO is unaffected.
    void set_drift_depth(double depth_cents) noexcept { drift_depth_ = depth_cents; }
    double drift_depth() const noexcept { return drift_depth_; }

    /// Corner of the drift coloring filter, in Hz. Lower is a slower wander; the
    /// default puts the wander in the hundreds-of-milliseconds range. Recomputes
    /// the filter pole against the current sample rate.
    void set_drift_rate_hz(double corner_hz) noexcept {
        drift_rate_hz_ = corner_hz > 0.0 ? corner_hz : 0.0;
        update_drift_coeffs();
    }
    double drift_rate_hz() const noexcept { return drift_rate_hz_; }

    /// RMS of the fast, near-white cycle-to-cycle jitter, in cents per sample. 0
    /// disables jitter (its stream does not advance). Independent per sample, so
    /// this is the RMS of the per-sample frequency deviation. Off by default.
    void set_jitter_depth(double depth_cents) noexcept { jitter_depth_ = depth_cents; }
    double jitter_depth() const noexcept { return jitter_depth_; }

    /// Seed for both noise streams. Determinism is a contract: the same seed and
    /// inputs reproduce the output bit-for-bit. Takes effect on the streams
    /// immediately and is re-applied by `reset()`, so a seeded render starts from
    /// a known point.
    void set_seed(std::uint64_t seed) noexcept {
        seed_ = seed;
        seed_streams();
    }
    std::uint64_t seed() const noexcept { return seed_; }

    /// Default drift corner (Hz): a wander whose correlation time is a few hundred
    /// milliseconds — slow relative to any audio-rate structure.
    static constexpr double kDefaultDriftRateHz = 0.4;

    /// Reset the phase (and the AC-coupling and correction state) to a known
    /// point, and restart the noise streams from `seed()` so a reset render
    /// reproduces bit-for-bit.
    void reset(double phase = 0.0) noexcept {
        core_.reset(phase);
        dc_blocker_.reset();
        drift_state_ = 0.0;
        seed_streams();
    }

    double phase() const noexcept { return core_.phase(); }

    /// Generate one sample and advance by `increment` cycles.
    ///
    /// `increment` is the per-sample phase step (frequency ÷ sample rate) and is
    /// read every call, so FM and per-sample pitch modulation — and the
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

    /// The frequency multiplier the drift and jitter sources apply to the
    /// increment this sample.
    ///
    /// Both depths neutral is an exact bypass: it returns 1.0 bit-for-bit and
    /// advances no noise state, so a neutral VCO is unperturbed and the master
    /// null holds. Otherwise each active source contributes a cents deviation —
    /// drift from the one-pole-colored (unit-variance) random walk, jitter from
    /// an independent per-sample normal — summed and mapped to a ratio by
    /// `2^(cents/1200)`. `exp2` is always positive, so the increment keeps its
    /// sign (noise never reverses the phase) and stays finite.
    double pitch_noise_factor() noexcept {
        if (drift_depth_ == 0.0 && jitter_depth_ == 0.0) return 1.0;

        double cents = 0.0;
        if (drift_depth_ != 0.0) {
            drift_state_ = drift_pole_ * drift_state_ + drift_norm_ * drift_rng_.next_gaussian();
            cents += drift_depth_ * drift_state_;
        }
        if (jitter_depth_ != 0.0)
            cents += jitter_depth_ * jitter_rng_.next_gaussian();

        return std::exp2(cents / 1200.0);
    }

    /// A deterministic, seed-reproducible white-Gaussian source. splitmix64 for
    /// the uniform stream (no `random_device`, no clock — determinism is the
    /// contract), Box–Muller for the normal variate. Pure integer plus libcall
    /// arithmetic: no allocation, no lock, no I/O, so it is safe on the audio
    /// thread, exactly like the `sin`/`exp`/`tanh` the deterministic stages call.
    struct NoiseSource {
        std::uint64_t state = 0;

        void seed(std::uint64_t s) noexcept { state = s; }

        std::uint64_t next_u64() noexcept {
            std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }

        /// A double in (0, 1): the top 53 bits, nudged off exact 0 so the log in
        /// the normal transform stays finite.
        double next_uniform() noexcept {
            const double u =
                static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
            return u > 0.0 ? u : std::numeric_limits<double>::min();
        }

        double next_gaussian() noexcept {
            const double u1 = next_uniform();
            const double u2 = next_uniform();
            return std::sqrt(-2.0 * std::log(u1)) *
                   std::cos(2.0 * std::numbers::pi * u2);
        }
    };

    /// Reseed both streams from `seed_`. The jitter stream is offset by a fixed
    /// constant so drift and jitter are independent sequences from one seed.
    void seed_streams() noexcept {
        drift_rng_.seed(seed_);
        jitter_rng_.seed(seed_ ^ 0xD1B54A32D192ED03ull);
    }

    /// One-pole (Ornstein–Uhlenbeck) coloring coefficients for the drift wander:
    /// the pole sets the timescale, and the input gain is chosen so the stationary
    /// output has unit variance — the variance of `a·x + b·w` (w unit-variance) is
    /// `b^2 / (1 - a^2)`, so `b = sqrt(1 - a^2)` gives variance 1. `drift_depth`
    /// then scales that unit wander straight to cents RMS.
    void update_drift_coeffs() noexcept {
        // Lower rate is a slower wander, so the pole rises toward 1 as the rate
        // falls; the limit as rate -> 0 is a frozen walk (pole 1), which the
        // unit-variance gain below turns into no wander at all. A non-positive
        // rate takes that limit rather than the opposite extreme: mapping it to
        // pole 0 would make drift full-depth per-sample white noise -- the
        // FASTEST wander -- so a caller passing 0 to mean "off" or "very slow"
        // would get audible hiss-like FM instead.
        drift_pole_ = drift_rate_hz_ > 0.0
                          ? std::exp(-2.0 * std::numbers::pi * drift_rate_hz_ / sample_rate_)
                          : 1.0;
        drift_norm_ = std::sqrt(1.0 - drift_pole_ * drift_pole_);
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
    double drift_rate_hz_ = kDefaultDriftRateHz;
    double drift_pole_ = 0.0;
    double drift_norm_ = 1.0;
    double drift_state_ = 0.0;
    std::uint64_t seed_ = 0x0123456789ABCDEFull;
    NoiseSource drift_rng_{seed_};
    NoiseSource jitter_rng_{seed_ ^ 0xD1B54A32D192ED03ull};
};

} // namespace pulp::signal::osc
