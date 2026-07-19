#include "modal_analysis.hpp"

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/buffer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>

namespace pulp::test::audio {
namespace {

constexpr double kPi = 3.14159265358979323846;
/// Linear magnitude floor, so a log of a decayed-to-zero envelope stays finite.
constexpr double kMagFloor = 1.0e-30;

int round_up_pow2(int v) {
    int n = 1;
    while (n < v && n < (1 << 24)) n <<= 1;
    return n;
}

std::string fmt(const char* spec, double a) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), spec, a);
    return buf;
}

bool all_finite(std::span<const float> x) {
    for (float v : x)
        if (!std::isfinite(v)) return false;
    return true;
}

double peak_abs(std::span<const float> x) {
    double p = 0.0;
    for (float v : x) p = std::max(p, std::abs(static_cast<double>(v)));
    return p;
}

/// Least-squares fit of y = slope * t + intercept. Returns false when t has
/// no spread (a vertical fit is not a decay rate).
bool linear_fit(const std::vector<double>& t, const std::vector<double>& y,
                double& slope, double& intercept, double& r2) {
    const auto n = static_cast<double>(t.size());
    if (t.size() < 2) return false;
    const double st = std::accumulate(t.begin(), t.end(), 0.0);
    const double sy = std::accumulate(y.begin(), y.end(), 0.0);
    double stt = 0.0, sty = 0.0;
    for (std::size_t i = 0; i < t.size(); ++i) {
        stt += t[i] * t[i];
        sty += t[i] * y[i];
    }
    const double denom = n * stt - st * st;
    if (std::abs(denom) < 1.0e-18) return false;
    slope = (n * sty - st * sy) / denom;
    intercept = (sy - slope * st) / n;

    const double mean_y = sy / n;
    double ss_res = 0.0, ss_tot = 0.0;
    for (std::size_t i = 0; i < t.size(); ++i) {
        const double pred = slope * t[i] + intercept;
        ss_res += (y[i] - pred) * (y[i] - pred);
        ss_tot += (y[i] - mean_y) * (y[i] - mean_y);
    }
    // A perfectly flat y has no variance to explain; call that a perfect fit
    // of a zero slope rather than 0/0.
    r2 = ss_tot > 1.0e-18 ? 1.0 - ss_res / ss_tot : 1.0;
    return true;
}

double pearson(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() < 3 || a.size() != b.size()) return 0.0;
    const auto n = static_cast<double>(a.size());
    const double ma = std::accumulate(a.begin(), a.end(), 0.0) / n;
    const double mb = std::accumulate(b.begin(), b.end(), 0.0) / n;
    double cov = 0.0, va = 0.0, vb = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double da = a[i] - ma, db = b[i] - mb;
        cov += da * db;
        va += da * da;
        vb += db * db;
    }
    if (va < 1.0e-18 || vb < 1.0e-18) return 0.0;
    return cov / std::sqrt(va * vb);
}

/// Sum of hann[i] * r^i over a window of `length` — the gain the windowed DFT
/// probe applies to a mode decaying at per-sample ratio `r`. Dividing it out
/// is what turns a windowed magnitude back into the mode's own amplitude.
double window_decay_gain(std::size_t length, double r) {
    if (length == 0) return 0.0;
    if (length == 1) return 1.0;
    double s = 0.0, rn = 1.0;
    for (std::size_t i = 0; i < length; ++i) {
        const double hann =
            0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) /
                                  static_cast<double>(length - 1)));
        s += hann * rn;
        rn *= r;
    }
    return s;
}

/// Refine `guess_hz` by scanning the windowed magnitude on a cents grid and
/// parabolically interpolating the peak.
double refine_frequency(std::span<const float> x, double guess_hz,
                        double sample_rate, std::size_t window,
                        double span_cents, double step_cents) {
    if (step_cents <= 0.0 || span_cents <= 0.0) return guess_hz;
    const int steps = static_cast<int>(2.0 * span_cents / step_cents) + 1;
    std::vector<double> mags(static_cast<std::size_t>(steps));
    double best_mag = -1.0, best_cents = 0.0;
    int best_idx = 0;
    for (int k = 0; k < steps; ++k) {
        const double cents = -span_cents + step_cents * k;
        const double f = guess_hz * std::pow(2.0, cents / 1200.0);
        mags[static_cast<std::size_t>(k)] =
            windowed_magnitude(x, 0, window, f, sample_rate);
        if (mags[static_cast<std::size_t>(k)] > best_mag) {
            best_mag = mags[static_cast<std::size_t>(k)];
            best_cents = cents;
            best_idx = k;
        }
    }
    if (best_idx > 0 && best_idx < steps - 1) {
        const double m0 = mags[static_cast<std::size_t>(best_idx - 1)];
        const double m1 = mags[static_cast<std::size_t>(best_idx)];
        const double m2 = mags[static_cast<std::size_t>(best_idx + 1)];
        const double denom = m0 - 2.0 * m1 + m2;
        if (std::abs(denom) > 1.0e-15)
            best_cents += step_cents * 0.5 * (m0 - m2) / denom;
    }
    return guess_hz * std::pow(2.0, best_cents / 1200.0);
}

