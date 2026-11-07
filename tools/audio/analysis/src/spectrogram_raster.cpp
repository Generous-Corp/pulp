// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

#include <pulp/audio/analysis/spectrogram_raster.hpp>

#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>

namespace pulp::test::audio {

// ── Frequency axis ─────────────────────────────────────────────────────────

std::string_view frequency_scale_name(FrequencyScale scale) noexcept {
    switch (scale) {
    case FrequencyScale::linear:
        return "linear";
    case FrequencyScale::log:
        return "log";
    case FrequencyScale::mel:
        return "mel";
    }
    return "linear";
}

std::optional<FrequencyScale> parse_frequency_scale(std::string_view text) noexcept {
    if (text == "linear")
        return FrequencyScale::linear;
    if (text == "log")
        return FrequencyScale::log;
    if (text == "mel")
        return FrequencyScale::mel;
    return std::nullopt;
}

double hz_to_mel(double hz) noexcept {
    return 2595.0 * std::log10(1.0 + hz / 700.0);
}

double mel_to_hz(double mel) noexcept {
    return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0);
}

namespace {

double axis_transform(double hz, FrequencyScale scale) noexcept {
    switch (scale) {
    case FrequencyScale::linear:
        return hz;
    case FrequencyScale::log:
        return std::log2(std::max(hz, std::numeric_limits<double>::min()));
    case FrequencyScale::mel:
        return hz_to_mel(std::max(hz, 0.0));
    }
    return hz;
}

double axis_inverse(double value, FrequencyScale scale) noexcept {
    switch (scale) {
    case FrequencyScale::linear:
        return value;
    case FrequencyScale::log:
        return std::exp2(value);
    case FrequencyScale::mel:
        return mel_to_hz(value);
    }
    return value;
}

} // namespace

double frequency_to_unit(double hz, double f_lo, double f_hi, FrequencyScale scale) noexcept {
    const double lo = axis_transform(f_lo, scale);
    const double hi = axis_transform(f_hi, scale);
    if (!(hi > lo))
        return 0.0;
    return (axis_transform(hz, scale) - lo) / (hi - lo);
}

double unit_to_frequency(double unit, double f_lo, double f_hi, FrequencyScale scale) noexcept {
    const double lo = axis_transform(f_lo, scale);
    const double hi = axis_transform(f_hi, scale);
    return axis_inverse(lo + unit * (hi - lo), scale);
}

double axis_low_frequency(double sample_rate, int fft_length, FrequencyScale scale) noexcept {
    if (scale == FrequencyScale::log && fft_length > 0)
        return sample_rate / static_cast<double>(fft_length);
    return 0.0;
}

std::vector<BinBand> row_bin_bands(std::uint32_t bins, double sample_rate, int fft_length,
                                   std::uint32_t rows, FrequencyScale scale) {
    std::vector<BinBand> bands;
    if (bins == 0 || rows == 0 || fft_length <= 0 || !(sample_rate > 0.0))
        return bands;
    bands.reserve(rows);
    const double bin_hz = sample_rate / static_cast<double>(fft_length);
    const double f_lo = axis_low_frequency(sample_rate, fft_length, scale);
    const double f_hi = sample_rate / 2.0;
    const std::uint32_t first_bin = scale == FrequencyScale::log ? 1u : 0u;
    const std::uint32_t last_bin = bins - 1;

    auto bin_for_frequency = [&](double hz) {
        const double raw = hz / bin_hz;
        const long rounded = std::lround(raw);
        return static_cast<std::uint32_t>(
            std::clamp<long>(rounded, static_cast<long>(first_bin), static_cast<long>(last_bin)));
    };

    for (std::uint32_t row = 0; row < rows; ++row) {
        // Row 0 is the top of the plot (highest frequency).
        const double unit_top = 1.0 - static_cast<double>(row) / static_cast<double>(rows);
        const double unit_bottom = 1.0 - static_cast<double>(row + 1) / static_cast<double>(rows);
        const double hz_top = unit_to_frequency(unit_top, f_lo, f_hi, scale);
        const double hz_bottom = unit_to_frequency(unit_bottom, f_lo, f_hi, scale);
        // Bins whose centre lies inside [hz_bottom, hz_top).
        const double first_raw = std::ceil(hz_bottom / bin_hz - 1e-9);
        const double last_raw = std::ceil(hz_top / bin_hz - 1e-9) - 1.0;
        BinBand band;
        if (last_raw >= first_raw) {
            band.first =
                static_cast<std::uint32_t>(std::clamp<double>(first_raw, first_bin, last_bin));
            band.last =
                static_cast<std::uint32_t>(std::clamp<double>(last_raw, first_bin, last_bin));
        } else {
            // Row narrower than one bin: snap to the nearest bin centre.
            band.first = band.last = bin_for_frequency(0.5 * (hz_bottom + hz_top));
        }
        if (band.last < band.first)
            band.last = band.first;
        bands.push_back(band);
    }
    // The top row owns Nyquist and the bottom row owns the axis floor, so no
    // bin in range is orphaned by rounding at the ends.
    bands.front().last = last_bin;
    bands.back().first = first_bin;
    return bands;
}

