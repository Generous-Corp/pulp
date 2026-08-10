// The behaviour report, as the one document every reader agrees on.
//
// The gate's stdout is read by three audiences and they want different things:
// a person diagnosing a patch wants prose, the model retrying a generation
// wants a short sentence naming what was wrong, and Python wants numbers it can
// put a predicate over. Prose that a parser scrapes is a contract nobody wrote
// down, and it breaks the first time somebody improves a sentence. So the
// numbers are emitted once, as JSON, on a line with a marker in front of it,
// and the prose is written for people without a parser looking over its
// shoulder.
//
// SCHEMA STABILITY: `schema` is bumped when a field changes meaning or leaves.
// Adding a field is not a bump — every reader takes what it knows and ignores
// the rest. A consumer that finds a `schema` higher than it understands should
// say so rather than guess.

#pragma once

#include "patch_behaviour.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace patch_behaviour {

/// The line prefix a parser looks for. One line, one JSON object, so a reader
/// needs no bracket counting and interleaved output cannot confuse it.
inline constexpr const char* kJsonMarker = "BEHAVIOUR_JSON ";

/// Bumped only when a field changes meaning or is removed.
inline constexpr int kSchema = 1;

namespace detail {

inline std::string num(double v, int places = 4) {
    // JSON has no NaN or Infinity. A measurement that could not be made is
    // null, which is a different statement from zero and reads as one.
    if (!std::isfinite(v)) return "null";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", places, v);
    return buf;
}

inline std::string ints(const std::vector<int>& v) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += ",";
        s += std::to_string(v[i]);
    }
    return s + "]";
}

inline std::string nums(const std::vector<double>& v, int places = 4) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += ",";
        s += num(v[i], places);
    }
    return s + "]";
}

inline std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) continue;
        else out += c;
    }
    return out + "\"";
}

}  // namespace detail

/// The settings actually used, so a report is self-describing: reading an old
/// report tells you what the numbers in it meant, without knowing which version
/// of this file produced them.
inline std::string settings_json(const Settings& s) {
    using detail::num;
    return std::string("{\"sample_rate\":") + num(s.sample_rate, 1) +
           ",\"window_ms\":" + num(s.window_ms, 2) +
           ",\"envelope_window_ms\":" + num(s.envelope_window_ms, 2) +
           ",\"envelope_hop_ms\":" + num(s.envelope_hop_ms, 2) +
           ",\"f0_min_hz\":" + num(s.f0_min_hz, 2) +
           ",\"f0_max_hz\":" + num(s.f0_max_hz, 2) +
           ",\"voiced_window_fraction\":" + num(s.voiced_window_fraction, 3) +
           ",\"min_note_windows\":" + std::to_string(s.min_note_windows) +
           ",\"onset_threshold_mult\":" + num(s.onset_threshold_mult, 3) +
           ",\"onset_threshold_delta\":" + num(s.onset_threshold_delta, 3) +
           ",\"onset_median_hops\":" + std::to_string(s.onset_median_hops) +
           ",\"onset_min_gap_ms\":" + num(s.onset_min_gap_ms, 2) +
           ",\"active_floor_ratio\":" + num(s.active_floor_ratio, 4) +
           ",\"active_floor_v\":" + num(s.active_floor_v, 6) +
           ",\"tail_ms\":" + num(s.tail_ms, 2) +
           ",\"centroid_fft\":" + std::to_string(s.centroid_fft) +
           ",\"max_period_ms\":" + num(s.max_period_ms, 2) + "}";
}