/// Fit the decay of the mode at `freq_hz` and fill in t60/amplitude/confidence.
void fit_decay(std::span<const float> x, double sample_rate, double freq_hz,
               const ModeAnalysisOptions& o, MeasuredMode& mode) {
    const auto window = static_cast<std::size_t>(std::max(o.envelope_window, 16));
    const auto hop = static_cast<std::size_t>(std::max(o.envelope_hop, 1));
    const auto first = static_cast<std::size_t>(
        std::max(0.0, o.fit_offset_s * sample_rate));

    std::vector<std::size_t> starts;
    std::vector<double> times, dbs, mags;
    for (std::size_t s = first; s + window <= x.size(); s += hop) {
        const double m = windowed_magnitude(x, s, window, freq_hz, sample_rate);
        starts.push_back(s);
        mags.push_back(m);
        times.push_back((static_cast<double>(s) + static_cast<double>(window) * 0.5) /
                        sample_rate);
        dbs.push_back(20.0 * std::log10(std::max(m, kMagFloor)));
    }
    if (dbs.size() < 2) {
        mode.note = "buffer shorter than one envelope window; no decay fit";
        return;
    }

    // Walk the envelope once, in order: skip the head above fit_start_db, then
    // collect until it drops past fit_end_db and stop there. Never search past
    // the window for a -60 dB crossing — below the fit window the envelope is
    // the noise floor (or a neighbouring mode's tail) and a detector that
    // chases it reports a confident, wrong, typically flat T60.
    const double ref_db = dbs.front();
    const double hi = ref_db + o.fit_start_db;
    const double lo = ref_db + o.fit_end_db;
    std::vector<double> ft, fy;
    std::size_t fit_first_start = 0;
    bool started = false, reached_floor = false;
    for (std::size_t i = 0; i < dbs.size(); ++i) {
        if (!started) {
            if (dbs[i] > hi) continue;
            started = true;
            fit_first_start = starts[i];
        }
        if (dbs[i] < lo) {
            reached_floor = true;
            break;
        }
        ft.push_back(times[i]);
        fy.push_back(dbs[i]);
    }

    mode.fit_points = static_cast<int>(ft.size());
    if (mode.fit_points < o.min_fit_points) {
        mode.note = "fit window (" + fmt("%.0f", o.fit_start_db) + " to " +
                    fmt("%.0f", o.fit_end_db) + " dB) yielded only " +
                    std::to_string(mode.fit_points) + " points; buffer too " +
                    "short or decay too fast for this envelope hop";
        return;
    }
    if (!reached_floor)
        mode.note = "decay never reached " + fmt("%.0f", o.fit_end_db) +
                    " dB within the buffer; T60 extrapolated from a partial window";

    double slope = 0.0, intercept = 0.0, r2 = 0.0;
    if (!linear_fit(ft, fy, slope, intercept, r2)) {
        mode.note = "degenerate envelope fit";
        return;
    }
    if (slope >= 0.0) {
        mode.note = "envelope does not decay (fitted slope " +
                    fmt("%.2f", slope) + " dB/s); not an exponential mode";
        return;
    }

    mode.t60_s = -60.0 / slope;
    mode.confidence = std::clamp(r2, 0.0, 1.0);

    // Amplitude: invert the probe. For y[n] = A r^n sin((n+1)w), the
    // Hann-windowed magnitude at f over a window starting at s0 is
    // (A/2) * r^s0 * sum_i hann[i] r^i. Solve for A and carry it back to n = 0.
    const double r = std::pow(10.0, slope / (20.0 * sample_rate));
    const double gain = window_decay_gain(window, r);
    const double m0 = windowed_magnitude(x, fit_first_start, window, freq_hz,
                                         sample_rate);
    if (gain > 0.0 && m0 > kMagFloor) {
        const double log_a = std::log(2.0 * m0 / gain) -
                             static_cast<double>(fit_first_start) * std::log(r);
        mode.amplitude = std::isfinite(log_a) && log_a < 700.0 ? std::exp(log_a)
                                                               : 0.0;
        if (!std::isfinite(mode.amplitude)) {
            mode.amplitude = 0.0;
            mode.note = "amplitude back-extrapolation overflowed";
        }
    }
}

} // namespace