std::vector<AxisLabel> frequency_axis_labels(double sample_rate, int fft_length,
                                             FrequencyScale scale, std::uint32_t rows) {
    std::vector<AxisLabel> labels;
    if (!(sample_rate > 0.0) || fft_length <= 0 || rows == 0)
        return labels;
    const double f_lo = axis_low_frequency(sample_rate, fft_length, scale);
    const double f_hi = sample_rate / 2.0;
    auto add = [&](double hz) {
        if (hz < f_lo - 1e-9 || hz > f_hi + 1e-9)
            return;
        labels.push_back(
            AxisLabel{frequency_to_unit(hz, f_lo, f_hi, scale), format_frequency_label(hz)});
    };
    if (scale == FrequencyScale::linear) {
        const int max_ticks = std::max(2, static_cast<int>(rows) / 32);
        const double step = nice_tick_step(0.0, f_hi, max_ticks);
        if (step > 0.0)
            for (double hz = 0.0; hz <= f_hi + step * 1e-6; hz += step)
                add(std::round(hz / step) * step);
    } else {
        for (double decade = 10.0; decade <= f_hi; decade *= 10.0)
            for (double m : {1.0, 2.0, 5.0})
                add(m * decade);
        add(f_hi);
    }
    return labels;
}

// ── Palette ────────────────────────────────────────────────────────────────

Rgb8 spectrogram_palette(double t) noexcept {
    // Stops chosen so luminance is monotone: a grey-scale copy stays ordered.
    constexpr std::array<std::array<double, 3>, 5> kStops = {{
        {8.0, 8.0, 24.0},      // floor: near-black navy
        {30.0, 60.0, 200.0},   // blue
        {170.0, 40.0, 180.0},  // magenta
        {250.0, 140.0, 40.0},  // orange
        {255.0, 250.0, 200.0}, // ceiling: pale yellow
    }};
    if (!(t > 0.0))
        t = 0.0;
    if (t > 1.0)
        t = 1.0;
    const double scaled = t * static_cast<double>(kStops.size() - 1);
    const std::size_t index =
        std::min<std::size_t>(static_cast<std::size_t>(scaled), kStops.size() - 2);
    const double frac = scaled - static_cast<double>(index);
    const auto& a = kStops[index];
    const auto& b = kStops[index + 1];
    auto mix = [&](int channel) {
        const double value =
            a[static_cast<std::size_t>(channel)] +
            (b[static_cast<std::size_t>(channel)] - a[static_cast<std::size_t>(channel)]) * frac;
        return static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
    };
    return Rgb8{mix(0), mix(1), mix(2)};
}

double db_to_palette_unit(double db, double ceiling_db, double range_db) noexcept {
    if (!(range_db > 0.0))
        return 0.0;
    return std::clamp((db - (ceiling_db - range_db)) / range_db, 0.0, 1.0);
}

// ── STFT grid ──────────────────────────────────────────────────────────────

std::string validate_spectrogram_settings(const SpectrogramSettings& settings) {
    if (settings.fft_length < 16)
        return "fft length must be at least 16";
    if ((settings.fft_length & (settings.fft_length - 1)) != 0)
        return "fft length must be a power of two";
    if (settings.hop < 1)
        return "hop must be at least 1";
    if (settings.hop > settings.fft_length)
        return "hop must not exceed the fft length";
    if (!(settings.db_range > 0.0) || !std::isfinite(settings.db_range))
        return "db range must be a positive number";
    return {};
}

std::uint64_t spectrogram_frame_count(std::uint64_t total_frames, int fft_length,
                                      int hop) noexcept {
    if (fft_length <= 0 || hop <= 0)
        return 0;
    const auto fft = static_cast<std::uint64_t>(fft_length);
    if (total_frames < fft)
        return 0;
    return (total_frames - fft) / static_cast<std::uint64_t>(hop) + 1;
}

float bin_magnitude_dbfs(double magnitude, int fft_length) noexcept {
    if (fft_length <= 0 || !(magnitude > 0.0))
        return kSpectrogramFloorDb;
    const double normalized = 2.0 * magnitude / static_cast<double>(fft_length);
    const double db = 20.0 * std::log10(normalized);
    return static_cast<float>(std::max<double>(db, kSpectrogramFloorDb));
}

SpectrogramBuilder::SpectrogramBuilder(const SpectrogramSettings& settings, double sample_rate,
                                       std::uint64_t total_frames, std::uint32_t columns)
    : settings_(settings), sample_rate_(sample_rate),
      columns_(std::max<std::uint32_t>(columns, 1)) {
    frames_total_ = spectrogram_frame_count(total_frames, settings_.fft_length, settings_.hop);
    bins_ = static_cast<std::uint32_t>(settings_.fft_length / 2 + 1);
    window_ = window_coefficients(settings_.window, settings_.fft_length);
    segment_.assign(static_cast<std::size_t>(settings_.fft_length), 0.0);
    carry_.reserve(static_cast<std::size_t>(settings_.fft_length) * 2);
    column_touched_.assign(columns_, 0);
    grid_.columns = columns_;
    grid_.bins = bins_;
    grid_.sample_rate = sample_rate_;
    grid_.fft_length = settings_.fft_length;
    grid_.hop = settings_.hop;
    grid_.frames_total = frames_total_;
    grid_.magnitude_db.assign(static_cast<std::size_t>(columns_) * bins_, kSpectrogramFloorDb);
}

