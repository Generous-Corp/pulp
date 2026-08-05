// What a patch DOES, measured — pitch, articulation, level and brightness over
// time, as numbers rather than a verdict.
//
// The audibility gate answers "is there signal", and signal was never the
// property anyone asked for. A held single note passes an audibility check with
// a perfect score; so does a drone when the request said melody, and a wash
// when the request said rhythm. Presence is the precondition, not the property.
//
// So this measures the property and REPORTS it. Nothing here decides whether a
// patch is good: it produces numbers, and the caller decides which numbers a
// given request needs. That split is deliberate and load-bearing. A gate that
// returns a verdict can only say no; a gate that returns measurements can say
// "you asked for a melody and this is one pitch for six seconds", which is a
// sentence a model can act on and a person can disagree with.
//
// EVERY CONSTANT IN `Settings` IS A GUESS. They are placeholders chosen to be
// reasonable, not thresholds validated against patches anybody has heard. They
// live in one struct, they are overridable, and the values actually used are
// emitted in the report — so tuning them is a data edit with an audit trail
// rather than a source change. Treat a number here as a hypothesis.
//
// The estimators are Pulp's own, which ship in the same bundle as this file:
// `pulp::signal::YinTrackerT` for f0 (cumulative mean normalized difference
// with parabolic refinement and a median across hops, which is a great deal
// better than the bare autocorrelation this would otherwise hand-roll) and
// `pulp::signal::FftT` for the spectrum. Both are header-only and both are
// already required inputs for module generation, so depending on them here
// adds no new thing a machine has to have.

#pragma once

#include <pulp/signal/fft.hpp>
#include <pulp/signal/yin_tracker.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <string>
#include <vector>

namespace patch_behaviour {

/// How the signal is measured. Not what counts as good — that is the caller's.
///
/// Each field is a guess with a stated reason. The reasons are the useful part:
/// when a measurement reads wrong on a patch someone has heard, the reason says
/// which number to move and what moving it costs.
struct Settings {
    /// Analysis rate. The gate steps its DSP here, so this is a fact, not a
    /// choice — but the measurement is written to work at any rate.
    double sample_rate = 48000.0;

    /// The pitch/level window. 50 ms is short enough to see a sixteenth note at
    /// 120 bpm (125 ms) and long enough to hold two periods of the lowest pitch
    /// tracked. Shorter windows fragment held notes; longer ones swallow fast
    /// runs.
    double window_ms = 50.0;

    /// The onset envelope is measured far finer than the pitch track, because
    /// an attack is a millisecond-scale event and a 50 ms window cannot tell a
    /// struck note from a swell.
    ///
    /// The window is TAPERED and deliberately several periods long. A flat
    /// block of samples whose length is not a whole number of the signal's
    /// periods measures a different amount of energy each hop purely because
    /// the partial cycle at its edge moves — a sustained sawtooth read through
    /// a 20 ms rectangular window beats against its own period and produces a
    /// stream of attacks that are not there. Tapering removes the edge, which
    /// removes the beat.
    double envelope_window_ms = 40.0;
    double envelope_hop_ms = 5.0;

    /// The tracked fundamental range. 55 Hz is A1 — below a bass guitar's low
    /// E, which is as low as a *melody* goes; a sub-bass drone under it reads
    /// as unvoiced, and `voiced_fraction` says so rather than the tracker
    /// inventing an octave. The ceiling is above the top of a piano.
    double f0_min_hz = 55.0;
    double f0_max_hz = 1500.0;

    /// A window counts as voiced when at least this fraction of the tracker's
    /// estimates inside it were confident. One confident estimate in nine is
    /// noise; most of them is a note.
    double voiced_window_fraction = 0.5;

    /// How long a pitch must hold to count as a note, in windows. One window is
    /// a glide artefact or an octave flip the median did not catch; two (100 ms)
    /// is something a listener hears as a pitch. This is what stops
    /// `pitch_changes` counting vibrato as a melody.
    int min_note_windows = 2;

    /// Onset peak-picking. The detection function is the half-wave-rectified
    /// rise in log energy; a peak counts when it stands above `mult` times the
    /// local median plus `delta`. The median makes the threshold adapt to the
    /// patch's own busyness instead of to its level.
    double onset_threshold_mult = 1.6;
    double onset_threshold_delta = 0.08;
    /// Half-width of that median, in envelope hops. 20 hops is ±100 ms.
    int onset_median_hops = 20;
    /// Two attacks closer than this are one attack. 60 ms is ~16 events/s,
    /// past which a listener hears a texture rather than a rhythm.
    double onset_min_gap_ms = 60.0;

