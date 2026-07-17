/// @file pitch_track.cpp
/// Implementation of the fundamental estimator and f0(t) trajectory extractor.
/// See pitch_track.hpp for the method and the fail-closed contract.

#include "pulp/audio/analysis/pitch_track.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace pulp::test::audio {
namespace {

void require(bool condition, const char* message) {
    if (!condition)
        throw std::invalid_argument(message);
}

/// Largest power of two <= n (>= 1 for n >= 1).
int floor_pow2(int n) {
    int p = 1;
    while ((p << 1) > 0 && (p << 1) <= n)
        p <<= 1;
    return p;
}

/// Fitted energy of a single sinusoid at `hz` over the (mean-removed) segment.
/// Zero outside the open (0, Nyquist) band where `fit_tone` is well posed —
/// keeps the search from straying onto the degenerate DC/Nyquist basis.
double fitted_energy_at(std::span<const double> seg, double hz,
                        double sample_rate) {
    const double cps = hz / sample_rate;
    if (!(cps > 0.0 && cps < 0.5))
        return 0.0;
    return fit_tone(seg, cps).fitted_energy;
}

/// Fitted amplitude of a single sinusoid at `hz`; 0 outside the open band.
double fitted_amplitude_at(std::span<const double> seg, double hz,
                           double sample_rate) {
    const double cps = hz / sample_rate;
    if (!(cps > 0.0 && cps < 0.5))
        return 0.0;
    return fit_tone(seg, cps).amplitude;
}

/// Golden-section maximisation of the single-tone fitted energy over [lo, hi].
/// The fitted energy is unimodal within one FFT bin of the true fundamental
/// (the coarse stage guarantees that bracket), so a golden search converges to
/// the peak without derivatives. Terminates when the bracket is far tighter
/// than any cent-level tolerance.
double golden_max_frequency(std::span<const double> seg, double lo, double hi,
                            double sample_rate, int max_iterations) {
    const double inv_phi = (std::sqrt(5.0) - 1.0) / 2.0;   // 0.6180339887…
    const double inv_phi2 = (3.0 - std::sqrt(5.0)) / 2.0;  // 0.3819660112…
    double a = lo, b = hi;
    double c = a + inv_phi2 * (b - a);
    double d = a + inv_phi * (b - a);
    double fc = fitted_energy_at(seg, c, sample_rate);
    double fd = fitted_energy_at(seg, d, sample_rate);
    for (int i = 0; i < max_iterations && (b - a) > 1.0e-7; ++i) {
        if (fc > fd) {
            b = d;
            d = c;
            fd = fc;
            c = a + inv_phi2 * (b - a);
            fc = fitted_energy_at(seg, c, sample_rate);
        } else {
            a = c;
            c = d;
            fc = fd;
            d = a + inv_phi * (b - a);
            fd = fitted_energy_at(seg, d, sample_rate);
        }
    }
    return 0.5 * (a + b);
}

} // namespace

double cents_between(double hz, double reference_hz) {
    require(hz > 0.0 && reference_hz > 0.0,
            "cents_between requires two positive frequencies");
    return 1200.0 * std::log2(hz / reference_hz);
}

