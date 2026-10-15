#pragma once
/// @file lfo_sampling.hpp
/// How to SAMPLE an LFO waveform without aliasing, for each kind of consumer.
///
/// An LFO is a continuous function of phase. What a plugin can do with it
/// depends on how often the destination can take a new value:
///
/// - **Once per block / per hop** -- a spectral mask, a filter restage, a
///   parameter the host reads at block rate. Reading the waveform at one
///   phase per block samples it at the block rate, and a cycle shorter than
///   two blocks aliases: a 2 kHz square sampled at 187.5 Hz is a random-
///   looking stepper, not a 2 kHz anything. The band-limited value for such a
///   consumer is the waveform's MEAN over the block's phase interval
///   (`lfo_wave_mean`): a box filter one block wide, whose first null sits at
///   the block rate, before the decimation to block rate. Below the block
///   Nyquist it is the value at the block's centre to within the box's own
///   rolloff (-0.9 dB at a quarter of the block rate); above it the swing
///   collapses to the waveform's average instead of folding. Closed form, so a
///   2 kHz LFO costs the same as a 0.2 Hz one.
///
/// - **Every sample** -- a gain stage, a per-sample smoother, AM. Here the
///   waveform itself must be band-limited at audio rates: a trivial square or
///   saw folds its harmonics above Nyquist straight into the band. The
///   per-sample value with the polyBLEP (step) and polyBLAMP (slope)
///   corrections of `osc/blep.hpp` is `lfo_wave_bandlimited`; a sine needs
///   none. At sub-audio rates the correction only touches the two samples
///   straddling each corner, so a slow LFO costs two comparisons a sample and
///   reads exactly as the trivial waveform elsewhere.
///
/// - **For display** -- the trivial point value, `lfo_wave`.
///
/// `lfo_wave_integral` is the cumulative integral the mean is built from, so a
/// consumer with its own interval bookkeeping can take differences itself.
///
/// Waveforms are the effect vocabulary `LfoWave`. A square's pulse width and a
/// triangle's peak position are parameters; the triangle starts at -1, peaks at
/// +1 at `triangle_peak`, and returns to -1 at the cycle's end (the toolkit
/// `LfoT` law; the effect lane's triangle is this one a quarter cycle later).
/// The two random waves are not functions of phase: they belong to `LfoT`'s
/// own state, and every function here returns 0 for them.
///
/// RT contract: pure functions, no allocation, no state.
#include <pulp/signal/osc/blep.hpp>

#include <cmath>
#include <cstdint>