    /// A window is "active" above the larger of these. The ratio makes the
    /// floor follow the patch's own level, so a quiet patch is not read as
    /// mostly silent; the absolute volt floor stops a patch that is genuinely
    /// silent from looking 100% active relative to its own noise.
    double active_floor_ratio = 0.05;
    double active_floor_v = 1e-3;

    /// `end_rms` is measured over the final stretch rather than the last
    /// window, so one gap between notes does not read as a patch that stopped.
    double tail_ms = 500.0;

    /// Spectrum window. Must be a power of two and must fit inside `window_ms`
    /// worth of samples; 2048 at 48 kHz is 43 ms, inside a 50 ms window, with
    /// 23 Hz bins.
    int centroid_fft = 2048;

    /// Longest period the onset autocorrelation looks for. Past 3 s a "period"
    /// over a 6 s run is two events, which is not evidence of a pulse.
    double max_period_ms = 3000.0;
};

/// A pitch that was held long enough to be a note.
struct Note {
    int semitone = -1;   // MIDI note number
    int start_window = 0;
    int windows = 0;
};

struct PitchMeasure {
    int windows = 0;
    int voiced_windows = 0;
    double voiced_fraction = 0.0;
    /// Held pitches, in order. `distinct_pitches` and `pitch_changes` are
    /// counted over these, never over the raw window track — the raw track
    /// counts every flicker as a melody.
    int notes = 0;
    int distinct_pitches = 0;
    int pitch_changes = 0;
    int semitone_range = 0;
    double median_hz = 0.0;
    /// Per window, MIDI note number, or -1 where the window was unvoiced.
    std::vector<int> semitones;
    std::vector<Note> note_runs;
};

struct DynamicsMeasure {
    double peak_rms = 0.0;
    double mean_rms = 0.0;
    double end_rms = 0.0;
    /// `end_rms / peak_rms`. The number behind "did it keep going".
    double end_over_peak = 0.0;
    /// Fraction of windows above the active floor. A drone is near 1; a sparse
    /// rhythm is well under it.
    double duty_cycle = 0.0;
    /// Least-squares slope across the run, scaled by run length and divided by
    /// the mean: +1 means the level grew by one mean's worth over the run.
    /// Dimensionless, so it compares across patches at different levels.
    double rms_trend = 0.0;
    std::vector<double> window_rms;
};

struct OnsetMeasure {
    int onsets = 0;
    double per_second = 0.0;
    double interval_mean_ms = 0.0;
    /// Coefficient of variation of the inter-onset intervals. Near 0 is a
    /// metronome; high is rubato or randomness.
    double interval_cv = 0.0;
    /// Normalized slope of the interval series. Negative means the intervals
    /// are shrinking — the patch is speeding up.
    double interval_trend = 0.0;
    /// Peak of the normalized autocorrelation of the onset detection function,
    /// 0..1, and the lag it sits at. This is the "is there a pulse" number;
    /// `interval_cv` is the same question asked a second, independent way, and
    /// the two disagreeing is worth knowing.
    double periodicity = 0.0;
    double period_ms = 0.0;
    std::vector<double> times_ms;
};

struct SpectrumMeasure {
    double centroid_mean_hz = 0.0;
    double centroid_cv = 0.0;
    /// Same normalization as `rms_trend`: a filter opening across the run reads
    /// positive, a decaying one negative.
    double centroid_trend = 0.0;
    /// Spread between the 10th and 90th percentile centroid, in octaves. Robust
    /// to a single odd window in a way max/min is not.
    double centroid_range_octaves = 0.0;
    int active_windows = 0;
    std::vector<double> centroid_hz;
};

struct Behaviour {
    Settings settings;
    double seconds = 0.0;
    /// Which cable this describes, for a report that names its source.
    std::string source;
    double mean_abs_v = 0.0;
    double peak_abs_v = 0.0;
    bool finite = true;
    PitchMeasure pitch;
    DynamicsMeasure dynamics;
    OnsetMeasure onsets;
    SpectrumMeasure spectrum;
};

// ── small statistics, shared by several measures ────────────────────────────

namespace detail {

inline double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(mid), v.end());
    return v[mid];
}

inline double percentile_of(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double pos = p * static_cast<double>(v.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(pos));
    const std::size_t hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (pos - static_cast<double>(lo));
}