void SpectrogramBuilder::analyze_frame(const float* frame) {
    const int N = settings_.fft_length;
    for (int i = 0; i < N; ++i)
        segment_[static_cast<std::size_t>(i)] =
            static_cast<double>(frame[i]) * window_[static_cast<std::size_t>(i)];

    // Fft64 pins the portable double path; the spectrogram's numbers do not
    // move with the float backend (vDSP vs mufft vs radix-2).
    pulp::signal::Fft64 fft(N);
    std::vector<std::complex<double>> spectrum(static_cast<std::size_t>(bins_));
    fft.forward_real_half(segment_.data(), spectrum.data());

    const std::uint32_t column = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        frames_done_ * columns_ / std::max<std::uint64_t>(frames_total_, 1), columns_ - 1));
    column_touched_[column] = 1;
    float* dst = grid_.magnitude_db.data() + static_cast<std::size_t>(column) * bins_;
    for (std::uint32_t bin = 0; bin < bins_; ++bin) {
        const float db = bin_magnitude_dbfs(std::abs(spectrum[bin]), N);
        if (db > dst[bin])
            dst[bin] = db;
    }
    ++frames_done_;
}

void SpectrogramBuilder::push(std::span<const float> samples) {
    if (finished_ || frames_done_ >= frames_total_ || samples.empty())
        return;
    carry_.insert(carry_.end(), samples.begin(), samples.end());
    const auto fft = static_cast<std::size_t>(settings_.fft_length);
    const auto hop = static_cast<std::size_t>(settings_.hop);
    std::size_t offset = 0;
    while (carry_.size() - offset >= fft && frames_done_ < frames_total_) {
        analyze_frame(carry_.data() + offset);
        offset += hop;
    }
    if (offset > 0)
        carry_.erase(carry_.begin(), carry_.begin() + static_cast<std::ptrdiff_t>(offset));
}

SpectrogramGrid SpectrogramBuilder::finish() {
    if (finished_)
        return grid_;
    finished_ = true;
    grid_.frames_analyzed = frames_done_;
    // Fill columns no frame reached with the nearest analysed column so a
    // short range still renders as a continuous image.
    std::int64_t last_touched = -1;
    for (std::uint32_t column = 0; column < columns_; ++column) {
        if (column_touched_[column]) {
            last_touched = column;
            continue;
        }
        std::int64_t source = last_touched;
        if (source < 0) {
            for (std::uint32_t probe = column + 1; probe < columns_; ++probe) {
                if (column_touched_[probe]) {
                    source = probe;
                    break;
                }
            }
        }
        if (source < 0)
            continue;
        std::copy_n(grid_.magnitude_db.begin() + static_cast<std::ptrdiff_t>(source) * bins_, bins_,
                    grid_.magnitude_db.begin() + static_cast<std::ptrdiff_t>(column) * bins_);
    }
    carry_.clear();
    carry_.shrink_to_fit();
    return grid_;
}

// ── Rendering ──────────────────────────────────────────────────────────────

RasterImage render_spectrogram_plot(const SpectrogramGrid& grid,
                                    const SpectrogramImageOptions& options) {
    RasterImage plot =
        RasterImage::filled(grid.columns, options.plot_height, spectrogram_palette(0.0));
    if (grid.columns == 0 || grid.bins == 0 || options.plot_height == 0)
        return plot;
    const auto bands = row_bin_bands(grid.bins, grid.sample_rate, grid.fft_length,
                                     options.plot_height, options.scale);
    for (std::uint32_t row = 0; row < options.plot_height; ++row) {
        const BinBand band = bands[row];
        for (std::uint32_t column = 0; column < grid.columns; ++column) {
            float peak = kSpectrogramFloorDb;
            for (std::uint32_t bin = band.first; bin <= band.last; ++bin)
                peak = std::max(peak, grid.at(column, bin));
            const double unit = db_to_palette_unit(peak, options.db_ceiling, options.db_range);
            plot.put(static_cast<int>(column), static_cast<int>(row), spectrogram_palette(unit));
        }
    }
    return plot;
}

RasterImage render_spectrogram_image(const SpectrogramGrid& grid,
                                     const SpectrogramImageOptions& options,
                                     const ImageChrome& chrome) {
    const RasterImage plot = render_spectrogram_plot(grid, options);
    const auto labels = frequency_axis_labels(grid.sample_rate, grid.fft_length, options.scale,
                                              options.plot_height);
    return compose_plot_with_chrome(plot, labels, chrome);
}

} // namespace pulp::test::audio