// ── Primitives ────────────────────────────────────────────────────────────

double windowed_magnitude(std::span<const float> x, std::size_t start,
                          std::size_t length, double freq_hz,
                          double sample_rate) {
    if (start >= x.size() || length == 0 || sample_rate <= 0.0) return 0.0;
    length = std::min(length, x.size() - start);
    if (length < 2) return std::abs(static_cast<double>(x[start]));
    double re = 0.0, im = 0.0;
    const double w = 2.0 * kPi * freq_hz / sample_rate;
    for (std::size_t i = 0; i < length; ++i) {
        const double hann =
            0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) /
                                  static_cast<double>(length - 1)));
        const double v = hann * static_cast<double>(x[start + i]);
        const double phase = w * static_cast<double>(start + i);
        re += v * std::cos(phase);
        im -= v * std::sin(phase);
    }
    return std::sqrt(re * re + im * im);
}

// ── Modes ─────────────────────────────────────────────────────────────────

MeasuredMode measure_mode(std::span<const float> ir, double sample_rate,
                          double guess_hz, const ModeAnalysisOptions& options) {
    MeasuredMode mode;
    if (ir.empty() || sample_rate <= 0.0 || guess_hz <= 0.0) {
        mode.note = "empty buffer or non-positive sample rate / guess";
        return mode;
    }
    if (!all_finite(ir)) {
        mode.note = "buffer contains NaN/Inf";
        return mode;
    }
    const auto refine_window =
        static_cast<std::size_t>(std::max(options.refine_window, 64));
    mode.freq_hz =
        refine_frequency(ir, guess_hz, sample_rate, refine_window,
                         options.refine_span_cents, options.refine_step_cents);
    fit_decay(ir, sample_rate, mode.freq_hz, options, mode);
    return mode;
}