/// Least-squares slope of `y` against its own index, scaled by the number of
/// points and divided by the mean. The result answers "by how many mean-sized
/// steps did this move across the run", which is comparable between a patch at
/// 0.1 V and one at 8 V; a raw slope is not.
inline double normalized_trend(const std::vector<double>& y) {
    const std::size_t n = y.size();
    if (n < 2) return 0.0;
    double sum = 0.0;
    for (double v : y) sum += v;
    const double mean_y = sum / static_cast<double>(n);
    if (!(std::fabs(mean_y) > 0.0)) return 0.0;
    const double mean_x = static_cast<double>(n - 1) / 2.0;
    double num = 0.0, den = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double dx = static_cast<double>(i) - mean_x;
        num += dx * (y[i] - mean_y);
        den += dx * dx;
    }
    if (!(den > 0.0)) return 0.0;
    return (num / den) * static_cast<double>(n - 1) / mean_y;
}

inline double coefficient_of_variation(const std::vector<double>& v) {
    if (v.size() < 2) return 0.0;
    double sum = 0.0;
    for (double x : v) sum += x;
    const double mean = sum / static_cast<double>(v.size());
    if (!(std::fabs(mean) > 0.0)) return 0.0;
    double var = 0.0;
    for (double x : v) var += (x - mean) * (x - mean);
    var /= static_cast<double>(v.size());
    return std::sqrt(var) / std::fabs(mean);
}

inline int semitone_of(double hz) {
    if (!(hz > 0.0) || !std::isfinite(hz)) return -1;
    return static_cast<int>(std::lround(69.0 + 12.0 * std::log2(hz / 440.0)));
}

}  // namespace detail

// ── the measurement ─────────────────────────────────────────────────────────

/// Per-window fundamental, as MIDI note numbers with -1 for unvoiced.
///
/// The tracker reports one estimate per hop and its estimate for a hop
/// describes the window ENDING there, so each estimate is credited to the
/// window containing its centre — `hop_index - latency/2`. Skipping that shift
/// puts a note's pitch one window late, which at 50 ms is enough to smear every
/// transition in a fast line into a spurious extra note.
inline PitchMeasure measure_pitch(const std::vector<float>& x, const Settings& s) {
    PitchMeasure out;
    const long win = std::max(1L, std::lround(s.window_ms * s.sample_rate / 1000.0));
    const std::size_t windows = x.size() / static_cast<std::size_t>(win);
    if (windows == 0) return out;
    out.windows = static_cast<int>(windows);

    pulp::signal::YinTrackerT<float> yin;
    yin.set_f0_range(s.f0_min_hz, s.f0_max_hz);
    yin.prepare(s.sample_rate);
    const long half_latency = yin.latency_samples() / 2;

    std::vector<std::vector<double>> voiced(windows);
    std::vector<int> estimates(windows, 0);
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!yin.process(x[i])) continue;
        const long centre = static_cast<long>(i) - half_latency;
        if (centre < 0) continue;
        const std::size_t w = static_cast<std::size_t>(centre / win);
        if (w >= windows) continue;
        ++estimates[w];
        if (yin.voiced()) voiced[w].push_back(yin.f0_hz());
    }

    std::vector<double> all_voiced;
    out.semitones.assign(windows, -1);
    for (std::size_t w = 0; w < windows; ++w) {
        if (estimates[w] == 0) continue;
        const double share = static_cast<double>(voiced[w].size()) /
                             static_cast<double>(estimates[w]);
        if (share < s.voiced_window_fraction) continue;
        const double hz = detail::median_of(voiced[w]);
        const int semi = detail::semitone_of(hz);
        if (semi < 0) continue;
        out.semitones[w] = semi;
        ++out.voiced_windows;
        all_voiced.push_back(hz);
    }
    out.voiced_fraction = static_cast<double>(out.voiced_windows) /
                          static_cast<double>(windows);
    out.median_hz = detail::median_of(all_voiced);

    // Run-length encode, then keep only the runs long enough to be heard as a
    // pitch. A patch that wobbles a semitone every window is not melodic and
    // must not score as though it were.
    std::vector<Note> runs;
    for (std::size_t w = 0; w < windows; ++w) {
        const int semi = out.semitones[w];
        if (semi < 0) continue;
        if (!runs.empty() && runs.back().semitone == semi &&
            static_cast<std::size_t>(runs.back().start_window + runs.back().windows) == w) {
            ++runs.back().windows;
            continue;
        }
        runs.push_back(Note{semi, static_cast<int>(w), 1});
    }
    for (const Note& n : runs)
        if (n.windows >= s.min_note_windows) out.note_runs.push_back(n);

    out.notes = static_cast<int>(out.note_runs.size());
    std::vector<int> distinct;
    for (std::size_t i = 0; i < out.note_runs.size(); ++i) {
        distinct.push_back(out.note_runs[i].semitone);
        if (i && out.note_runs[i].semitone != out.note_runs[i - 1].semitone)
            ++out.pitch_changes;
    }
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
    out.distinct_pitches = static_cast<int>(distinct.size());
    out.semitone_range = distinct.empty() ? 0 : distinct.back() - distinct.front();
    return out;
}

