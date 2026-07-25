#pragma once

// Reverb-specific measurements: reverberation time, band-limited reverberation
// time, and echo density.
//
// These live here rather than in pulp::audio-analysis because they measure a
// REVERBERANT DECAY rather than a buffer or a transfer function — they need an
// impulse response and a decay model, not a spectrum. Everything spectral a
// reverb test needs (magnitude curves, tone gains, THD) is already in
// <pulp/audio/analysis/audio_spectrum.hpp> and should be taken from there.
//
// Header-only and dependency-light on purpose: a test that wants a T60 should
// not have to link another library to get one.

#include <pulp/signal/biquad.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace pulp::test::audio {

/// Schroeder backward integration of the squared impulse response
/// (Schroeder, JASA 37, 1965): edc[i] is the energy remaining from sample i on.
inline std::vector<double> energy_decay_curve(std::span<const float> ir) {
    std::vector<double> edc(ir.size() + 1, 0.0);
    for (std::size_t i = ir.size(); i-- > 0;)
        edc[i] = edc[i + 1] + static_cast<double>(ir[i]) * static_cast<double>(ir[i]);
    return edc;
}

/// Reverberation time from the energy decay curve, extrapolated from the
/// -5 dB .. -25 dB span (the T20 convention). Starting at -5 dB skips the
/// direct/early burst; stopping at -25 dB keeps the fit clear of the noise
/// floor. Returns 0 when the decay never reaches -25 dB inside the buffer.
inline double t60_from_edc(const std::vector<double>& edc, double sample_rate) {
    if (edc.empty() || edc[0] <= 0.0) return 0.0;
    const double reference = edc[0];
    auto crossing = [&](double db) -> double {
        const double target = reference * std::pow(10.0, db / 10.0);
        for (std::size_t i = 0; i + 1 < edc.size(); ++i)
            if (edc[i] <= target) return static_cast<double>(i) / sample_rate;
        return -1.0;
    };
    const double t5 = crossing(-5.0);
    const double t25 = crossing(-25.0);
    if (t5 < 0.0 || t25 <= t5) return 0.0;
    return (t25 - t5) * 3.0;
}

inline double t60_schroeder(std::span<const float> ir, double sample_rate) {
    return t60_from_edc(energy_decay_curve(ir), sample_rate);
}

/// T60 inside one band. The impulse response is bandpassed (two cascaded
/// biquads, so the skirts do not let a neighbouring band's decay leak in) and
/// then integrated as above.
inline double band_t60(std::span<const float> ir, double sample_rate, double centre_hz,
                       double q = 2.0) {
    signal::BiquadT<double> a;
    signal::BiquadT<double> b;
    a.set_coefficients(signal::BiquadT<double>::Type::bandpass, centre_hz, q, sample_rate);
    b.set_coefficients(signal::BiquadT<double>::Type::bandpass, centre_hz, q, sample_rate);
    std::vector<float> filtered(ir.size());
    for (std::size_t i = 0; i < ir.size(); ++i)
        filtered[i] = static_cast<float>(b.process(a.process(static_cast<double>(ir[i]))));
    return t60_schroeder(filtered, sample_rate);
}

/// Normalized echo density (Abel & Huang): the fraction of samples in a sliding
/// window whose magnitude exceeds that window's standard deviation, divided by
/// the same fraction for a Gaussian (erfc(1/sqrt(2)) = 0.3173). It rises from
/// near 0 in the sparse early reflections to about 1 once the response is
/// indistinguishable from noise — the standard way to say "the tank has mixed".
inline std::vector<double> echo_density_curve(std::span<const float> ir,
                                              double sample_rate, double window_ms = 20.0,
                                              int hop_samples = 128) {
    const auto window = static_cast<std::size_t>(window_ms * 0.001 * sample_rate);
    std::vector<double> curve;
    if (ir.size() < window || window == 0) return curve;
    constexpr double kGaussianFraction = 0.3173;
    for (std::size_t start = 0; start + window <= ir.size();
         start += static_cast<std::size_t>(hop_samples)) {
        double sum = 0.0;
        for (std::size_t i = 0; i < window; ++i) {
            const double v = static_cast<double>(ir[start + i]);
            sum += v * v;
        }
        const double sigma = std::sqrt(sum / static_cast<double>(window));
        if (sigma <= 0.0) {
            curve.push_back(0.0);
            continue;
        }
        std::size_t above = 0;
        for (std::size_t i = 0; i < window; ++i)
            if (std::abs(static_cast<double>(ir[start + i])) > sigma) ++above;
        curve.push_back(static_cast<double>(above) / static_cast<double>(window) /
                        kGaussianFraction);
    }
    return curve;
}

/// Time, in seconds, at which the echo density first reaches `threshold` and
/// stays there — the mixing time. Returns -1 if it never does.
inline double mixing_time_seconds(std::span<const float> ir, double sample_rate,
                                  double threshold = 0.9, double window_ms = 20.0,
                                  int hop_samples = 128) {
    const auto curve = echo_density_curve(ir, sample_rate, window_ms, hop_samples);
    for (std::size_t i = 0; i < curve.size(); ++i) {
        if (curve[i] < threshold) continue;
        bool holds = true;
        for (std::size_t j = i; j < std::min(curve.size(), i + 8); ++j)
            if (curve[j] < threshold * 0.9) holds = false;
        if (holds)
            return static_cast<double>(i * static_cast<std::size_t>(hop_samples)) /
                   sample_rate;
    }
    return -1.0;
}

/// RMS over a half-open sample range.
inline double range_rms(std::span<const float> x, std::size_t from, std::size_t to) {
    to = std::min(to, x.size());
    if (to <= from) return 0.0;
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i)
        sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    return std::sqrt(sum / static_cast<double>(to - from));
}

/// Spectral DENSITY in a proportional band around `centre_hz` — the RMS of a
/// bank of Goertzel bins, not their sum.
///
/// Two reasons it is a density rather than a total. A granular pitch shifter
/// spreads its output into grain-rate sidebands, so a single bin under-reports
/// "is the octave there" by an order of magnitude and a band is the only honest
/// measure. And a proportional band is 32x wider at 8 kHz than at 250 Hz, so
/// summing its bins builds a 15 dB tilt into any band-to-band comparison —
/// dividing by the bin count removes it, and a flat spectrum then reads flat.
inline double band_energy(std::span<const float> x, std::size_t from, std::size_t to,
                          double centre_hz, double sample_rate, double fraction = 0.12,
                          double bin_hz = 8.0) {
    to = std::min(to, x.size());
    if (to <= from) return 0.0;
    double energy = 0.0;
    int bins = 0;
    for (double hz = centre_hz * (1.0 - fraction); hz <= centre_hz * (1.0 + fraction);
         hz += bin_hz) {
        ++bins;
        const double w = 6.283185307179586 * hz / sample_rate;
        const double c = 2.0 * std::cos(w);
        double s1 = 0.0;
        double s2 = 0.0;
        for (std::size_t i = from; i < to; ++i) {
            const double s0 = static_cast<double>(x[i]) + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double n = static_cast<double>(to - from);
        const double mag = 2.0 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / n;
        energy += mag * mag;
    }
    return bins > 0 ? std::sqrt(energy / static_cast<double>(bins)) : 0.0;
}

}  // namespace pulp::test::audio