ModeAnalysis analyze_modes(std::span<const float> ir, double sample_rate,
                           const ModeAnalysisOptions& options) {
    ModeAnalysis result;
    result.sample_rate = sample_rate;

    if (ir.empty() || sample_rate <= 0.0) {
        result.message = "empty buffer or non-positive sample rate";
        return result;
    }
    if (!all_finite(ir)) {
        result.message = "buffer contains NaN/Inf; refusing to analyze";
        return result;
    }
    if (peak_abs(ir) <= 0.0) {
        result.message = "buffer is all zeros; no modes to find";
        return result;
    }

    const int n_fft = round_up_pow2(std::max(options.fft_length, 256));
    result.bin_hz = sample_rate / n_fft;
    // Hann's main lobe spans 4 bins: two peaks closer than that are one peak.
    result.resolution_limit_hz = 4.0 * result.bin_hz;

    const float* ptrs[1] = {ir.data()};
    const pulp::audio::BufferView<const float> view(ptrs, 1, ir.size());
    ResponseOptions spec_opts;
    spec_opts.fft_length = n_fft;
    spec_opts.window = Window::hann;
    spec_opts.channel = 0;
    const auto curve =
        magnitude_spectrum_curve(view, sample_rate, {}, spec_opts);

    // Nominate local maxima inside the search band and above the floor. The
    // curve is already normalized to its own peak bin, so peak_floor_db is
    // relative to the strongest peak by construction.
    struct Candidate {
        double hz;
        double db;
    };
    std::vector<Candidate> candidates;
    for (std::size_t i = 1; i + 1 < curve.full.size(); ++i) {
        const auto& p = curve.full[i];
        if (p.hz < options.search_low_hz || p.hz > options.search_high_hz) continue;
        if (p.magnitude_db < options.peak_floor_db) continue;
        if (p.magnitude_db <= curve.full[i - 1].magnitude_db) continue;
        if (p.magnitude_db < curve.full[i + 1].magnitude_db) continue;
        candidates.push_back({p.hz, p.magnitude_db});
    }
    result.discovered_peaks = static_cast<int>(candidates.size());
    if (candidates.empty()) {
        result.message = "no spectral peak above " +
                         fmt("%.0f", options.peak_floor_db) +
                         " dB in the search band";
        return result;
    }

    // Strongest first, then greedily reject anything within the separation
    // guard of an already-accepted peak: a merged pair reported as two modes
    // would be two wrong frequencies instead of one honest one.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.db > b.db; });
    std::vector<Candidate> accepted;
    const double sep = std::max(options.min_separation_hz, result.resolution_limit_hz);
    for (const auto& c : candidates) {
        if (static_cast<int>(accepted.size()) >= std::max(options.max_modes, 1)) break;
        const bool clash =
            std::any_of(accepted.begin(), accepted.end(), [&](const Candidate& a) {
                return std::abs(a.hz - c.hz) < sep;
            });
        if (!clash) accepted.push_back(c);
    }

    const auto refine_window =
        static_cast<std::size_t>(std::max(options.refine_window, 64));
    for (const auto& c : accepted) {
        MeasuredMode mode;
        mode.prominence_db = c.db;
        mode.freq_hz =
            refine_frequency(ir, c.hz, sample_rate, refine_window,
                             options.refine_span_cents, options.refine_step_cents);
        fit_decay(ir, sample_rate, mode.freq_hz, options, mode);
        result.modes.push_back(std::move(mode));
    }

    std::sort(result.modes.begin(), result.modes.end(),
              [](const MeasuredMode& a, const MeasuredMode& b) {
                  return a.amplitude > b.amplitude;
              });

    result.ok = true;
    result.message = std::to_string(result.modes.size()) + " modes from " +
                     std::to_string(result.discovered_peaks) + " peaks; bin " +
                     fmt("%.2f", result.bin_hz) + " Hz, cannot separate closer than " +
                     fmt("%.1f", result.resolution_limit_hz) + " Hz";
    return result;
}

std::string summarize(const MeasuredMode& mode) {
    std::string s = fmt("%.2f", mode.freq_hz) + " Hz  T60 " +
                    fmt("%.4f", mode.t60_s) + " s  amp " +
                    fmt("%.4f", mode.amplitude) + "  conf " +
                    fmt("%.3f", mode.confidence) + "  prom " +
                    fmt("%.1f", mode.prominence_db) + " dB  (" +
                    std::to_string(mode.fit_points) + " pts)";
    if (!mode.note.empty()) s += "  [" + mode.note + "]";
    return s;
}

std::string summarize(const ModeAnalysis& analysis) {
    std::string s = "modal analysis: " + analysis.message + "\n";
    for (const auto& m : analysis.modes) s += "  " + summarize(m) + "\n";
    return s;
}

// ── Partials and inharmonicity ────────────────────────────────────────────

