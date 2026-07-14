#pragma once

/// @file graph_scale.hpp
/// Shared axis mapping for frequency-domain plots.
///
/// Every widget that draws against a frequency axis — EQ curves, spectrum
/// analyzers, spectrograms — needs the same four conversions and the same tick
/// placement. When each one keeps its own private copy they drift: one ends up
/// logarithmic and another linear, and then a spectrum can no longer be
/// overlaid on an EQ curve because the two disagree about where 1 kHz is.
/// These scales are the single definition, so the overlay lines up by
/// construction.

#include <cstddef>
#include <span>
#include <vector>

namespace pulp::view {

/// Maps frequency (Hz) to a horizontal pixel position on a logarithmic axis.
/// Frequency is perceived roughly logarithmically, so an audio frequency axis
/// is log by default — an octave occupies the same width everywhere.
struct LogFrequencyScale {
    float min_hz = 20.0f;
    float max_hz = 20000.0f;
    float x = 0.0f;      ///< left edge, pixels
    float width = 0.0f;  ///< axis width, pixels

    /// Pixel x of a frequency. Not clamped — callers plotting a curve want
    /// points just outside the viewport to keep the line's slope correct at
    /// the edges.
    float to_x(float hz) const;

    /// Frequency at a pixel x. Inverse of to_x().
    float to_frequency(float px) const;

    /// Frequency of point `index` of `count`, log-spaced across the axis, so
    /// point i lands on evenly spaced pixel columns. Mirrors
    /// pulp::signal::log_frequency_at so a curve sampled in the DSP layer maps
    /// one-to-one onto this axis with no resampling.
    float frequency_at(std::size_t index, std::size_t count) const;

    /// Gridline frequencies in a 1-2-5 sequence per decade (20, 30, 50, 100,
    /// 200, …) clipped to [min_hz, max_hz].
    std::vector<float> ticks() const;

    /// True for decade boundaries (100, 1k, 10k) — the lines conventionally
    /// drawn heavier and labelled.
    static bool is_major_tick(float hz);
};

/// Maps decibels to a vertical pixel position on a linear axis (dB is already
/// logarithmic in amplitude, so the axis itself is linear in dB). y grows
/// downward, as in screen coordinates: max_db sits at the top.
struct DecibelScale {
    float min_db = -24.0f;
    float max_db = 24.0f;
    float y = 0.0f;       ///< top edge, pixels
    float height = 0.0f;  ///< axis height, pixels

    float to_y(float db) const;
    float to_decibels(float py) const;

    /// Gridline values every `step_db`, always including 0 dB when it is in
    /// range (the unity line a reader looks for first).
    std::vector<float> ticks(float step_db) const;
};

/// Resample a real-FFT magnitude array (bins spanning DC..Nyquist) onto
/// `out.size()` log-spaced frequencies across [scale.min_hz, scale.max_hz],
/// linearly interpolating in dB between adjacent bins.
///
/// Plotting raw bins on a log axis directly does not work: FFT bins are
/// linearly spaced, so on a log axis the bottom decade receives almost no bins
/// (leaving visible gaps) while the top decade piles hundreds into a few pixel
/// columns. Resampling to one value per pixel column produces a continuous
/// curve at every frequency, and puts the spectrum in the same x-spacing as a
/// response curve sampled with signal::response_curve_db — so the two overlay
/// exactly.
///
/// Allocation-free; `out` is caller-owned.
void resample_spectrum_log(std::span<const float> bins_db,
                           float sample_rate,
                           const LogFrequencyScale& scale,
                           std::span<float> out);

} // namespace pulp::view
