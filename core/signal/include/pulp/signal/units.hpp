#pragma once

/// @file units.hpp
/// The real-unit vocabulary shared by every DSP block in the catalog.
///
/// Series law 3 says parameters are expressed in real units — Hz, ms, s, dB,
/// semitones, cents — because those are what a musician reads on a knob and
/// what a spec can state a range in. That law is only enforceable if there is
/// exactly ONE conversion from each real unit into the internal quantity a
/// filter or oscillator actually wants. Two hand-rolled `pow(10, db/20)` call
/// sites are two chances to disagree about the dB floor; two hand-rolled
/// one-pole coefficients are two chances to disagree about whether "20 ms"
/// means a time constant or a settling time. Everything converts here.
///
/// Conventions this header fixes, once, for the whole library:
///
///   - **dB is amplitude dB** (`20·log10`), never power dB. Converting a
///     non-positive amplitude to dB floors at `kDbFloorAmplitude` rather than
///     returning −inf, so a silent buffer produces a finite, plottable number.
///   - **MIDI note 69 is 440 Hz.** Concert pitch is not a parameter here; a
///     block that needs a different A4 scales the result.
///   - **A one-pole "coefficient" is the `a` in `y += a·(x − y)`**, derived
///     from a TIME CONSTANT τ (the 63.2 % point), not from a settling time.
///     The other two common readings are one multiply away and documented on
///     `ms_to_onepole_coef` so nobody has to rediscover them:
///     `t60 = τ·ln(1000) ≈ 6.908·τ` and `t90 − t10 = τ·ln 9 ≈ 2.197·τ`.
///   - **Pitch CV is the 1 V/octave standard** — 12 semitones per volt, 100
///     cents per semitone — so any block's pitch output feeds any other's
///     pitch input without unit negotiation.
///
/// Everything is a template over the sample type so a `double` block does not
/// silently round its coefficients through `float`.
///
/// RT contract: every function here is a pure, stateless, branch-light scalar
/// computation. Nothing allocates, locks, or performs I/O, so all of it is
/// safe to call per sample on the audio thread. Several use `std::pow` /
/// `std::log2` / `std::exp`, which are not free — the conversions that appear
/// in a hot path (`db_to_linear`, `semitones_to_ratio`) are cheap enough to
/// use per sample, but a block that converts a control value that only changes
/// per block should convert per block.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::signal::units {

/// Amplitude floor used when converting to dB. Guards `log10(0)`; any value far
/// below the quietest amplitude anyone measures works.
/// [design parameter] default 1e-10 (−200 dBFS), range 1e-15 .. 1e-6.
inline constexpr double kDbFloorAmplitude = 1e-10;

/// MIDI note number of concert A. Fixed by the MIDI specification.
inline constexpr double kMidiA4 = 69.0;

/// Frequency of concert A in Hz — the tuning reference for `midi_to_hz`.
inline constexpr double kA4Hz = 440.0;

/// Semitones per octave, and cents per semitone. Fixed by 12-TET notation.
inline constexpr double kSemitonesPerOctave = 12.0;
inline constexpr double kCentsPerSemitone = 100.0;

/// Semitones per volt on the 1 V/octave pitch-CV standard.
inline constexpr double kSemitonesPerVolt = kSemitonesPerOctave;

// ── Amplitude ↔ decibels ──────────────────────────────────────────────────

/// Amplitude dB → linear gain. `0 dB → 1`, `−6.0206 dB → 0.5`.
template <typename T>
inline T db_to_linear(T db) {
    return std::pow(T{10}, db / T{20});
}

/// Linear gain → amplitude dB, floored at `kDbFloorAmplitude` so silence maps
/// to a finite −200 dB rather than −inf. Takes the magnitude, so a negative
/// gain reports its level rather than NaN.
template <typename T>
inline T linear_to_db(T linear) {
    return T{20} * std::log10(std::max(std::abs(linear), static_cast<T>(kDbFloorAmplitude)));
}

// ── Pitch ─────────────────────────────────────────────────────────────────

/// MIDI note number → Hz (note 69 = 440 Hz). Fractional notes are meaningful:
/// 69.5 is a quarter-tone above A4.
template <typename T>
inline T midi_to_hz(T note) {
    return static_cast<T>(kA4Hz) *
           std::pow(T{2}, (note - static_cast<T>(kMidiA4)) / static_cast<T>(kSemitonesPerOctave));
}

/// Hz → MIDI note number. Non-positive input would be −inf, so it is floored
/// at the dB amplitude floor's frequency analogue: the result saturates rather
/// than returning −inf, keeping a tracker's output finite on silence.
template <typename T>
inline T hz_to_midi(T hz) {
    const T safe = std::max(hz, static_cast<T>(kDbFloorAmplitude));
    return static_cast<T>(kMidiA4) +
           static_cast<T>(kSemitonesPerOctave) * std::log2(safe / static_cast<T>(kA4Hz));
}