InharmonicityResult measure_inharmonicity(std::span<const float> signal,
                                          double sample_rate, double f0_guess,
                                          const InharmonicityOptions& options) {
    InharmonicityResult result;
    if (signal.empty() || sample_rate <= 0.0 || f0_guess <= 0.0) {
        result.message = "empty buffer or non-positive sample rate / f0 guess";
        return result;
    }
    if (!all_finite(signal)) {
        result.message = "buffer contains NaN/Inf; refusing to analyze";
        return result;
    }
    const int num = std::max(options.num_partials, 1);
    const auto& mo = options.mode_options;
    const auto refine_window =
        static_cast<std::size_t>(std::max(mo.refine_window, 64));

    const MeasuredMode fundamental =
        measure_mode(signal, sample_rate, f0_guess, mo);
    result.f0_hz = fundamental.freq_hz;
    if (result.f0_hz <= 0.0) {
        result.message = "could not refine the fundamental near " +
                         fmt("%.2f", f0_guess) + " Hz";
        return result;
    }

    // Search each partial around the stiff-string prediction using the B fit
    // from the partials found so far. Predicting with B = 0 throughout would
    // lose the high partials of a stiff string, which are exactly the ones
    // that carry the inharmonicity.
    double b = 0.0;
    std::vector<double> xs, ys; // n^2 and (f_n/(n f0))^2 - 1, for the B fit.
    double loudest = 0.0;

    for (int n = 1; n <= num; ++n) {
        MeasuredPartial p;
        p.index = n;
        p.harmonic_hz = n * result.f0_hz;
        const double predicted =
            n * result.f0_hz * std::sqrt(std::max(1.0 + b * n * n, 1.0e-6));
        if (predicted >= 0.5 * sample_rate) {
            p.found = false;
            result.partials.push_back(p);
            continue;
        }
        if (n == 1) {
            p.freq_hz = result.f0_hz;
            p.amplitude = fundamental.amplitude;
            p.found = true;
        } else {
            p.freq_hz = refine_frequency(signal, predicted, sample_rate,
                                         refine_window, options.search_span_cents,
                                         mo.refine_step_cents);
            MeasuredMode m;
            m.freq_hz = p.freq_hz;
            fit_decay(signal, sample_rate, p.freq_hz, mo, m);
            p.amplitude = m.amplitude;
            p.found = true;
        }
        loudest = std::max(loudest, p.amplitude);
        p.deviation_cents = 1200.0 * std::log2(p.freq_hz / p.harmonic_hz);
        result.partials.push_back(p);

        if (n >= 2) {
            const double ratio = p.freq_hz / (n * result.f0_hz);
            xs.push_back(static_cast<double>(n) * n);
            ys.push_back(ratio * ratio - 1.0);
            double sxx = 0.0, sxy = 0.0;
            for (std::size_t i = 0; i < xs.size(); ++i) {
                sxx += xs[i] * xs[i];
                sxy += xs[i] * ys[i];
            }
            if (sxx > 1.0e-18) b = sxy / sxx;
        }
    }

    // Second pass on amplitude: a "partial" that is 60 dB below the loudest is
    // the analyzer refining a noise peak, not a partial. Drop those before the
    // deviation statistics, so the fit is not steered by frequencies that are
    // whatever the noise floor happened to peak at.
    const double floor_lin =
        loudest * std::pow(10.0, options.partial_floor_db / 20.0);
    for (auto& p : result.partials)
        if (p.found && p.index > 1 && p.amplitude < floor_lin) p.found = false;

    // Refit B over only the surviving partials.
    xs.clear();
    ys.clear();
    for (const auto& p : result.partials) {
        if (!p.found || p.index < 2) continue;
        const double ratio = p.freq_hz / (p.index * result.f0_hz);
        xs.push_back(static_cast<double>(p.index) * p.index);
        ys.push_back(ratio * ratio - 1.0);
    }
    double sxx = 0.0, sxy = 0.0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        sxx += xs[i] * xs[i];
        sxy += xs[i] * ys[i];
    }
    result.b_coefficient = sxx > 1.0e-18 ? sxy / sxx : 0.0;

    double ss_stiff = 0.0, ss_harm = 0.0;
    int count = 0;
    for (const auto& p : result.partials) {
        if (!p.found) continue;
        ++count;
        const double stiff_hz =
            p.index * result.f0_hz *
            std::sqrt(std::max(1.0 + result.b_coefficient * p.index * p.index, 1.0e-6));
        const double d_stiff = 1200.0 * std::log2(p.freq_hz / stiff_hz);
        ss_stiff += d_stiff * d_stiff;
        ss_harm += p.deviation_cents * p.deviation_cents;
    }
    result.found_partials = count;
    if (count > 0) {
        result.rms_deviation_cents = std::sqrt(ss_stiff / count);
        result.rms_harmonic_deviation_cents = std::sqrt(ss_harm / count);
    }
    result.ok = count >= 2;
    result.message =
        result.ok
            ? std::to_string(count) + "/" + std::to_string(num) +
                  " partials found; f0 " + fmt("%.3f", result.f0_hz) +
                  " Hz, B = " + fmt("%.3e", result.b_coefficient) +
                  ", stiff-string residual " + fmt("%.2f", result.rms_deviation_cents) +
                  " cents (vs " + fmt("%.2f", result.rms_harmonic_deviation_cents) +
                  " cents for a pure harmonic series)"
            : "fewer than 2 partials found above the floor; inharmonicity "
              "undefined";
    return result;
}