/// One measured cable. `series` adds the per-window tracks, which are the whole
/// diagnosis when a number reads wrong and dead weight when it does not — so
/// they are off by default and a flag away.
inline std::string cable_json(const Behaviour& b, bool series) {
    using detail::num;
    std::string s = "{\"source\":" + detail::quoted(b.source) +
                    ",\"seconds\":" + num(b.seconds, 3) +
                    ",\"finite\":" + (b.finite ? "true" : "false") +
                    ",\"mean_abs_v\":" + num(b.mean_abs_v, 6) +
                    ",\"peak_abs_v\":" + num(b.peak_abs_v, 6);

    s += ",\"pitch\":{\"windows\":" + std::to_string(b.pitch.windows) +
         ",\"voiced_windows\":" + std::to_string(b.pitch.voiced_windows) +
         ",\"voiced_fraction\":" + num(b.pitch.voiced_fraction, 4) +
         ",\"notes\":" + std::to_string(b.pitch.notes) +
         ",\"distinct_pitches\":" + std::to_string(b.pitch.distinct_pitches) +
         ",\"pitch_changes\":" + std::to_string(b.pitch.pitch_changes) +
         ",\"semitone_range\":" + std::to_string(b.pitch.semitone_range) +
         ",\"median_hz\":" + num(b.pitch.median_hz, 3);
    if (series) {
        s += ",\"semitones\":" + detail::ints(b.pitch.semitones);
        std::vector<int> note_semis;
        for (const Note& n : b.pitch.note_runs) note_semis.push_back(n.semitone);
        s += ",\"note_semitones\":" + detail::ints(note_semis);
    }
    s += "}";

    s += ",\"dynamics\":{\"peak_rms\":" + num(b.dynamics.peak_rms, 6) +
         ",\"mean_rms\":" + num(b.dynamics.mean_rms, 6) +
         ",\"end_rms\":" + num(b.dynamics.end_rms, 6) +
         ",\"end_over_peak\":" + num(b.dynamics.end_over_peak, 4) +
         ",\"duty_cycle\":" + num(b.dynamics.duty_cycle, 4) +
         ",\"rms_trend\":" + num(b.dynamics.rms_trend, 4);
    if (series) s += ",\"window_rms\":" + detail::nums(b.dynamics.window_rms, 6);
    s += "}";

    s += ",\"onsets\":{\"onsets\":" + std::to_string(b.onsets.onsets) +
         ",\"per_second\":" + num(b.onsets.per_second, 3) +
         ",\"interval_mean_ms\":" + num(b.onsets.interval_mean_ms, 2) +
         ",\"interval_cv\":" + num(b.onsets.interval_cv, 4) +
         ",\"interval_trend\":" + num(b.onsets.interval_trend, 4) +
         ",\"periodicity\":" + num(b.onsets.periodicity, 4) +
         ",\"period_ms\":" + num(b.onsets.period_ms, 2);
    if (series) s += ",\"times_ms\":" + detail::nums(b.onsets.times_ms, 1);
    s += "}";

    s += ",\"spectrum\":{\"centroid_mean_hz\":" + num(b.spectrum.centroid_mean_hz, 2) +
         ",\"centroid_cv\":" + num(b.spectrum.centroid_cv, 4) +
         ",\"centroid_trend\":" + num(b.spectrum.centroid_trend, 4) +
         ",\"centroid_range_octaves\":" + num(b.spectrum.centroid_range_octaves, 4) +
         ",\"active_windows\":" + std::to_string(b.spectrum.active_windows);
    if (series) s += ",\"centroid_hz\":" + detail::nums(b.spectrum.centroid_hz, 2);
    s += "}}";
    return s;
}

/// The whole block. `loudest` indexes `cables` and is a convenience, not a
/// judgement: a caller asking "is there a melody in this patch" should ask
/// every cable, because a lead against a bass drone is not the louder of the
/// two often enough to bet on.
inline std::string report_json(const std::vector<Behaviour>& cables,
                               const Settings& s, bool series) {
    std::size_t loudest = 0;
    for (std::size_t i = 1; i < cables.size(); ++i)
        if (cables[i].mean_abs_v > cables[loudest].mean_abs_v) loudest = i;
    std::string out = std::string("{\"schema\":") + std::to_string(kSchema) +
                      ",\"settings\":" + settings_json(s) +
                      ",\"loudest\":" + std::to_string(cables.empty() ? 0 : loudest) +
                      ",\"cables\":[";
    for (std::size_t i = 0; i < cables.size(); ++i) {
        if (i) out += ",";
        out += cable_json(cables[i], series);
    }
    return out + "]}";
}

/// The same facts for a person, who does not want to read JSON to find out that
/// a melodic request produced one pitch.
inline std::string cable_summary(const Behaviour& b) {
    char buf[512];
    std::string out;
    std::snprintf(buf, sizeof buf,
                  "        %s over %.1f s\n", b.source.c_str(), b.seconds);
    out += buf;
    if (!b.finite) return out + "          not measurable: the signal contained NaN or Inf\n";
    std::snprintf(buf, sizeof buf,
                  "          pitch     %d distinct over %d notes, %d changes, "
                  "%.0f%% voiced, median %.1f Hz, range %d semitones\n",
                  b.pitch.distinct_pitches, b.pitch.notes, b.pitch.pitch_changes,
                  100.0 * b.pitch.voiced_fraction, b.pitch.median_hz,
                  b.pitch.semitone_range);
    out += buf;
    std::snprintf(buf, sizeof buf,
                  "          dynamics  peak rms %.3f V, end rms %.3f V (%.2fx), "
                  "duty %.0f%%, trend %+.2f\n",
                  b.dynamics.peak_rms, b.dynamics.end_rms, b.dynamics.end_over_peak,
                  100.0 * b.dynamics.duty_cycle, b.dynamics.rms_trend);
    out += buf;
    std::snprintf(buf, sizeof buf,
                  "          onsets    %d (%.1f/s), interval %.0f ms cv %.2f "
                  "trend %+.2f, periodicity %.2f at %.0f ms\n",
                  b.onsets.onsets, b.onsets.per_second, b.onsets.interval_mean_ms,
                  b.onsets.interval_cv, b.onsets.interval_trend,
                  b.onsets.periodicity, b.onsets.period_ms);
    out += buf;
    std::snprintf(buf, sizeof buf,
                  "          spectrum  centroid %.0f Hz, cv %.2f, trend %+.2f, "
                  "range %.2f octaves\n",
                  b.spectrum.centroid_mean_hz, b.spectrum.centroid_cv,
                  b.spectrum.centroid_trend, b.spectrum.centroid_range_octaves);
    out += buf;
    return out;
}

}  // namespace patch_behaviour
