#pragma once

/// @file frequency_response.hpp
/// Evaluate what a biquad (or a cascade of them) does to the spectrum.
///
/// This is the analysis half of the biquad story. The design half
/// (filter_design.hpp / iir_design.hpp) turns filter parameters into
/// coefficients; BiquadT runs those coefficients over samples. Neither can
/// answer "what curve does this filter draw?" — that is what lives here.
///
/// Every entry point takes the canonical BiquadCoefficients from biquad.hpp,
/// so the same section that is processing audio right now (via
/// BiquadT::coefficients()) is the one being plotted. A UI never has to
/// re-derive, and therefore never has to approximate, a filter's shape.
///
/// RT contract: allocation-free. These are trig-heavy and meant for
/// control-rate / UI use, not the audio callback.

#include <pulp/signal/biquad.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <span>

namespace pulp::signal {

/// Floor applied to dB conversions. A notch's null is a true zero, so its
/// magnitude in dB is -inf; clamping keeps curve points finite and plottable.
inline constexpr float min_response_db = -200.0f;

/// Convert a linear magnitude to dB, floored at min_response_db.
inline float magnitude_to_db(double magnitude) {
    if (!(magnitude > 0.0)) return min_response_db;
    double db = 20.0 * std::log10(magnitude);
    if (!std::isfinite(db) || db < min_response_db) return min_response_db;
    return static_cast<float>(db);
}

/// Linear magnitude |H(e^jω)| of one section at normalized angular frequency
/// ω = 2π·f/fs (radians/sample, meaningful over [0, π]).
inline double section_magnitude(const BiquadCoefficients& c, double omega) {
    const std::complex<double> z_inv = std::polar(1.0, -omega);
    const std::complex<double> z_inv2 = z_inv * z_inv;
    const std::complex<double> num =
        static_cast<double>(c.b0) + static_cast<double>(c.b1) * z_inv + static_cast<double>(c.b2) * z_inv2;
    const std::complex<double> den =
        1.0 + static_cast<double>(c.a1) * z_inv + static_cast<double>(c.a2) * z_inv2;
    const double d = std::abs(den);
    if (!(d > 0.0)) return 0.0; // pole on the unit circle — treat as no output
    return std::abs(num) / d;
}

/// Linear magnitude |H| of a cascade at angular frequency ω. Sections are in
/// series, so magnitudes multiply (equivalently, their dB values sum).
inline double cascade_magnitude(std::span<const BiquadCoefficients> sos, double omega) {
    double mag = 1.0;
    for (const auto& c : sos) mag *= section_magnitude(c, omega);
    return mag;
}

/// Angular frequency (radians/sample) of `freq_hz` at `sample_rate`, clamped
/// to [0, π]. Frequencies at or above Nyquist alias, so they saturate at π
/// rather than wrapping around and reporting a bogus magnitude.
inline double angular_frequency(double freq_hz, double sample_rate) {
    if (!(sample_rate > 0.0)) return 0.0;
    const double omega = 2.0 * std::acos(-1.0) * freq_hz / sample_rate;
    return std::clamp(omega, 0.0, std::acos(-1.0));
}

/// Magnitude of one section at a frequency in Hz, in dB.
inline float section_magnitude_db(const BiquadCoefficients& c, double freq_hz, double sample_rate) {
    return magnitude_to_db(section_magnitude(c, angular_frequency(freq_hz, sample_rate)));
}

/// Magnitude of a cascade at a frequency in Hz, in dB.
inline float cascade_magnitude_db(std::span<const BiquadCoefficients> sos,
                                  double freq_hz,
                                  double sample_rate) {
    return magnitude_to_db(cascade_magnitude(sos, angular_frequency(freq_hz, sample_rate)));
}

/// Ask a live filter what it looks like: the magnitude, in dB, that this
/// BiquadT is applying at `freq_hz` with its coefficients as they stand.
template <typename SampleType>
inline float magnitude_db(const BiquadT<SampleType>& filter, double freq_hz, double sample_rate) {
    const auto c = filter.coefficients();
    const BiquadCoefficients f{static_cast<float>(c.b0),
                               static_cast<float>(c.b1),
                               static_cast<float>(c.b2),
                               static_cast<float>(c.a1),
                               static_cast<float>(c.a2)};
    return section_magnitude_db(f, freq_hz, sample_rate);
}

/// Center frequency of bin `index` in a real-FFT magnitude array.
///
/// A real FFT of `n` samples yields `n/2 + 1` magnitude bins, linearly spaced
/// from DC (bin 0) to Nyquist (the last bin) — NOT spanning some display range
/// like 20 Hz–20 kHz. Plotting code that assumes otherwise misplaces every bin,
/// so the mapping lives here next to the FFT's own contract rather than being
/// re-guessed at each call site.
inline double fft_bin_frequency(std::size_t index, std::size_t bin_count, double sample_rate) {
    if (bin_count <= 1) return 0.0;
    const double nyquist = sample_rate * 0.5;
    return nyquist * static_cast<double>(index) / static_cast<double>(bin_count - 1);
}

/// Frequency of sample `index` of `count`, log-spaced across [min_hz, max_hz].
/// Index 0 lands exactly on min_hz and index count-1 exactly on max_hz — the
/// spacing a frequency plot's x-axis uses, so a curve sampled with this maps
/// one-to-one onto evenly spaced pixels of a log axis.
inline double log_frequency_at(std::size_t index, std::size_t count, double min_hz, double max_hz) {
    if (count <= 1) return min_hz;
    const double log_min = std::log10(min_hz);
    const double log_max = std::log10(max_hz);
    const double t = static_cast<double>(index) / static_cast<double>(count - 1);
    return std::pow(10.0, log_min + t * (log_max - log_min));
}

/// Sample a cascade's magnitude response into `out`, at out.size() log-spaced
/// frequencies spanning [min_hz, max_hz], in dB.
///
/// This is the stage a curve widget wants: hand it one point per pixel column
/// and the result is the polyline, already in the plot's own x-spacing.
/// Allocation-free — `out` is caller-owned.
inline void response_curve_db(std::span<const BiquadCoefficients> sos,
                              double min_hz,
                              double max_hz,
                              double sample_rate,
                              std::span<float> out) {
    const std::size_t n = out.size();
    for (std::size_t i = 0; i < n; ++i) {
        const double hz = log_frequency_at(i, n, min_hz, max_hz);
        out[i] = cascade_magnitude_db(sos, hz, sample_rate);
    }
}

} // namespace pulp::signal