/// Semitones → frequency ratio. `+12 → 2`, `−12 → 0.5`.
template <typename T>
inline T semitones_to_ratio(T semitones) {
    return std::pow(T{2}, semitones / static_cast<T>(kSemitonesPerOctave));
}

/// Frequency ratio → semitones. Inverse of `semitones_to_ratio`.
template <typename T>
inline T ratio_to_semitones(T ratio) {
    return static_cast<T>(kSemitonesPerOctave) *
           std::log2(std::max(ratio, static_cast<T>(kDbFloorAmplitude)));
}

/// Cents → frequency ratio. `+100 → 2^(1/12)`, `+1200 → 2`.
template <typename T>
inline T cents_to_ratio(T cents) {
    return std::pow(T{2}, cents / (static_cast<T>(kSemitonesPerOctave) *
                                   static_cast<T>(kCentsPerSemitone)));
}

/// Frequency ratio → cents. Inverse of `cents_to_ratio`.
template <typename T>
inline T ratio_to_cents(T ratio) {
    return ratio_to_semitones(ratio) * static_cast<T>(kCentsPerSemitone);
}

/// Pitch CV volts → semitones, on the 1 V/octave standard.
template <typename T>
inline T volts_to_semitones(T volts) {
    return volts * static_cast<T>(kSemitonesPerVolt);
}

/// Semitones → pitch CV volts, on the 1 V/octave standard.
template <typename T>
inline T semitones_to_volts(T semitones) {
    return semitones / static_cast<T>(kSemitonesPerVolt);
}

// ── Time ↔ samples ────────────────────────────────────────────────────────

/// Milliseconds → samples at `sample_rate` Hz. Fractional — callers that need
/// an integer delay decide their own rounding, because a delay line that
/// interpolates and one that does not want different answers.
template <typename T>
inline T ms_to_samples(T ms, T sample_rate) {
    return ms * sample_rate / T{1000};
}

/// Samples → milliseconds at `sample_rate` Hz.
template <typename T>
inline T samples_to_ms(T samples, T sample_rate) {
    return sample_rate > T{0} ? samples * T{1000} / sample_rate : T{0};
}

/// Frequency → its period in samples. A 100 Hz tone at 48 kHz is 480 samples.
template <typename T>
inline T hz_to_period_samples(T hz, T sample_rate) {
    return hz > T{0} ? sample_rate / hz : T{0};
}

/// A period in samples → its frequency in Hz.
template <typename T>
inline T period_samples_to_hz(T samples, T sample_rate) {
    return samples > T{0} ? sample_rate / samples : T{0};
}

// ── Frequency → internal filter/oscillator quantities ─────────────────────

/// Hz → normalised frequency in **cycles per sample** (`f / fs`) — the unit a
/// phase accumulator increments by.
template <typename T>
inline T hz_to_cycles_per_sample(T hz, T sample_rate) {
    return sample_rate > T{0} ? hz / sample_rate : T{0};
}

/// Hz → **radians per sample** (`2π·f / fs`) — the unit a `sin`/`cos`
/// oscillator and most filter design formulas want.
template <typename T>
inline T hz_to_radians_per_sample(T hz, T sample_rate) {
    constexpr T two_pi = static_cast<T>(6.283185307179586476925286766559L);
    return sample_rate > T{0} ? two_pi * hz / sample_rate : T{0};
}

// ── One-pole coefficients ─────────────────────────────────────────────────

/// A time constant in ms → the `a` in the one-pole smoother `y += a·(x − y)`,
/// i.e. `a = 1 − exp(−1 / (τ·fs))` with τ in seconds. `a ∈ (0, 1]`.
///
/// τ is the **63.2 % point** — the time for a step response to cover
/// `1 − 1/e` of the distance. The other two readings people mean by "the
/// smoothing time" are one multiply away, so state which one you mean and
/// convert:
///
///   - 60 dB decay: `t60 = τ·ln(1000) ≈ 6.908·τ`
///   - 10–90 % rise: `t90 − t10 = τ·ln 9 ≈ 2.197·τ`
///
/// `ms <= 0` returns 1 (instant), which is what a caller asking for zero
/// smoothing means; it never divides by zero.
template <typename T>
inline T ms_to_onepole_coef(T ms, T sample_rate) {
    if (!(ms > T{0}) || !(sample_rate > T{0})) return T{1};
    const T samples = ms * sample_rate / T{1000};
    return T{1} - std::exp(T{-1} / samples);
}

/// The complementary pole of `ms_to_onepole_coef`: `p = 1 − a = exp(−1/(τ·fs))`,
/// the `p` in `y = p·y + (1 − p)·x`. Same τ convention.
template <typename T>
inline T ms_to_onepole_pole(T ms, T sample_rate) {
    return T{1} - ms_to_onepole_coef(ms, sample_rate);
}