inline DynamicsMeasure measure_dynamics(const std::vector<float>& x, const Settings& s) {
    DynamicsMeasure out;
    const long win = std::max(1L, std::lround(s.window_ms * s.sample_rate / 1000.0));
    const std::size_t windows = x.size() / static_cast<std::size_t>(win);
    if (windows == 0) return out;

    out.window_rms.reserve(windows);
    for (std::size_t w = 0; w < windows; ++w) {
        double acc = 0.0;
        for (long i = 0; i < win; ++i) {
            const double v = x[w * static_cast<std::size_t>(win) + static_cast<std::size_t>(i)];
            acc += v * v;
        }
        out.window_rms.push_back(std::sqrt(acc / static_cast<double>(win)));
    }
    out.peak_rms = *std::max_element(out.window_rms.begin(), out.window_rms.end());
    double sum = 0.0;
    for (double v : out.window_rms) sum += v;
    out.mean_rms = sum / static_cast<double>(windows);

    const std::size_t tail = std::min<std::size_t>(
        windows, std::max<std::size_t>(1, static_cast<std::size_t>(
                     std::lround(s.tail_ms / s.window_ms))));
    double tail_acc = 0.0;
    for (std::size_t w = windows - tail; w < windows; ++w)
        tail_acc += out.window_rms[w] * out.window_rms[w];
    out.end_rms = std::sqrt(tail_acc / static_cast<double>(tail));
    out.end_over_peak = out.peak_rms > 0.0 ? out.end_rms / out.peak_rms : 0.0;

    const double floor_v = std::max(s.active_floor_v, s.active_floor_ratio * out.peak_rms);
    std::size_t active = 0;
    for (double v : out.window_rms) if (v > floor_v) ++active;
    out.duty_cycle = static_cast<double>(active) / static_cast<double>(windows);
    out.rms_trend = detail::normalized_trend(out.window_rms);
    return out;
}

