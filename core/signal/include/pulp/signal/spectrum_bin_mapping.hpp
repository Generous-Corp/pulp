#pragma once

/// @file spectrum_bin_mapping.hpp
/// Maps FFT magnitude bins onto the columns of a log-frequency display.
///
/// FFT bins are linearly spaced, a log axis is not. Near the bottom of the axis
/// a display column is much narrower than one bin, so reading "the nearest bin"
/// draws a staircase of identical columns. Near the top a column spans many
/// bins, so reading one of them throws away the others. This header is the one
/// definition of how a column reads the spectrum:
///
///  - Column `i` of `n` covers the log-frequency interval
///    `[i/n, (i+1)/n]` of `[min_hz, max_hz]`; its centre is at `(i+0.5)/n`.
///    The same geometry is what a pixel axis with `n` one-pixel columns sees.
///  - A column no wider than one bin reads the spectrum at its centre's
///    fractional bin position. The three nearest bins define a parabola in the
///    dB domain, and the parabola is evaluated AT the fractional position (not
///    at its vertex), so the curve is smooth and a peak between two bins is
///    reproduced. Where the three bins are not concave (a ramp or a valley) the
///    reading falls back to plain linear interpolation, which cannot overshoot
///    and keeps monotone data monotone.
///  - A column wider than one bin aggregates every bin it covers, weighting the
///    two edge bins by the fraction of their cell inside the column. `peak`
///    keeps the loudest value, `power_sum` adds the bins' power, `power_mean`
///    averages it. Sums and means work in the linear power domain.
///
/// Inputs are dB values for `fft_size / 2 + 1` bins ordered DC..Nyquist, bin k
/// centred on `k * sample_rate / fft_size`. NaN and -inf read as `floor_db`;
/// +inf reads as `ceiling_db`.
///
/// RT contract: `configure()` allocates and runs on the UI/prepare thread.
/// `map()` and every accessor are allocation-free and take no lock, but the
/// mapping is a plain value: do not call `configure()` while another thread is
/// inside `map()` on the same object.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::signal {

/// How a column wider than one bin combines the bins it covers.
enum class SpectrumAggregation : std::uint8_t {
    peak,       ///< loudest bin (dB max)
    power_sum,  ///< total power of the covered bins
    power_mean, ///< mean power of the covered bins
};

struct SpectrumBinMappingConfig {
    double sample_rate = 48000.0;
    int fft_size = 2048; ///< transform size; the input has fft_size/2+1 bins
    std::size_t columns = 0;
    double min_hz = 20.0;
    double max_hz = 20000.0;
    float floor_db = -200.0f;   ///< value for NaN/-inf input and above-Nyquist columns
    float ceiling_db = 200.0f;  ///< clamp for +inf input
};

class SpectrumBinMapping {
  public:
    /// Rebuild the column tables. Allocates. Returns false (and leaves the
    /// mapping invalid, so map() writes the floor) for a non-positive sample
    /// rate, an FFT size below 2, or a frequency range that is not
    /// `0 < min_hz < max_hz`. Zero columns is valid and maps nothing.
    bool configure(const SpectrumBinMappingConfig& config) {
        valid_ = false;
        config_ = config;
        lo_pos_.clear();
        centre_pos_.clear();
        hi_pos_.clear();

        const bool ok = config.sample_rate > 0.0 && std::isfinite(config.sample_rate) &&
                        config.fft_size >= 2 && config.min_hz > 0.0 &&
                        config.max_hz > config.min_hz && std::isfinite(config.max_hz) &&
                        config.floor_db < config.ceiling_db;
        if (!ok)
            return false;

        const std::size_t n = config.columns;
        lo_pos_.resize(n);
        centre_pos_.resize(n);
        hi_pos_.resize(n);

        const double log_min = std::log10(config.min_hz);
        const double log_span = std::log10(config.max_hz) - log_min;
        const double bin_hz = config.sample_rate / static_cast<double>(config.fft_size);
        for (std::size_t i = 0; i < n; ++i) {
            const double inv = 1.0 / static_cast<double>(n);
            const auto pos = [&](double t) {
                return std::pow(10.0, log_min + t * log_span) / bin_hz;
            };
            lo_pos_[i] = pos(static_cast<double>(i) * inv);
            centre_pos_[i] = pos((static_cast<double>(i) + 0.5) * inv);
            hi_pos_[i] = pos(static_cast<double>(i + 1) * inv);
        }
        valid_ = true;
        return true;
    }

