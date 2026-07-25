#pragma once

/// @file lfo.hpp
/// The one low-frequency modulation source the catalog modulates with.
///
/// Chorus voices, flanger sweeps, phaser sweeps, Leslie rotors, tremolo,
/// scanner vibrato, auto-pan — every one of them is "a periodic shape at a
/// stated rate, at a stated phase offset". Written separately they each drift
/// into their own conventions: one counts phase in radians and one in cycles,
/// one calls the triangle's peak +1 and another calls it 1, one resets phase on
/// a rate change and another does not. `LfoT` fixes all of that once, so a
/// spec that says "voice k of N sits at phase k/N" means the same thing in
/// every module that reads it.
///
/// The contract, in the form specs cite it:
///
///   - **Output is bipolar `[-1, +1]`** for every shape. A module that wants
///     unipolar `[0, 1]` asks for `next_unipolar()`; it does not scale by hand.
///   - **Phase is in CYCLES**, `[0, 1)`. `set_phase_offset(0.25)` is a quarter
///     cycle; `set_phase_offset(0.5)` is exact inversion for any shape that is
///     odd-symmetric about its half-cycle (triangle, sine, saw).
///   - **Rate accuracy is exact to the phase accumulator**, which counts in
///     `double`. Zero-crossing count over 100 s matches the configured rate to
///     far better than the ±0.01 % the chorus/flanger specs assert.
///   - **`next_quadrature()` computes `sin` and `cos` of the SAME accumulated
///     phase**, not a recursive resonator. The exactness matters: a resonator's
///     slow amplitude and phase drift is harmless in a tremolo and fatal in a
///     frequency shifter's carrier, where it wrecks image rejection. One
///     discipline, no per-caller decision.
///   - **Randomness is seeded.** The two stochastic shapes (`sample_hold`,
///     `smooth_random`) draw from `Xorshift32`, rewound by `reset()`, per
///     series law 2. The seed is a construction choice, never an automatable
///     parameter.
///
/// The phase itself is `osc::PhaseAccumulator` — the house wrap law, exact for
/// negative and greater-than-one increments alike. An LFO never needs those,
/// but sharing the accumulator means there is exactly one place in the library
/// where "what happens at the wrap" is decided.
///
/// RT contract: `prepare()` recomputes one increment and allocates nothing.
/// `set_*`, `next*`, and `reset()` allocate nothing, take no locks, and perform
/// no I/O; all are safe per sample on the audio thread. `next()` costs one
/// accumulator advance plus one shape evaluation — a `sin` call for
/// `Wave::sine`, arithmetic only for the rest.

#include <pulp/signal/osc/phase.hpp>
#include <pulp/signal/rng.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

/// The shapes an `LfoT` can produce. All are bipolar `[-1, +1]` and all start
/// at phase 0 when `reset()` is called.
enum class LfoWave {
    sine,           ///< `sin(2π·φ)`. Smooth; the default for anything summed.
    triangle,       ///< Linear rise and fall, peaks at φ = 0.25 / 0.75.
    saw_up,         ///< Rises −1 → +1 across the cycle, steps back at the wrap.
    saw_down,       ///< Falls +1 → −1 across the cycle.
    square,         ///< +1 for the first half cycle, −1 for the second.
    sample_hold,    ///< A new seeded random value latched at each wrap.
    smooth_random,  ///< `sample_hold` interpolated smoothly between draws.
};

/// A low-frequency oscillator. Bipolar output, phase in cycles, exact rate.
template <typename SampleType = float>
class LfoT {
public:
    using Wave = LfoWave;

    /// Rate ceiling. An "LFO" above this is an audio oscillator and should be
    /// one — the shapes here are not band-limited, so running them into the
    /// audible range aliases. Modules that genuinely want an audio-rate
    /// modulator use the `osc` family instead.
    /// [design parameter] default 200 Hz, range 20 .. 1000 Hz.
    static constexpr double kMaxRateHz = 200.0;

    /// A default-constructed LFO is a 1 Hz sine at 44.1 kHz — usable without a
    /// setup call, so a caller that only sets a depth still modulates.
    LfoT() {
        update();
        // Draws the first random-shape pair too, so `sample_hold` on a
        // never-reset LFO still modulates instead of sitting at zero.
        reset();
    }