std::string summarize(const InharmonicityResult& result) {
    std::string s = "inharmonicity: " + result.message + "\n";
    for (const auto& p : result.partials) {
        if (!p.found) {
            s += "  n=" + std::to_string(p.index) + "  (not found)\n";
            continue;
        }
        s += "  n=" + std::to_string(p.index) + "  " + fmt("%.2f", p.freq_hz) +
             " Hz  vs " + fmt("%.2f", p.harmonic_hz) + " Hz  " +
             fmt("%+.2f", p.deviation_cents) + " cents  amp " +
             fmt("%.4f", p.amplitude) + "\n";
    }
    return s;
}

// ── Time-resolved f0(t), sigma(t), Q(t) ───────────────────────────────────

namespace {

/// Mean of a projection over the valid cycles inside [from, to).
double window_mean(const std::vector<CycleObservation>& cycles, double from,
                   double to, double (*pick)(const CycleObservation&)) {
    double sum = 0.0;
    int n = 0;
    for (const auto& c : cycles) {
        if (!c.sigma_valid || c.time_s < from || c.time_s >= to) continue;
        sum += pick(c);
        ++n;
    }
    return n > 0 ? sum / n : 0.0;
}

double window_span_percent(const std::vector<CycleObservation>& cycles,
                           double from, double to,
                           double (*pick)(const CycleObservation&)) {
    double lo = std::numeric_limits<double>::max();
    double hi = std::numeric_limits<double>::lowest();
    double sum = 0.0;
    int n = 0;
    for (const auto& c : cycles) {
        if (!c.sigma_valid || c.time_s < from || c.time_s >= to) continue;
        const double v = pick(c);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        sum += v;
        ++n;
    }
    if (n < 2) return 0.0;
    const double mean = sum / n;
    if (std::abs(mean) < 1.0e-18) return 0.0;
    return (hi - lo) / mean * 100.0;
}

} // namespace

double CycleTrack::mean_f0(double from_s, double to_s) const {
    return window_mean(cycles, from_s, to_s,
                       [](const CycleObservation& c) { return c.freq_hz; });
}
double CycleTrack::mean_sigma(double from_s, double to_s) const {
    return window_mean(cycles, from_s, to_s,
                       [](const CycleObservation& c) { return c.sigma_np_s; });
}
double CycleTrack::mean_q(double from_s, double to_s) const {
    return window_mean(cycles, from_s, to_s,
                       [](const CycleObservation& c) { return c.q; });
}
double CycleTrack::sigma_span_percent(double from_s, double to_s) const {
    return window_span_percent(cycles, from_s, to_s,
                               [](const CycleObservation& c) { return c.sigma_np_s; });
}
double CycleTrack::f0_span_percent(double from_s, double to_s) const {
    return window_span_percent(cycles, from_s, to_s,
                               [](const CycleObservation& c) { return c.freq_hz; });
}
double CycleTrack::f0_q_correlation(double from_s, double to_s) const {
    std::vector<double> f, q;
    for (const auto& c : cycles) {
        if (!c.sigma_valid || c.time_s < from_s || c.time_s >= to_s) continue;
        f.push_back(c.freq_hz);
        q.push_back(c.q);
    }
    return pearson(f, q);
}