    bool valid() const noexcept {
        return valid_;
    }
    std::size_t columns() const noexcept {
        return lo_pos_.size();
    }
    /// Number of input bins map() expects.
    std::size_t bin_count() const noexcept {
        return config_.fft_size >= 2 ? static_cast<std::size_t>(config_.fft_size / 2 + 1) : 0;
    }
    const SpectrumBinMappingConfig& config() const noexcept {
        return config_;
    }

    /// Centre frequency of column `i` (geometric middle of the column).
    double column_frequency(std::size_t i) const noexcept {
        return i < centre_pos_.size() ? centre_pos_[i] * bin_hz() : 0.0;
    }
    /// Fractional bin position of column `i`'s centre (bin k sits at k).
    double column_bin_position(std::size_t i) const noexcept {
        return i < centre_pos_.size() ? centre_pos_[i] : 0.0;
    }
    /// Width of column `i` in bins. Above 1 the column aggregates (a width
    /// within 1e-9 of one bin still counts as one bin).
    double column_width_bins(std::size_t i) const noexcept {
        return i < lo_pos_.size() ? hi_pos_[i] - lo_pos_[i] : 0.0;
    }
    /// Fractional column coordinate of `hz`: 0 at the left edge of the first
    /// column, `columns()` at the right edge of the last. Out-of-range input
    /// extrapolates; non-positive frequency returns 0.
    double frequency_to_column(double hz) const noexcept {
        if (!valid_ || !(hz > 0.0) || lo_pos_.empty())
            return 0.0;
        const double log_min = std::log10(config_.min_hz);
        const double log_span = std::log10(config_.max_hz) - log_min;
        return (std::log10(hz) - log_min) / log_span * static_cast<double>(lo_pos_.size());
    }

    /// Map `bins_db` onto `out` (one value per column; extra `out` entries
    /// beyond columns() are set to the floor). If the mapping is invalid or
    /// `bins_db` does not have bin_count() entries (an FFT-size change in
    /// flight), every output is the floor rather than a misread.
    void map(std::span<const float> bins_db, std::span<float> out,
             SpectrumAggregation aggregation) const noexcept {
        const float floor_db = config_.floor_db;
        const std::size_t n = std::min(out.size(), lo_pos_.size());
        for (std::size_t i = n; i < out.size(); ++i)
            out[i] = floor_db;
        if (!valid_ || bins_db.size() != bin_count()) {
            for (std::size_t i = 0; i < n; ++i)
                out[i] = floor_db;
            return;
        }

        const double last = static_cast<double>(bins_db.size() - 1);
        for (std::size_t i = 0; i < n; ++i) {
            // Entirely above Nyquist: nothing to show. Report the floor rather
            // than clamping to the top bin, which would draw a phantom shelf.
            if (centre_pos_[i] > last) {
                out[i] = floor_db;
                continue;
            }
            if (hi_pos_[i] - lo_pos_[i] <= 1.0 + 1e-9) {
                out[i] = interpolate(bins_db, centre_pos_[i]);
            } else {
                out[i] = aggregate(bins_db, lo_pos_[i], std::min(hi_pos_[i], last), aggregation);
            }
        }
    }