    /// Recomputes the phase increment for a sample rate. Does not reset phase:
    /// a sample-rate change mid-render should not restart the sweep.
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        update();
    }

    /// Rate in Hz. Clamped to `(0, kMaxRateHz]`.
    void set_rate_hz(double hz) {
        rate_hz_ = std::clamp(hz, 0.0, kMaxRateHz);
        update();
    }

    double rate_hz() const { return rate_hz_; }

    void set_wave(Wave wave) { wave_ = wave; }
    Wave wave() const { return wave_; }

    /// Phase offset in **cycles**, wrapped into `[0, 1)`. This is the N-voice
    /// spacing control: voice `k` of `N` sits at `set_phase_offset(k / N)`.
    /// Changing it does not move the accumulator, so an offset change shifts
    /// the output immediately without a discontinuity in the underlying phase.
    void set_phase_offset(double cycles) { offset_ = wrap_cycles(cycles); }

    double phase_offset() const { return offset_; }

    /// The stereo special case, named because it is what most callers want:
    /// `set_stereo_offset(0.5)` puts this LFO in anti-phase with an
    /// otherwise-identical one. Identical to `set_phase_offset`, kept as a
    /// distinct spelling so a call site reads as intent.
    void set_stereo_offset(double cycles) { set_phase_offset(cycles); }

    /// Seeds the stochastic shapes. Construction/preset choice only — never an
    /// automatable parameter (series law 2).
    void set_seed(std::uint32_t seed) {
        rng_.set_seed(seed);
        reset();
    }

    /// Rewinds phase to 0, rewinds the generator, and redraws the random-shape
    /// state so a render from `reset()` is bit-identical every time.
    void reset() {
        phase_.reset(0.0);
        rng_.reset();
        previous_phi_ = wrap_cycles(offset_);
        random_previous_ = rng_.next_bipolar<double>();
        random_current_ = rng_.next_bipolar<double>();
    }

    /// Current phase in cycles, INCLUDING the offset. `[0, 1)`.
    double phase() const { return wrap_cycles(phase_.phase() + offset_); }

    /// Advances one sample and returns the shape value in `[-1, +1]`.
    SampleType next() {
        phase_.advance(increment_);
        const double phi = wrap_cycles(phase_.phase() + offset_);
        // A wrap is what re-draws the random shapes. Detected from the
        // OFFSET-INCLUDED phase going backwards, not the raw accumulator: the
        // shape a caller hears is the offset one, so a draw keyed to the raw
        // wrap would land mid-shape and step. Comparing phases rather than
        // reading the accumulator's event span also keeps this O(1) — the LFO
        // increment is always in [0, 1), so it wraps at most once per sample.
        if (phi < previous_phi_) {
            random_previous_ = random_current_;
            random_current_ = rng_.next_bipolar<double>();
        }
        previous_phi_ = phi;
        return static_cast<SampleType>(shape(phi));
    }

    /// Advances one sample and returns the shape mapped to `[0, 1]` — for
    /// depths and gains, where a bipolar value would have to be rescaled at
    /// every call site.
    SampleType next_unipolar() {
        return static_cast<SampleType>(0.5 * (static_cast<double>(next()) + 1.0));
    }

    /// One sample of an exact quadrature pair: `sin` and `cos` of the same
    /// accumulated phase. Ignores `wave()` — quadrature is only meaningful for
    /// a sinusoid — and never uses a recursive resonator, whose amplitude and
    /// phase drift would destroy the image rejection of anything that mixes
    /// with it.
    void next_quadrature(SampleType& sine_out, SampleType& cosine_out) {
        phase_.advance(increment_);
        const double radians = kTwoPi * wrap_cycles(phase_.phase() + offset_);
        sine_out = static_cast<SampleType>(std::sin(radians));
        cosine_out = static_cast<SampleType>(std::cos(radians));
    }

private:
    static constexpr double kTwoPi = 6.283185307179586476925286766559;

    /// Smoothing applied between `smooth_random` draws. Smoothstep rather than
    /// a linear ramp so the modulation has no corner at the draw instant —
    /// a corner is audible as a tick when the target is a filter cutoff.
    static double smoothstep(double t) { return t * t * (3.0 - 2.0 * t); }

    static double wrap_cycles(double c) {
        c -= std::floor(c);
        // `floor` of a value just below an integer can round up to it in the
        // subtraction, leaving exactly 1.0; fold that back so the domain stays
        // half-open and `shape()` never sees φ = 1.
        return c < 1.0 ? c : 0.0;
    }

    double shape(double phi) const {
        switch (wave_) {
            case Wave::sine:
                return std::sin(kTwoPi * phi);
            case Wave::triangle:
                // Odd-symmetric about the half cycle, exactly like the sine it
                // stands in for: 0 at φ = 0, +1 at 0.25, 0 at 0.5, −1 at 0.75.
                // That symmetry is what makes `set_stereo_offset(0.5)` an exact
                // inversion, which the chorus voicings assert directly.
                if (phi < 0.25) return 4.0 * phi;
                if (phi < 0.75) return 2.0 - 4.0 * phi;
                return 4.0 * phi - 4.0;
            case Wave::saw_up:
                return 2.0 * phi - 1.0;
            case Wave::saw_down:
                return 1.0 - 2.0 * phi;
            case Wave::square:
                return phi < 0.5 ? 1.0 : -1.0;
            case Wave::sample_hold:
                return random_current_;
            case Wave::smooth_random:
                return random_previous_ +
                       (random_current_ - random_previous_) * smoothstep(phi);
        }
        return 0.0;
    }

    void update() {
        increment_ = sample_rate_ > 0.0 ? rate_hz_ / sample_rate_ : 0.0;
    }

    double sample_rate_ = 44100.0;
    double rate_hz_ = 1.0;
    double increment_ = 0.0;
    double offset_ = 0.0;
    Wave wave_ = Wave::sine;

    osc::PhaseAccumulator phase_{};
    Xorshift32 rng_{0x2545F491u};
    double previous_phi_ = 0.0;
    double random_previous_ = 0.0;
    double random_current_ = 0.0;
};

using Lfo = LfoT<float>;
using Lfo64 = LfoT<double>;

}  // namespace pulp::signal