CycleTrack track_cycles(std::span<const float> signal, double sample_rate,
                        const CycleTrackOptions& options) {
    CycleTrack track;
    track.sample_rate = sample_rate;
    if (signal.empty() || sample_rate <= 0.0) {
        track.message = "empty buffer or non-positive sample rate";
        return track;
    }
    if (!all_finite(signal)) {
        track.message = "buffer contains NaN/Inf; refusing to analyze";
        return track;
    }

    const double peak = peak_abs(signal);
    if (peak <= 0.0) {
        track.message = "buffer is all zeros";
        return track;
    }

    std::size_t onset = 0;
    while (onset < signal.size() &&
           std::abs(static_cast<double>(signal[onset])) <= options.onset_threshold)
        ++onset;
    if (onset >= signal.size()) {
        track.message = "no sample exceeds the onset threshold " +
                        fmt("%.2e", options.onset_threshold);
        return track;
    }
    track.onset_s = static_cast<double>(onset) / sample_rate;

    const double tail_floor = peak * options.tail_floor_ratio;
    std::size_t last = signal.size();
    while (last > onset &&
           std::abs(static_cast<double>(signal[last - 1])) <= tail_floor)
        --last;

    const auto start = static_cast<std::size_t>(
        (track.onset_s + options.start_offset_s) * sample_rate);
    if (start + 2 >= last) {
        track.message = "usable tail is shorter than start_offset_s (" +
                        fmt("%.3f", options.start_offset_s) + " s)";
        return track;
    }

    // Positive-going zero crossings with sub-sample linear interpolation.
    std::vector<double> zc;
    for (std::size_t i = start; i + 1 < last; ++i) {
        const double a = signal[i], b = signal[i + 1];
        if (!(a < 0.0 && b >= 0.0)) continue;
        const double denom = b - a;
        const double frac = std::abs(denom) > 1.0e-30 ? -a / denom : 0.0;
        zc.push_back((static_cast<double>(i) + frac) / sample_rate);
    }
    if (zc.size() < 2) {
        track.message = "fewer than two zero crossings in the tracked region; "
                        "not a quasi-sinusoidal note";
        return track;
    }

    for (std::size_t i = 0; i + 1 < zc.size(); ++i) {
        const double a = zc[i], b = zc[i + 1];
        const double period = b - a;
        if (period <= 0.0) continue;
        CycleObservation c;
        c.time_s = 0.5 * (a + b);
        c.freq_hz = 1.0 / period;
        const auto ia = static_cast<std::size_t>(a * sample_rate);
        const auto ib = std::min(static_cast<std::size_t>(b * sample_rate),
                                 signal.size());
        double env = 0.0;
        for (std::size_t k = ia; k < ib; ++k)
            env = std::max(env, std::abs(static_cast<double>(signal[k])));
        c.envelope = env;
        c.directional_only = (i == 0);
        track.cycles.push_back(c);
    }
    if (static_cast<int>(track.cycles.size()) < options.min_cycles) {
        track.message = "only " + std::to_string(track.cycles.size()) +
                        " cycles tracked (min " + std::to_string(options.min_cycles) +
                        ")";
        return track;
    }

    // Sliding least-squares fit of the log envelope over 2*halfwin+1 cycles.
    // sigma is its negative slope; the ends have no full window and are left
    // invalid rather than fit over a lopsided one.
    const int hw = std::max(options.sigma_half_window, 1);
    const int n = static_cast<int>(track.cycles.size());
    for (int i = hw; i < n - hw; ++i) {
        std::vector<double> t, y;
        for (int k = i - hw; k <= i + hw; ++k) {
            t.push_back(track.cycles[static_cast<std::size_t>(k)].time_s);
            y.push_back(std::log(std::max(
                track.cycles[static_cast<std::size_t>(k)].envelope, 1.0e-12)));
        }
        double slope = 0.0, intercept = 0.0, r2 = 0.0;
        if (!linear_fit(t, y, slope, intercept, r2)) continue;
        auto& c = track.cycles[static_cast<std::size_t>(i)];
        c.sigma_np_s = -slope;
        c.q = std::abs(c.sigma_np_s) > 1.0e-12 ? kPi * c.freq_hz / c.sigma_np_s : 0.0;
        c.sigma_valid = true;
    }

    // Monocomponent check: a zero-crossing tracker following a single mode
    // produces near-constant periods. Wildly varying periods mean it is
    // hopping between components and none of the numbers mean anything.
    std::vector<double> periods;
    for (const auto& c : track.cycles)
        if (c.freq_hz > 0.0) periods.push_back(1.0 / c.freq_hz);
    if (!periods.empty()) {
        auto sorted = periods;
        std::sort(sorted.begin(), sorted.end());
        const double median = sorted[sorted.size() / 2];
        int within = 0;
        for (double p : periods)
            if (std::abs(p - median) <= 0.2 * median) ++within;
        track.monocomponent_confidence =
            static_cast<double>(within) / static_cast<double>(periods.size());
    }

    const int valid = static_cast<int>(std::count_if(
        track.cycles.begin(), track.cycles.end(),
        [](const CycleObservation& c) { return c.sigma_valid; }));
    track.ok = valid > 0;
    track.message = std::to_string(track.cycles.size()) + " cycles (" +
                    std::to_string(valid) + " with sigma), onset " +
                    fmt("%.1f", track.onset_s * 1000.0) + " ms, monocomponent " +
                    fmt("%.2f", track.monocomponent_confidence);
    if (!track.ok)
        track.message += "; no cycle had a full sigma window";
    return track;
}