/// A 60 dB decay time in ms → the one-pole time constant τ in ms that produces
/// it. Pair with `ms_to_onepole_coef` when a spec states a t60 rather than a τ.
template <typename T>
inline T t60_ms_to_tau_ms(T t60_ms) {
    constexpr T ln1000 = static_cast<T>(6.907755278982137L);
    return t60_ms / ln1000;
}

// ── Parameter tapers ──────────────────────────────────────────────────────

/// Geometric ("log") taper: maps a normalised knob position `u ∈ [0, 1]` onto
/// `[lo, hi]` so equal knob travel is equal RATIO — the right law for anything
/// heard geometrically (frequency, delay time, drive). `u = 0.5` lands on the
/// geometric mean `sqrt(lo·hi)`, not the arithmetic midpoint.
///
/// Requires `lo > 0`; a taper through zero has no geometric midpoint. Returns
/// `lo` unchanged if that precondition fails rather than producing a NaN.
template <typename T>
inline T taper_log(T u, T lo, T hi) {
    if (!(lo > T{0}) || !(hi > T{0})) return lo;
    const T clamped = std::clamp(u, T{0}, T{1});
    return lo * std::pow(hi / lo, clamped);
}

/// Inverse of `taper_log`: a value in `[lo, hi]` → its knob position in [0, 1].
template <typename T>
inline T untaper_log(T value, T lo, T hi) {
    if (!(lo > T{0}) || !(hi > T{0}) || hi == lo) return T{0};
    const T clamped = std::clamp(value, std::min(lo, hi), std::max(lo, hi));
    return std::log(clamped / lo) / std::log(hi / lo);
}

/// Linear taper — the arithmetic counterpart of `taper_log`, present so a
/// parameter table can name its law instead of open-coding a lerp.
template <typename T>
inline T taper_linear(T u, T lo, T hi) {
    return lo + std::clamp(u, T{0}, T{1}) * (hi - lo);
}

/// Inverse of `taper_linear`.
template <typename T>
inline T untaper_linear(T value, T lo, T hi) {
    if (hi == lo) return T{0};
    return std::clamp((value - lo) / (hi - lo), T{0}, T{1});
}

/// Compatibility spelling used by the shared modulation toolkit.
template <typename T>
inline T taper_log_inverse(T value, T lo, T hi) {
    return untaper_log(value, lo, hi);
}

/// Per-sample gain for a feedback path that decays by 60 dB in `t60_seconds`.
template <typename T>
inline T t60_to_per_sample_gain(T t60_seconds, T sample_rate) {
    const T samples = std::max(T{1}, t60_seconds * sample_rate);
    return std::pow(T{10}, T{-3} / samples);
}

template <typename T>
inline T beats_to_seconds(T beats, T bpm) {
    return beats * T{60} / std::max(T{1e-3}, bpm);
}

template <typename T>
inline T seconds_to_beats(T seconds, T bpm) {
    return seconds * std::max(T{1e-3}, bpm) / T{60};
}

/// Shared serialized musical-division vocabulary. Order is ABI/preset state:
/// append before `count`; never reorder existing entries.
enum class Division : std::uint8_t {
    whole,
    whole_dotted,
    whole_triplet,
    half,
    half_dotted,
    half_triplet,
    quarter,
    quarter_dotted,
    quarter_triplet,
    eighth,
    eighth_dotted,
    eighth_triplet,
    sixteenth,
    sixteenth_dotted,
    sixteenth_triplet,
    thirty_second,
    thirty_second_dotted,
    thirty_second_triplet,
    sixty_fourth,
    sixty_fourth_dotted,
    sixty_fourth_triplet,
    count,
};

inline constexpr int kDivisionCount = static_cast<int>(Division::count);

constexpr float division_to_beats(Division division) {
    const int index = static_cast<int>(division);
    if (index < 0 || index >= kDivisionCount) return 1.0f;
    const int family = index / 3;
    float beats = 4.0f;
    for (int i = 0; i < family; ++i) beats *= 0.5f;
    switch (index % 3) {
        case 1: return beats * 1.5f;
        case 2: return beats * (2.0f / 3.0f);
        default: return beats;
    }
}

constexpr float division_to_beats(int index) {
    return division_to_beats(static_cast<Division>(index));
}

inline float division_to_seconds(Division division, float bpm) {
    return beats_to_seconds(division_to_beats(division), bpm);
}

inline double division_to_samples(Division division, float bpm, double sample_rate) {
    return static_cast<double>(division_to_seconds(division, bpm)) * sample_rate;
}

inline float division_to_hz(Division division, float bpm) {
    return 1.0f / std::max(1.0e-6f, division_to_seconds(division, bpm));
}

}  // namespace pulp::signal::units