    /// Value of the dB spectrum at a fractional bin position, by the
    /// parabola-with-linear-fallback rule described at the top of this file.
    float interpolate(std::span<const float> bins_db, double pos) const noexcept {
        const std::size_t count = bins_db.size();
        if (count == 0)
            return config_.floor_db;
        if (count == 1 || !(pos > 0.0))
            return sanitize(bins_db[0]);
        const double last = static_cast<double>(count - 1);
        if (pos >= last)
            return sanitize(bins_db[count - 1]);

        // The pair of bins bracketing `pos`, and the louder of the two as the
        // centre of the parabola. Centring on the louder bin (rather than the
        // nearest one) makes both sides of a peak read the SAME parabola, so
        // the curve is continuous across the peak instead of jumping where
        // two different parabolas meet and overshooting at the join.
        const auto lo = static_cast<std::size_t>(std::floor(pos));
        const float a = sanitize(bins_db[lo]);
        const float b = sanitize(bins_db[lo + 1]);
        const float linear = a + (b - a) * static_cast<float>(pos - static_cast<double>(lo));

        const std::size_t k0 = b > a ? lo + 1 : lo;
        // The parabola needs a neighbour on each side of its centre bin.
        if (k0 == 0 || k0 + 1 >= count)
            return linear;
        const float y0 = k0 == lo ? a : b;
        const float ym = sanitize(bins_db[k0 - 1]);
        const float yp = sanitize(bins_db[k0 + 1]);
        const float curvature = 0.5f * (yp + ym) - y0; // second difference / 2
        if (!(curvature < 0.0f))
            return linear; // ramp or valley: monotone linear fallback

        const float slope = 0.5f * (yp - ym);
        const auto fd = static_cast<float>(pos - static_cast<double>(k0)); // [-1, 1]
        float value = y0 + slope * fd + curvature * fd * fd;
        // Round-off guard: between two bins the reading never dips below the
        // lower of them. (A concave parabola centred on the louder bin is
        // either monotone across the pair or peaks inside it.)
        value = std::max(value, std::min(a, b));
        return std::clamp(value, config_.floor_db, config_.ceiling_db);
    }

  private:
    double bin_hz() const noexcept {
        return config_.sample_rate / static_cast<double>(config_.fft_size);
    }

    float sanitize(float v) const noexcept {
        if (std::isnan(v))
            return config_.floor_db;
        return std::clamp(v, config_.floor_db, config_.ceiling_db);
    }

    float aggregate(std::span<const float> bins_db, double lo, double hi,
                    SpectrumAggregation aggregation) const noexcept {
        const std::size_t count = bins_db.size();
        if (aggregation == SpectrumAggregation::peak) {
            // The loudest bin centre inside the column, plus the interpolated
            // values at both fractional edges so the result is continuous with
            // the narrow-column reading.
            float best = std::max(interpolate(bins_db, lo), interpolate(bins_db, hi));
            const auto first = static_cast<std::size_t>(std::ceil(lo));
            const auto last = std::min(count - 1, static_cast<std::size_t>(std::floor(hi)));
            for (std::size_t k = first; k <= last; ++k)
                best = std::max(best, sanitize(bins_db[k]));
            return best;
        }

        // Power domain. Bin k owns the cell [k-0.5, k+0.5]; weight by the part
        // of the cell inside the column so a fractional edge counts fractionally.
        const double span = hi - lo;
        const auto first = static_cast<std::size_t>(std::max(0.0, std::floor(lo + 0.5)));
        const auto last = std::min(count - 1, static_cast<std::size_t>(std::floor(hi + 0.5)));
        double acc = 0.0;
        for (std::size_t k = first; k <= last; ++k) {
            const double cell_lo = std::max(lo, static_cast<double>(k) - 0.5);
            const double cell_hi = std::min(hi, static_cast<double>(k) + 0.5);
            const double w = cell_hi - cell_lo;
            if (w > 0.0)
                acc += w * std::pow(10.0, static_cast<double>(sanitize(bins_db[k])) * 0.1);
        }
        if (aggregation == SpectrumAggregation::power_mean && span > 0.0)
            acc /= span;
        if (!(acc > 0.0))
            return config_.floor_db;
        return std::clamp(static_cast<float>(10.0 * std::log10(acc)), config_.floor_db,
                          config_.ceiling_db);
    }

    SpectrumBinMappingConfig config_{};
    bool valid_ = false;
    std::vector<double> lo_pos_;
    std::vector<double> centre_pos_;
    std::vector<double> hi_pos_;
};

} // namespace pulp::signal