namespace pulp::signal {

/// The effect-wave vocabulary, shared by these functions and by `EffectLfoT`
/// (`lfo.hpp`), whose `set_wave(LfoWave)` overload selects the effect
/// convention. Distinct on purpose from the toolkit `LfoT::Wave`.
enum class LfoWave : std::uint8_t {
    sine,
    triangle,
    saw_up,
    saw_down,
    square,
    sample_hold,
    smooth_random,
};

/// The shape parameters the deterministic waves take.
struct LfoShapeParams {
    double pulse_width = 0.5;    ///< Square: fraction of the cycle spent at +1, in (0, 1).
    double triangle_peak = 0.5;  ///< Triangle: phase of the +1 apex, in (0, 1).
};

/// True for the waves that are periodic functions of phase (everything but
/// the two random waves).
[[nodiscard]] constexpr bool lfo_wave_is_periodic(LfoWave wave) noexcept {
    return wave != LfoWave::sample_hold && wave != LfoWave::smooth_random;
}

namespace detail {

constexpr double kTwoPi = 6.283185307179586477;

[[nodiscard]] inline double wrap01(double x) noexcept { return x - std::floor(x); }

[[nodiscard]] inline double clamp_open(double x) noexcept {
    return x < 1.0e-6 ? 1.0e-6 : (x > 1.0 - 1.0e-6 ? 1.0 - 1.0e-6 : x);
}

/// The integral of one wave over [0, x] for x in [0, 1].
[[nodiscard]] inline double wave_integral_in_cycle(LfoWave wave, double x,
                                                   const LfoShapeParams& params) noexcept {
    switch (wave) {
        case LfoWave::sine: return (1.0 - std::cos(kTwoPi * x)) / kTwoPi;
        case LfoWave::triangle: {
            const double a = clamp_open(params.triangle_peak);
            if (x < a) return x * x / a - x;
            const double r = x - a;
            return r - r * r / (1.0 - a);
        }
        case LfoWave::saw_up: return x * x - x;
        case LfoWave::saw_down: return x - x * x;
        case LfoWave::square: {
            const double pw = clamp_open(params.pulse_width);
            return x < pw ? x : 2.0 * pw - x;
        }
        case LfoWave::sample_hold:
        case LfoWave::smooth_random:
        default: return 0.0;
    }
}

/// The integral over one whole cycle: zero for every wave but the square.
[[nodiscard]] inline double wave_integral_per_cycle(LfoWave wave,
                                                    const LfoShapeParams& params) noexcept {
    return wave == LfoWave::square ? 2.0 * clamp_open(params.pulse_width) - 1.0 : 0.0;
}

}  // namespace detail

/// The trivial value of @p wave at @p phase (any real number of cycles), in
/// [-1, 1]. What a display draws, and the value every other function here
/// agrees with away from a corner and in the limit of a vanishing interval.
[[nodiscard]] inline double lfo_wave(LfoWave wave, double phase,
                                     const LfoShapeParams& params = {}) noexcept {
    const double p = detail::wrap01(phase);
    switch (wave) {
        case LfoWave::sine: return std::sin(detail::kTwoPi * p);
        case LfoWave::triangle: {
            const double a = detail::clamp_open(params.triangle_peak);
            return p < a ? -1.0 + 2.0 * p / a : 1.0 - 2.0 * (p - a) / (1.0 - a);
        }
        case LfoWave::saw_up: return 2.0 * p - 1.0;
        case LfoWave::saw_down: return 1.0 - 2.0 * p;
        case LfoWave::square: return p < detail::clamp_open(params.pulse_width) ? 1.0 : -1.0;
        case LfoWave::sample_hold:
        case LfoWave::smooth_random:
        default: return 0.0;
    }
}

/// The cumulative integral of @p wave from phase 0 to @p phase, in value x
/// cycles. Continuous in @p phase, defined for any real phase, so the mean of
/// an interval is `(integral(to) - integral(from)) / (to - from)`.
[[nodiscard]] inline double lfo_wave_integral(LfoWave wave, double phase,
                                              const LfoShapeParams& params = {}) noexcept {
    const double whole = std::floor(phase);
    return whole * detail::wave_integral_per_cycle(wave, params)
        + detail::wave_integral_in_cycle(wave, phase - whole, params);
}

/// The exact mean of @p wave over the phase interval [@p from, @p to], in any
/// units of whole cycles and of any length: the band-limited value for a
/// consumer that takes one value per interval. A zero-length interval is the
/// point value at @p from, so a consumer may shrink its interval to nothing
/// and read the trivial waveform; `to < from` is treated as the interval the
/// other way round.
[[nodiscard]] inline double lfo_wave_mean(LfoWave wave, double from, double to,
                                          const LfoShapeParams& params = {}) noexcept {
    if (!(std::isfinite(from) && std::isfinite(to))) return 0.0;
    if (to < from) {
        const double t = from;
        from = to;
        to = t;
    }
    const double span = to - from;
    // Below this the difference of two integrals loses more digits than the
    // point value is wrong by.
    if (span < 1.0e-9) return lfo_wave(wave, from, params);
    return (lfo_wave_integral(wave, to, params) - lfo_wave_integral(wave, from, params)) / span;
}

/// The band-limited value of @p wave for a consumer that reads it EVERY
/// sample, at @p phase with a per-sample phase step of @p increment (cycles).
///
/// The trivial waveform plus the polyBLEP correction for each step in its
/// value (the saw's wrap, the square's two edges) and the polyBLAMP correction
/// for each break in its slope (the triangle's apex and trough). Each
/// correction reaches the sample before a corner and the sample after it,
/// which is why this is stateless: both taps are placed from the phase's
/// distance to the corner. The kernels are `osc::poly_blep` /
/// `osc::poly_blamp`, with their documented reach -- tens of dB over the
/// trivial waveform, not a tabulated kernel's hundred.
///
/// A non-positive or non-finite @p increment (a stopped LFO) reads as the
/// trivial waveform: nothing is moving, so nothing is discontinuous.
[[nodiscard]] inline double lfo_wave_bandlimited(LfoWave wave, double phase, double increment,
                                                 const LfoShapeParams& params = {}) noexcept {
    const double p = detail::wrap01(phase);
    double value = lfo_wave(wave, p, params);
    if (!(increment > 0.0) || !std::isfinite(increment) || increment >= 1.0) return value;
    const double dt = increment;

    // The correction a corner at phase `at` contributes to the sample at `p`:
    // `after` when the corner lies within one step before p, `before` when it
    // lies within one step after p. `height` is a value jump (BLEP) or a
    // slope change per sample (BLAMP).
    const auto corner = [&](double at, double height, bool slope) {
        // Distance from the corner forward to this sample, in cycles, wrapped
        // so a corner at 0 is seen from both ends of the cycle.
        double behind = p - at;
        behind -= std::floor(behind);
        if (behind < dt) {
            // The corner is `behind` cycles before this sample: this is the
            // sample after it, and d (the corner's position past the previous
            // sample) is 1 - behind / dt.
            const double d = 1.0 - behind / dt;
            value += slope ? osc::poly_blamp(d, height).after : osc::poly_blep(d, height).after;
        }
        double ahead = at - p;
        ahead -= std::floor(ahead);
        if (ahead > 0.0 && ahead <= dt) {
            // The corner is `ahead` cycles after this sample: this is the
            // sample before it, with the corner `ahead / dt` of a step past it.
            const double d = ahead / dt;
            value += slope ? osc::poly_blamp(d, height).before : osc::poly_blep(d, height).before;
        }
    };

    switch (wave) {
        case LfoWave::saw_up: corner(0.0, -2.0, false); break;      // +1 wraps to -1
        case LfoWave::saw_down: corner(0.0, +2.0, false); break;    // -1 wraps to +1
        case LfoWave::square: {
            const double pw = detail::clamp_open(params.pulse_width);
            corner(0.0, +2.0, false);
            corner(pw, -2.0, false);
            break;
        }
        case LfoWave::triangle: {
            // Slopes per cycle: +2/a up to the apex, -2/(1-a) after it.
            const double a = detail::clamp_open(params.triangle_peak);
            const double turn = (2.0 / a + 2.0 / (1.0 - a)) * dt;  // per sample
            corner(a, -turn, true);
            corner(0.0, +turn, true);
            break;
        }
        case LfoWave::sine:
        case LfoWave::sample_hold:
        case LfoWave::smooth_random:
        default: break;
    }
    return value;
}

}  // namespace pulp::signal