/// Attacks, and whether they fall on a pulse.
///
/// The detection function is the rise in LOG energy, not in energy: a note
/// starting from near-silence and a note starting over a held drone are the
/// same musical event, and only the log makes them the same number. The
/// epsilon is set relative to the loudest frame for the same reason — a fixed
/// floor makes a quiet patch's noise look like a stream of attacks.
inline OnsetMeasure measure_onsets(const std::vector<float>& x, const Settings& s) {
    OnsetMeasure out;
    const long hop = std::max(1L, std::lround(s.envelope_hop_ms * s.sample_rate / 1000.0));
    const long ewin = std::max(hop, std::lround(s.envelope_window_ms * s.sample_rate / 1000.0));
    if (static_cast<long>(x.size()) < ewin + hop) return out;
    const std::size_t frames = (x.size() - static_cast<std::size_t>(ewin)) /
                               static_cast<std::size_t>(hop) + 1;

    std::vector<double> taper(static_cast<std::size_t>(ewin));
    double taper_sum = 0.0;
    for (long i = 0; i < ewin; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(i) /
                                              static_cast<double>(ewin - 1));
        taper[static_cast<std::size_t>(i)] = w;
        taper_sum += w;
    }

    std::vector<double> energy(frames, 0.0);
    for (std::size_t k = 0; k < frames; ++k) {
        double acc = 0.0;
        const std::size_t start = k * static_cast<std::size_t>(hop);
        for (long i = 0; i < ewin; ++i) {
            const double v = x[start + static_cast<std::size_t>(i)];
            acc += taper[static_cast<std::size_t>(i)] * v * v;
        }
        energy[k] = acc / taper_sum;
    }
    // A tapered frame's energy belongs to its middle, not its start. Reporting
    // the start puts every onset half a window late, which is a fixed offset
    // the intervals cancel but the absolute times do not.
    const double frame_offset_ms = 0.5 * static_cast<double>(ewin) * 1000.0 / s.sample_rate;
    const double peak_e = *std::max_element(energy.begin(), energy.end());
    if (!(peak_e > 0.0)) return out;
    const double eps = std::max(1e-12, 1e-4 * peak_e);

    std::vector<double> flux(frames, 0.0);
    for (std::size_t k = 1; k < frames; ++k)
        flux[k] = std::max(0.0, std::log(energy[k] + eps) - std::log(energy[k - 1] + eps));

    const double floor_e = std::max(s.active_floor_v * s.active_floor_v,
                                    s.active_floor_ratio * s.active_floor_ratio * peak_e);
    const long gap_frames = std::max(1L, std::lround(s.onset_min_gap_ms / s.envelope_hop_ms));
    const int w = std::max(1, s.onset_median_hops);
    long last = -gap_frames - 1;
    for (std::size_t k = 1; k + 1 < frames; ++k) {
        if (flux[k] <= flux[k - 1] || flux[k] < flux[k + 1]) continue;
        if (energy[k] <= floor_e) continue;
        const std::size_t lo = k > static_cast<std::size_t>(w) ? k - static_cast<std::size_t>(w) : 0;
        const std::size_t hi = std::min(frames, k + static_cast<std::size_t>(w) + 1);
        const double thr = s.onset_threshold_mult *
                               detail::median_of({flux.begin() + static_cast<long>(lo),
                                                  flux.begin() + static_cast<long>(hi)}) +
                           s.onset_threshold_delta;
        if (flux[k] <= thr) continue;
        if (static_cast<long>(k) - last < gap_frames) continue;
        last = static_cast<long>(k);
        out.times_ms.push_back(std::max(0.0, static_cast<double>(k) * s.envelope_hop_ms +
                                                 frame_offset_ms));
    }
    out.onsets = static_cast<int>(out.times_ms.size());
    const double seconds = static_cast<double>(x.size()) / s.sample_rate;
    if (seconds > 0.0) out.per_second = out.onsets / seconds;

    std::vector<double> intervals;
    for (std::size_t i = 1; i < out.times_ms.size(); ++i)
        intervals.push_back(out.times_ms[i] - out.times_ms[i - 1]);
    if (!intervals.empty()) {
        double sum = 0.0;
        for (double v : intervals) sum += v;
        out.interval_mean_ms = sum / static_cast<double>(intervals.size());
        out.interval_cv = detail::coefficient_of_variation(intervals);
        out.interval_trend = detail::normalized_trend(intervals);
    }

    // The pulse, asked independently of the intervals: autocorrelation of the
    // detection function itself. A rhythm whose events are unevenly SPACED but
    // land on a grid (a syncopated line) has a poor interval CV and a strong
    // autocorrelation, so the two numbers are not interchangeable and both are
    // reported.
    //
    // NOT ASKED AT ALL WITHOUT TWO ATTACKS TO BE PERIODIC ABOUT. A held tone's
    // detection function is flat to within arithmetic noise, and flat noise is
    // perfectly self-similar — so the autocorrelation of a drone reads 0.99 and
    // announces a pulse at whatever lag it happened to land on. There is no
    // rhythm in a signal with no attacks, and the honest report of "is there a
    // pulse" here is zero rather than a confident number about nothing.
    if (out.onsets < 2) return out;

    double mean_flux = 0.0;
    for (double v : flux) mean_flux += v;
    mean_flux /= static_cast<double>(frames);
    std::vector<double> centred(frames);
    for (std::size_t k = 0; k < frames; ++k) centred[k] = flux[k] - mean_flux;
    double zero = 0.0;
    for (double v : centred) zero += v * v;
    if (!(zero > 0.0)) return out;
    const std::size_t min_lag = static_cast<std::size_t>(
        std::max(1L, std::lround(s.onset_min_gap_ms / s.envelope_hop_ms)));
    const std::size_t max_lag = std::min(
        frames / 2, static_cast<std::size_t>(std::lround(s.max_period_ms / s.envelope_hop_ms)));
    for (std::size_t lag = min_lag; lag < max_lag; ++lag) {
        double acc = 0.0;
        for (std::size_t k = 0; k + lag < frames; ++k) acc += centred[k] * centred[k + lag];
        const double norm = acc / zero;
        if (norm > out.periodicity) {
            out.periodicity = norm;
            out.period_ms = static_cast<double>(lag) * s.envelope_hop_ms;
        }
    }
    if (out.periodicity < 0.0) { out.periodicity = 0.0; out.period_ms = 0.0; }
    return out;
}