PitchEstimate estimate_pitch(std::span<const float> samples, double sample_rate,
                             const PitchOptions& options) {
    require(sample_rate > 0.0, "sample_rate must be > 0");
    require(options.analysis_offset >= 0, "analysis_offset must be >= 0");

    const int total = static_cast<int>(samples.size());
    const int offset = options.analysis_offset;
    const int length =
        options.analysis_length > 0 ? options.analysis_length : total - offset;
    require(length > 0 && offset + length <= total,
            "analysis window must lie within the signal");
    require(length >= 64, "analysis window too short for a pitch estimate");

    const auto seg_f = samples.subspan(static_cast<std::size_t>(offset),
                                       static_cast<std::size_t>(length));

    // Mean-removed double copy for the projection stage; total energy drives the
    // silence gate and the confidence ratio. The FFT stage removes DC itself, so
    // it reads the original float samples.
    std::vector<double> seg(static_cast<std::size_t>(length));
    double mean = 0.0;
    for (float v : seg_f)
        mean += v;
    mean /= static_cast<double>(length);
    double total_energy = 0.0;
    for (int i = 0; i < length; ++i) {
        const double s = static_cast<double>(seg_f[static_cast<std::size_t>(i)]) - mean;
        seg[static_cast<std::size_t>(i)] = s;
        total_energy += s * s;
    }
    const double rms = std::sqrt(total_energy / static_cast<double>(length));

    PitchEstimate est;

    // ── Coarse: loudest non-DC bin, parabolic-interpolated in the dB domain. ──
    const int fft_length = floor_pow2(std::min(options.fft_length, length));
    require(fft_length >= 8, "analysis window too short for the coarse FFT");
    const double bin_hz = sample_rate / static_cast<double>(fft_length);
    const double lo_band = std::max(options.min_hz, bin_hz);
    const double hi_band =
        options.max_hz > 0.0 ? options.max_hz : 0.45 * sample_rate;

    const float* channel_ptr = seg_f.data();
    const pulp::audio::BufferView<const float> view(&channel_ptr, 1,
                                                    static_cast<std::size_t>(length));
    ResponseOptions ropts;
    ropts.fft_length = fft_length;
    ropts.analysis_offset = 0;
    ropts.window = options.window;
    ropts.kaiser_beta = options.kaiser_beta;
    ropts.channel = 0;
    const ResponseCurve curve =
        magnitude_spectrum_curve(view, sample_rate, {}, ropts);

    const int bins = static_cast<int>(curve.full.size());
    int peak_bin = -1;
    double peak_db = kSilenceFloorDb;
    for (int i = 1; i < bins - 1; ++i) {
        const double hz = curve.full[static_cast<std::size_t>(i)].hz;
        if (hz < lo_band || hz > hi_band)
            continue;
        const double db = curve.full[static_cast<std::size_t>(i)].magnitude_db;
        if (db > peak_db) {
            peak_db = db;
            peak_bin = i;
        }
    }

    if (peak_bin < 0) {
        // No bin inside the search band — nothing to estimate.
        est.voiced = false;
        return est;
    }

    // Parabolic (quadratic) peak interpolation in the log-magnitude domain.
    const double y0 = curve.full[static_cast<std::size_t>(peak_bin - 1)].magnitude_db;
    const double y1 = curve.full[static_cast<std::size_t>(peak_bin)].magnitude_db;
    const double y2 = curve.full[static_cast<std::size_t>(peak_bin + 1)].magnitude_db;
    const double denom = y0 - 2.0 * y1 + y2;
    double delta = denom != 0.0 ? 0.5 * (y0 - y2) / denom : 0.0;
    delta = std::clamp(delta, -0.5, 0.5);
    const double coarse_hz = (static_cast<double>(peak_bin) + delta) * bin_hz;
    est.coarse_hz = coarse_hz;

    // ── Octave / harmonic-peak correction. ──
    // The loudest bin is not always the fundamental: a mildly rolled-off pulse
    // can carry more energy in its 2nd or 3rd partial than in the fundamental,
    // and a magnitude peak then lands an octave (or a twelfth) high. Descend to
    // the LOWEST subharmonic of the coarse peak that still carries real energy.
    // The guard against over-descending is exactly that energy test: a pure
    // fundamental has ~none at f/2, so it is never dragged down, while a pulse
    // whose peak sat on 2·f0 has a genuine partial at f0 and is pulled back.
    const double amp_peak = fitted_amplitude_at(seg, coarse_hz, sample_rate);
    double fundamental_hz = coarse_hz;
    for (int m = 2; m <= 4; ++m) {
        const double candidate = coarse_hz / static_cast<double>(m);
        if (candidate < lo_band)
            break;
        if (fitted_amplitude_at(seg, candidate, sample_rate) >= 0.1 * amp_peak)
            fundamental_hz = candidate; // adopt the lowest qualifying subharmonic
    }

    // ── Refine: maximise the single-tone fitted energy in a one-bin bracket. ──
    const double lo = std::max(fundamental_hz - bin_hz, lo_band);
    const double hi = std::min(fundamental_hz + bin_hz, 0.499 * sample_rate);
    const double refined =
        hi > lo ? golden_max_frequency(seg, lo, hi, sample_rate, 200)
                : fundamental_hz;

    // ── Confidence: fraction of energy explained by the harmonic series. ──
    // Sum the WHOLE comb up to Nyquist, not just the first few partials: a
    // bright oscillator (BLIT/buzz, narrow pulse) spreads its energy across
    // hundreds of harmonics, so a fixed low harmonic count would under-count it
    // and refuse a perfectly voiced signal. `harmonic_count <= 0` means "every
    // harmonic below Nyquist"; a positive value caps the sum (and the cost).
    const int harmonic_ceiling =
        options.harmonic_count > 0 ? options.harmonic_count
                                   : std::numeric_limits<int>::max();
    double harmonic_energy = 0.0;
    if (total_energy > 0.0) {
        for (int k = 1; k <= harmonic_ceiling; ++k) {
            const double hz_k = refined * static_cast<double>(k);
            if (hz_k >= 0.5 * sample_rate)
                break;
            harmonic_energy += fitted_energy_at(seg, hz_k, sample_rate);
        }
    }
    const double ratio =
        total_energy > 0.0 ? harmonic_energy / total_energy : 0.0;
    est.harmonic_energy_ratio = ratio;
    est.confidence = std::clamp(ratio, 0.0, 1.0);

    est.voiced = rms >= options.silence_rms &&
                 est.confidence >= options.min_confidence && refined > 0.0;
    est.hz = est.voiced ? refined : 0.0;
    return est;
}

