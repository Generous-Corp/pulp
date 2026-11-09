#pragma once

/// @file waveform_raster.hpp
/// Waveform image pipeline over `pulp::audio::WaveformOverview`, the one
/// display peak pyramid Pulp has. Nothing here builds peaks from samples; it
/// consumes an overview level (pick it with `WaveformOverview::best_level_for`)
/// and reduces its (min, max) pairs to pixel columns, then paints them.
///
///   OverviewLevel ──columns_from_level──▶ WaveformColumns
///                 ──render_waveform_plot──▶ plot RasterImage
///                 ──compose_plot_with_chrome──▶ final image
///
/// Clipping is shown, never hidden: `AudioPeak` stores q7 values clamped to
/// [-1, 1], so a peak at or beyond full scale lands exactly on ±127 and the
/// column is flagged `clipped`. The renderer draws a marker on those columns.
/// The true sample peak above full scale is not recoverable from the
/// pyramid; the CLI measures it from the samples it streams and reports it
/// in the sidecar.
///
/// Display-only contract: columns and pixels never feed a verdict.

#include "raster_image.hpp"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// The overview types are declared only: every function below takes or returns
// them by reference or as an opaque value, so this header stays off the
// buffer.hpp include chain. Callers include <pulp/audio/waveform_overview.hpp>.
namespace pulp::audio {
struct AudioPeak;
struct OverviewLevel;
class WaveformOverview;
} // namespace pulp::audio

namespace pulp::test::audio {

/// Per-column (min, max) in [-1, 1] plus a clip flag.
struct WaveformColumns {
    std::vector<float> min;
    std::vector<float> max;
    std::vector<std::uint8_t> clipped;

    std::uint32_t width() const noexcept {
        return static_cast<std::uint32_t>(min.size());
    }
};

/// True when the peak touches full scale in either direction. `AudioPeak`
/// quantizes by truncation toward zero, so ±127 is reached only by samples
/// with |x| >= 1.0 (0.996 still truncates to 126).
bool peak_is_clipped(const pulp::audio::AudioPeak& peak) noexcept;

/// Base resolution for building an overview that will be drawn `width`
/// columns wide: the largest power of two at most `frames / width`, clamped
/// to [1, 65536]. Powers of two mirror the overview's own 2x decimation
/// ladder, so the chosen base is the level a full pyramid would have
/// selected for this width.
std::uint32_t choose_samples_per_peak(std::uint64_t frames, std::uint32_t width) noexcept;

/// Reduces one overview level to `width` columns for `channel`. Column `c`
/// folds peaks `[c * P / width, (c + 1) * P / width)` (min of mins, max of
/// maxes); when the level has fewer peaks than columns, columns repeat the
/// peak they fall on. `channel == WaveformOverview::kAllChannels` folds every
/// channel. An out-of-range channel or an empty level yields empty columns.
WaveformColumns columns_from_level(const pulp::audio::OverviewLevel& level, std::uint32_t channel,
                                   std::uint32_t width);

/// Concatenates per-chunk level-0 tables into one level. Every chunk must
/// share `samples_per_peak` and channel count; this is how a range longer
/// than memory is assembled from overviews built on consecutive windows.
/// Returns an empty level on a mismatch.
pulp::audio::OverviewLevel
concatenate_levels(const std::vector<pulp::audio::OverviewLevel>& chunks);

struct WaveformImageOptions {
    std::uint32_t plot_height = 160;
    /// Optional highlighted selection as unit positions along the time axis
    /// (0 = left edge, 1 = right edge).
    std::optional<std::pair<double, double>> selection_unit;
};

/// Left-axis labels for an amplitude plot: +1, +0.5, 0, -0.5, -1.
std::vector<AxisLabel> amplitude_axis_labels();

/// Plot pixels only: width = column count, height = `plot_height`. Zero sits
/// on the centre row; each column is a vertical bar from min to max, and a
/// clipped column gets a red marker across the top and bottom edge.
RasterImage render_waveform_plot(const WaveformColumns& columns,
                                 const WaveformImageOptions& options);

/// Plot plus amplitude labels, time ticks and footer.
RasterImage render_waveform_image(const WaveformColumns& columns,
                                  const WaveformImageOptions& options, const ImageChrome& chrome);

/// Colour of the clip marker, exposed so a test can look for it.
Color waveform_clip_marker_colour() noexcept;

} // namespace pulp::test::audio