/// Brightness over time — the amplitude-weighted mean frequency per window.
///
/// Silent windows are skipped rather than counted as zero. The centroid of
/// silence is not a low centroid, it is no centroid, and averaging it in turns
/// a rest into an apparent filter sweep.
inline SpectrumMeasure measure_spectrum(const std::vector<float>& x, const Settings& s) {
    SpectrumMeasure out;
    const long win = std::max(1L, std::lround(s.window_ms * s.sample_rate / 1000.0));
    const std::size_t windows = x.size() / static_cast<std::size_t>(win);
    int n = s.centroid_fft;
    while (n > 0 && static_cast<long>(n) > win) n /= 2;
    if (windows == 0 || n < 16) return out;

    pulp::signal::FftT<float> fft(n);
    std::vector<float> frame(static_cast<std::size_t>(n));
    std::vector<std::complex<float>> spec(static_cast<std::size_t>(n));
    std::vector<float> window_fn(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        window_fn[static_cast<std::size_t>(i)] = static_cast<float>(
            0.5 - 0.5 * std::cos(2.0 * M_PI * i / (n - 1)));

    // The same floor the dynamics use, so "active" means one thing in the whole
    // report rather than two things that nearly agree.
    double peak_rms = 0.0;
    std::vector<double> rms(windows, 0.0);
    for (std::size_t w = 0; w < windows; ++w) {
        double acc = 0.0;
        for (long i = 0; i < win; ++i) {
            const double v = x[w * static_cast<std::size_t>(win) + static_cast<std::size_t>(i)];
            acc += v * v;
        }
        rms[w] = std::sqrt(acc / static_cast<double>(win));
        peak_rms = std::max(peak_rms, rms[w]);
    }
    const double floor_v = std::max(s.active_floor_v, s.active_floor_ratio * peak_rms);

    out.centroid_hz.assign(windows, 0.0);
    std::vector<double> active;
    for (std::size_t w = 0; w < windows; ++w) {
        if (rms[w] <= floor_v) continue;
        for (int i = 0; i < n; ++i)
            frame[static_cast<std::size_t>(i)] =
                x[w * static_cast<std::size_t>(win) + static_cast<std::size_t>(i)] *
                window_fn[static_cast<std::size_t>(i)];
        fft.forward_real(frame.data(), spec.data());
        double num = 0.0, den = 0.0;
        for (int b = 1; b <= n / 2; ++b) {
            const double mag = std::abs(spec[static_cast<std::size_t>(b)]);
            num += mag * (static_cast<double>(b) * s.sample_rate / n);
            den += mag;
        }
        if (!(den > 0.0)) continue;
        out.centroid_hz[w] = num / den;
        active.push_back(out.centroid_hz[w]);
    }
    out.active_windows = static_cast<int>(active.size());
    if (active.empty()) return out;
    double sum = 0.0;
    for (double v : active) sum += v;
    out.centroid_mean_hz = sum / static_cast<double>(active.size());
    out.centroid_cv = detail::coefficient_of_variation(active);
    out.centroid_trend = detail::normalized_trend(active);
    const double lo = detail::percentile_of(active, 0.10);
    const double hi = detail::percentile_of(active, 0.90);
    if (lo > 0.0 && hi > 0.0) out.centroid_range_octaves = std::log2(hi / lo);
    return out;
}

/// Everything, on one recorded cable.
inline Behaviour measure(const std::vector<float>& x, const Settings& s,
                         const std::string& source = {}) {
    Behaviour b;
    b.settings = s;
    b.source = source;
    b.seconds = static_cast<double>(x.size()) / s.sample_rate;
    double sum = 0.0;
    for (float v : x) {
        if (!std::isfinite(v)) { b.finite = false; continue; }
        const double a = std::fabs(static_cast<double>(v));
        sum += a;
        b.peak_abs_v = std::max(b.peak_abs_v, a);
    }
    b.mean_abs_v = x.empty() ? 0.0 : sum / static_cast<double>(x.size());
    // A run with a NaN in it has no meaningful pitch or spectrum, and running
    // the estimators over it produces numbers that look like measurements.
    if (!b.finite) return b;
    b.pitch = measure_pitch(x, s);
    b.dynamics = measure_dynamics(x, s);
    b.onsets = measure_onsets(x, s);
    b.spectrum = measure_spectrum(x, s);
    return b;
}

}  // namespace patch_behaviour