Ar2Track track_ar2(std::span<const float> signal, double sample_rate,
                   const Ar2TrackOptions& options) {
    Ar2Track track;
    if (signal.empty() || sample_rate <= 0.0) {
        track.message = "empty buffer or non-positive sample rate";
        return track;
    }
    if (!all_finite(signal)) {
        track.message = "buffer contains NaN/Inf; refusing to analyze";
        return track;
    }

    std::size_t onset = 0;
    while (onset < signal.size() &&
           std::abs(static_cast<double>(signal[onset])) <= options.onset_threshold)
        ++onset;
    if (onset >= signal.size()) {
        track.message = "no sample exceeds the onset threshold";
        return track;
    }

    const auto win = static_cast<std::size_t>(options.window_s * sample_rate);
    const auto hop = static_cast<std::size_t>(options.hop_s * sample_rate);
    if (win < 8 || hop < 1) {
        track.message = "window/hop too short for an AR(2) fit";
        return track;
    }

    for (std::size_t s = onset + static_cast<std::size_t>(options.start_offset_s *
                                                          sample_rate);
         s + win <= signal.size(); s += hop) {
        const auto seg = signal.subspan(s, win);
        if (peak_abs(seg) < options.silence_threshold) break;

        // Normal equations for x[n] = a1 x[n-1] + a2 x[n-2].
        double r11 = 0, r12 = 0, r22 = 0, p1 = 0, p2 = 0, energy = 0;
        for (std::size_t i = 2; i < win; ++i) {
            const double y = seg[i], x1 = seg[i - 1], x2 = seg[i - 2];
            r11 += x1 * x1;
            r12 += x1 * x2;
            r22 += x2 * x2;
            p1 += x1 * y;
            p2 += x2 * y;
            energy += y * y;
        }
        const double det = r11 * r22 - r12 * r12;
        if (std::abs(det) < 1.0e-24 || energy < 1.0e-24) continue;
        const double a1 = (p1 * r22 - p2 * r12) / det;
        const double a2 = (p2 * r11 - p1 * r12) / det;

        // Complex pole pair required: a real pair is not a resonance.
        const double disc = a1 * a1 + 4.0 * a2;
        if (disc >= 0.0) continue;
        const double r = std::sqrt(std::max(-a2, 0.0));
        if (!(r > 0.0) || r >= 1.0) continue;
        const double theta = std::acos(std::clamp(a1 / (2.0 * r), -1.0, 1.0));

        Ar2Observation o;
        o.time_s = (static_cast<double>(s) + static_cast<double>(win) * 0.5) /
                   sample_rate;
        o.freq_hz = theta * sample_rate / (2.0 * kPi);
        o.sigma_np_s = -std::log(r) * sample_rate;
        o.q = o.sigma_np_s > 1.0e-12 ? kPi * o.freq_hz / o.sigma_np_s : 0.0;

        double residual = 0.0;
        for (std::size_t i = 2; i < win; ++i) {
            const double pred = a1 * seg[i - 1] + a2 * seg[i - 2];
            const double e = seg[i] - pred;
            residual += e * e;
        }
        o.residual_ratio = residual / energy;
        track.windows.push_back(o);
    }

    track.ok = !track.windows.empty();
    track.message = track.ok
                        ? std::to_string(track.windows.size()) +
                              " AR(2) windows with a complex pole pair"
                        : "no window yielded a complex pole pair; the signal is "
                          "not a decaying resonance in this region";
    return track;
}

std::string summarize(const CycleTrack& track) {
    std::string s = "cycle track: " + track.message + "\n";
    s += "    t(ms)     f0(Hz)   sigma(Np/s)        Q\n";
    for (const auto& c : track.cycles) {
        if (!c.sigma_valid) continue;
        s += "  " + fmt("%7.1f", c.time_s * 1000.0) + "  " +
             fmt("%9.2f", c.freq_hz) + "  " + fmt("%12.2f", c.sigma_np_s) + "  " +
             fmt("%8.2f", c.q) + (c.directional_only ? "  (directional)" : "") + "\n";
    }
    return s;
}

std::string summarize(const Ar2Track& track) {
    std::string s = "AR(2) track: " + track.message + "\n";
    for (const auto& o : track.windows)
        s += "  " + fmt("%7.1f", o.time_s * 1000.0) + " ms  " +
             fmt("%8.2f", o.freq_hz) + " Hz  sigma " + fmt("%7.2f", o.sigma_np_s) +
             "  Q " + fmt("%7.2f", o.q) + "  resid " +
             fmt("%.2e", o.residual_ratio) + "\n";
    return s;
}

} // namespace pulp::test::audio
