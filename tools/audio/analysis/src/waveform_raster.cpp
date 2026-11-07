// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

#include <pulp/audio/analysis/waveform_raster.hpp>

#include <pulp/audio/waveform_overview.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::test::audio {

using pulp::audio::AudioPeak;
using pulp::audio::OverviewLevel;
using pulp::audio::WaveformOverview;

bool peak_is_clipped(const AudioPeak& peak) noexcept {
    return peak.max_q7 >= 127 || peak.min_q7 <= -127;
}

std::uint32_t choose_samples_per_peak(std::uint64_t frames, std::uint32_t width) noexcept {
    if (width == 0 || frames == 0)
        return 1;
    const std::uint64_t per_column = frames / width;
    std::uint32_t spp = 1;
    while (static_cast<std::uint64_t>(spp) * 2 <= per_column && spp < 65536u)
        spp *= 2;
    return spp;
}

WaveformColumns columns_from_level(const OverviewLevel& level, std::uint32_t channel,
                                   std::uint32_t width) {
    WaveformColumns out;
    const std::uint32_t channels = static_cast<std::uint32_t>(level.peaks.size());
    const std::uint32_t peaks = level.peaks_per_channel;
    if (width == 0 || channels == 0 || peaks == 0)
        return out;
    if (channel != WaveformOverview::kAllChannels && channel >= channels)
        return out;
    out.min.assign(width, 0.0f);
    out.max.assign(width, 0.0f);
    out.clipped.assign(width, 0);

    const std::uint32_t first_channel = channel == WaveformOverview::kAllChannels ? 0 : channel;
    const std::uint32_t last_channel =
        channel == WaveformOverview::kAllChannels ? channels - 1 : channel;

    for (std::uint32_t column = 0; column < width; ++column) {
        const std::uint64_t begin = static_cast<std::uint64_t>(column) * peaks / width;
        std::uint64_t end = static_cast<std::uint64_t>(column + 1) * peaks / width;
        if (end <= begin)
            end = begin + 1; // fewer peaks than columns: repeat the peak this column falls on
        float lo = 1.0f;
        float hi = -1.0f;
        bool clipped = false;
        for (std::uint32_t ch = first_channel; ch <= last_channel; ++ch) {
            const auto& table = level.peaks[ch];
            for (std::uint64_t p = begin; p < end && p < table.size(); ++p) {
                const AudioPeak& peak = table[static_cast<std::size_t>(p)];
                lo = std::min(lo, peak.min());
                hi = std::max(hi, peak.max());
                clipped = clipped || peak_is_clipped(peak);
            }
        }
        if (hi < lo) {
            lo = 0.0f;
            hi = 0.0f;
        }
        out.min[column] = lo;
        out.max[column] = hi;
        out.clipped[column] = clipped ? 1 : 0;
    }
    return out;
}

WaveformColumns columns_from_overview(const WaveformOverview& overview, std::uint32_t channel,
                                      std::uint32_t width) {
    if (overview.empty() || width == 0)
        return {};
    return columns_from_level(overview.level(overview.best_level_for(width)), channel, width);
}

OverviewLevel concatenate_levels(const std::vector<OverviewLevel>& chunks) {
    OverviewLevel out;
    if (chunks.empty())
        return out;
    out.samples_per_peak = chunks.front().samples_per_peak;
    const std::size_t channels = chunks.front().peaks.size();
    out.peaks.resize(channels);
    for (const auto& chunk : chunks) {
        if (chunk.samples_per_peak != out.samples_per_peak || chunk.peaks.size() != channels)
            return {};
        for (std::size_t ch = 0; ch < channels; ++ch) {
            if (chunk.peaks[ch].size() != chunk.peaks_per_channel)
                return {};
            out.peaks[ch].insert(out.peaks[ch].end(), chunk.peaks[ch].begin(),
                                 chunk.peaks[ch].end());
        }
        out.peaks_per_channel += chunk.peaks_per_channel;
    }
    return out;
}

std::vector<AxisLabel> amplitude_axis_labels() {
    return {
        AxisLabel{1.0, "+1"},   AxisLabel{0.75, "+.5"}, AxisLabel{0.5, "0"},
        AxisLabel{0.25, "-.5"}, AxisLabel{0.0, "-1"},
    };
}

Rgb8 waveform_clip_marker_colour() noexcept {
    return Rgb8{255, 64, 64};
}

RasterImage render_waveform_plot(const WaveformColumns& columns,
                                 const WaveformImageOptions& options) {
    constexpr Rgb8 kBackground{18, 18, 22};
    constexpr Rgb8 kSelection{34, 46, 64};
    constexpr Rgb8 kGrid{40, 46, 56};
    constexpr Rgb8 kZero{70, 80, 95};
    constexpr Rgb8 kWave{90, 200, 140};

    const std::uint32_t width = columns.width();
    const std::uint32_t height = options.plot_height;
    RasterImage plot = RasterImage::filled(width, height, kBackground);
    if (width == 0 || height == 0)
        return plot;

    const int h = static_cast<int>(height);
    if (options.selection_unit) {
        const auto [a, b] = *options.selection_unit;
        const double lo = std::clamp(std::min(a, b), 0.0, 1.0);
        const double hi = std::clamp(std::max(a, b), 0.0, 1.0);
        const int x0 = static_cast<int>(std::floor(lo * width));
        const int x1 = static_cast<int>(std::ceil(hi * width));
        plot.fill_rect(x0, 0, std::max(x1, x0 + 1), h, kSelection);
    }
    // Quarter-amplitude grid lines and the zero line.
    for (double level : {0.25, 0.75})
        plot.fill_rect(0, static_cast<int>(std::round(level * (h - 1))), static_cast<int>(width),
                       static_cast<int>(std::round(level * (h - 1))) + 1, kGrid);
    const int zero_row = (h - 1) / 2;
    plot.fill_rect(0, zero_row, static_cast<int>(width), zero_row + 1, kZero);

    auto row_for = [&](float value) {
        const double v = std::clamp(static_cast<double>(value), -1.0, 1.0);
        return static_cast<int>(std::round((1.0 - (v + 1.0) * 0.5) * (h - 1)));
    };
    for (std::uint32_t column = 0; column < width; ++column) {
        int top = row_for(columns.max[column]);
        int bottom = row_for(columns.min[column]);
        if (bottom < top)
            std::swap(top, bottom);
        plot.fill_rect(static_cast<int>(column), top, static_cast<int>(column) + 1, bottom + 1,
                       kWave);
        if (columns.clipped[column]) {
            const Rgb8 marker = waveform_clip_marker_colour();
            plot.fill_rect(static_cast<int>(column), 0, static_cast<int>(column) + 1, 3, marker);
            plot.fill_rect(static_cast<int>(column), h - 3, static_cast<int>(column) + 1, h,
                           marker);
        }
    }
    return plot;
}

RasterImage render_waveform_image(const WaveformColumns& columns,
                                  const WaveformImageOptions& options, const ImageChrome& chrome) {
    return compose_plot_with_chrome(render_waveform_plot(columns, options), amplitude_axis_labels(),
                                    chrome);
}

} // namespace pulp::test::audio
