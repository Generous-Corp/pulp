#pragma once

/// @file units.hpp
/// Real-unit conversions shared by the modulation library, plus the one musical
/// division table the whole catalog agrees on.
///
/// RT contract: everything here is a pure function of its arguments with no
/// state and no allocation. All of it is audio-thread safe.
///
/// USE: the series law is "real units everywhere (Hz/ms/s), never normalized
/// magic". That only works if converting between them is one obvious call
/// rather than an inline `powf` someone got the sign of wrong. Two conversions
/// that already existed in `special_functions.hpp` — dB and MIDI pitch — are
/// re-exported here under their conventional names rather than reimplemented,
/// so there is exactly one implementation of each in the tree.

#include <pulp/signal/special_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::signal::units {

// ── amplitude ────────────────────────────────────────────────────────────────

/// Decibels to linear gain. Re-exported from `pulp::signal`.
using pulp::signal::db_to_linear;

/// Linear gain to decibels (floored at -200 dB). Re-exported from
/// `pulp::signal`.
using pulp::signal::linear_to_db;

// ── pitch ────────────────────────────────────────────────────────────────────

/// MIDI note number to frequency (A4 = 69 = 440 Hz).
///
/// `pulp::signal::midi_to_freq` is the same function; this name exists because
/// every other conversion here ends in the unit it produces and a caller
/// reaching for `hz_to_midi` should find its inverse next to it.
inline float midi_to_hz(float note) { return pulp::signal::midi_to_freq(note); }

/// Frequency to MIDI note number (A4 = 69 = 440 Hz).
inline float hz_to_midi(float hz) { return pulp::signal::freq_to_midi(hz); }

/// Semitone offset to a frequency multiplier.
inline float semitones_to_ratio(float semitones) {
    return std::exp2(semitones * (1.0f / 12.0f));
}

/// Cent offset to a frequency multiplier. Detune, drift, and vibrato all live
/// in cents because a cent is the same perceptual step at every pitch, and a Hz
/// offset is not.
inline float cents_to_ratio(float cents) {
    return std::exp2(cents * (1.0f / 1200.0f));
}

/// Frequency multiplier back to cents.
inline float ratio_to_cents(float ratio) {
    return 1200.0f * std::log2(std::max(ratio, 1.0e-9f));
}

// ── time ─────────────────────────────────────────────────────────────────────

/// One-pole smoothing coefficient for a given time constant.
///
/// The result `a` is used as `y += a * (x - y)`, which reaches 63.2% of a step
/// after `ms`. That is a *time constant*, not a settling time: a step is within
/// 1% after roughly 5x `ms`.
inline float ms_to_onepole_coef(float ms, float sample_rate) {
    const float samples = std::max(1.0e-6f, ms * 0.001f * sample_rate);
    return 1.0f - std::exp(-1.0f / samples);
}

/// Per-sample gain for a feedback path that should decay 60 dB in `t60`
/// seconds. Multiply the feedback signal by this once per sample.
inline float t60_to_per_sample_gain(float t60_seconds, float sample_rate) {
    const float samples = std::max(1.0f, t60_seconds * sample_rate);
    return std::pow(10.0f, -3.0f / samples);
}

/// Musical beats to seconds. A "beat" here is a quarter note, matching the
/// division table below and every host's BPM.
inline float beats_to_seconds(float beats, float bpm) {
    return beats * 60.0f / std::max(1.0e-3f, bpm);
}

/// Seconds to beats at a tempo.
inline float seconds_to_beats(float seconds, float bpm) {
    return seconds * std::max(1.0e-3f, bpm) / 60.0f;
}

// ── tapers ───────────────────────────────────────────────────────────────────

/// Exponential ("log-feel") taper: maps `t` in [0, 1] onto [lo, hi] with equal
/// ratios per equal knob travel.
///
/// USE: any control whose perception is multiplicative — LFO rate, filter
/// cutoff, delay time. A linear knob over 0.01-20 Hz spends 95% of its travel
/// above 1 Hz; this one spends half its travel below 0.45 Hz, which is where
/// the musically distinct settings actually are. Requires `lo > 0`.
inline float taper_log(float t, float lo, float hi) {
    const float safe_lo = std::max(lo, 1.0e-9f);
    const float safe_hi = std::max(hi, safe_lo * 1.000001f);
    return safe_lo * std::pow(safe_hi / safe_lo, std::clamp(t, 0.0f, 1.0f));
}

/// Inverse of `taper_log()` — value back to normalized knob position.
inline float taper_log_inverse(float value, float lo, float hi) {
    const float safe_lo = std::max(lo, 1.0e-9f);
    const float safe_hi = std::max(hi, safe_lo * 1.000001f);
    const float clamped = std::clamp(value, safe_lo, safe_hi);
    return std::log(clamped / safe_lo) / std::log(safe_hi / safe_lo);
}

// ── musical divisions ────────────────────────────────────────────────────────

/// The shared musical-division vocabulary.
///
/// **The order is locked.** `division_to_beats(int)` indexes this enum, and
/// serialized presets, the mod-matrix source list, and every synced LFO / gate /
/// burst in the catalog store the integer. Append new entries before `count`;
/// never reorder or remove.
///
/// Straight, dotted (x1.5), and triplet (x2/3) forms are all present for every
/// note value. Both in-house sync tables carried triplets and one carried
/// dotted, so supporting only one of the two would force a caller to
/// special-case the other.
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

/// Length of a division in beats (quarter notes).
///
/// A whole note is 4 beats, so `1/8T` is `0.5 * 2/3 = 1/3` beat — three of them
/// per beat, which is what a triplet eighth means and what every caller in the
/// catalog will now agree on.
constexpr float division_to_beats(Division d) {
    const int index = static_cast<int>(d);
    if (index < 0 || index >= kDivisionCount) return 1.0f;
    // Straight length of the note value this index belongs to: 4, 2, 1, ...
    const int family = index / 3;
    float beats = 4.0f;
    for (int i = 0; i < family; ++i) beats *= 0.5f;
    switch (index % 3) {
        case 1: return beats * 1.5f;          // dotted
        case 2: return beats * (2.0f / 3.0f); // triplet
        default: return beats;                // straight
    }
}

/// Index overload for callers holding a serialized parameter value.
constexpr float division_to_beats(int index) {
    return division_to_beats(static_cast<Division>(index));
}

/// Seconds per cycle of a division at a tempo.
inline float division_to_seconds(Division d, float bpm) {
    return beats_to_seconds(division_to_beats(d), bpm);
}

/// Samples per cycle of a division — the value to hand
/// `LfoT::set_period_samples()`, `GateGenT::set_length_samples()`, or any other
/// synced primitive. Tempo-to-period conversion is the caller's job precisely
/// so that one host transport read feeds every synced object in the plugin.
inline double division_to_samples(Division d, float bpm, double sample_rate) {
    return static_cast<double>(division_to_seconds(d, bpm)) * sample_rate;
}

/// Frequency of a division at a tempo — for callers driving an LFO by rate
/// rather than period.
inline float division_to_hz(Division d, float bpm) {
    return 1.0f / std::max(1.0e-6f, division_to_seconds(d, bpm));
}

} // namespace pulp::signal::units