PitchEstimate estimate_pitch(const pulp::audio::BufferView<const float>& signal,
                             double sample_rate, const PitchOptions& options) {
    require(signal.num_channels() > 0, "pitch estimate needs at least one channel");
    const int ch = std::clamp(options.channel, 0,
                              static_cast<int>(signal.num_channels()) - 1);
    return estimate_pitch(signal.channel(static_cast<std::size_t>(ch)),
                          sample_rate, options);
}

PitchTrack track_pitch(std::span<const float> samples, double sample_rate,
                       const PitchTrackOptions& options) {
    require(sample_rate > 0.0, "sample_rate must be > 0");
    require(options.window_length > 0, "window_length must be > 0");
    require(options.hop_length > 0, "hop_length must be > 0");
    const int total = static_cast<int>(samples.size());
    require(options.window_length <= total,
            "window_length must not exceed the signal length");

    PitchTrack track;
    track.sample_rate = sample_rate;
    track.window_length = options.window_length;
    track.hop_length = options.hop_length;

    PitchOptions frame_opts = options.pitch;
    frame_opts.analysis_offset = 0;
    frame_opts.analysis_length = 0;

    for (int start = 0; start + options.window_length <= total;
         start += options.hop_length) {
        const auto frame = samples.subspan(static_cast<std::size_t>(start),
                                           static_cast<std::size_t>(options.window_length));
        const PitchEstimate est = estimate_pitch(frame, sample_rate, frame_opts);
        PitchTrackPoint point;
        point.time_s = (static_cast<double>(start) +
                        0.5 * static_cast<double>(options.window_length)) /
                       sample_rate;
        point.hz = est.hz;
        point.confidence = est.confidence;
        point.voiced = est.voiced;
        track.points.push_back(point);
    }
    return track;
}

std::vector<double> PitchTrack::voiced_times_s() const {
    std::vector<double> out;
    for (const auto& p : points)
        if (p.voiced)
            out.push_back(p.time_s);
    return out;
}

std::vector<double> PitchTrack::voiced_hz() const {
    std::vector<double> out;
    for (const auto& p : points)
        if (p.voiced)
            out.push_back(p.hz);
    return out;
}

} // namespace pulp::test::audio
