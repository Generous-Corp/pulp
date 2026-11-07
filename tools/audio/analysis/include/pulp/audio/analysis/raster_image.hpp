#pragma once

/// @file raster_image.hpp
/// Minimal RGBA8 raster surface plus the drawing primitives the audio image
/// renderers share: solid fills, a built-in 5x7 bitmap font for burned-in
/// labels, and "nice" axis tick placement.
///
/// Everything here is pure arithmetic over an in-memory pixel buffer. There
/// is no file I/O and no dependency on the render or view layers; the CLI
/// hands the finished pixels to `pulp::render::HeadlessSurface::encode_png`.
/// Keeping the raster independent of the GPU stack lets the tests assert on
/// pixels directly and lets the renderers run anywhere the analysis library
/// builds.
///
/// Display-only contract: nothing produced here feeds a verdict. The images
/// exist so an agent or a person can look at a render; measurements come
/// from the analyzers, never from pixels.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::test::audio {

/// Opaque 8-bit colour.
struct Rgb8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    friend bool operator==(const Rgb8&, const Rgb8&) = default;
};

/// RGBA8 pixels, row-major, top row first. Same layout as
/// `pulp::render::HeadlessSurface::Rgba`, so the CLI can move the vector
/// across without a copy or a per-pixel conversion.
struct RasterImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;

    static RasterImage filled(std::uint32_t width, std::uint32_t height, Rgb8 colour);

    bool empty() const noexcept { return width == 0 || height == 0 || rgba.empty(); }

    /// Writes one opaque pixel. Out-of-range coordinates are ignored so
    /// callers can draw markers at the image edge without bounds bookkeeping.
    void put(int x, int y, Rgb8 colour) noexcept;

    /// Reads one pixel. Out-of-range coordinates read as black.
    Rgb8 get(int x, int y) const noexcept;

    /// Fills the half-open rectangle [x0, x1) x [y0, y1), clipped to the image.
    void fill_rect(int x0, int y0, int x1, int y1, Rgb8 colour) noexcept;
};

// ── Built-in 5x7 bitmap font ───────────────────────────────────────────────
//
// Printable ASCII 0x20..0x7E, one byte per row, bit 4 is the leftmost column.
// The footer line and axis labels are burned into the image with this font so
// a screenshot stays self-describing with no font file, no text shaper and no
// platform text API in the loop. Rendering is deterministic by construction.

inline constexpr int kGlyphWidth = 5;
inline constexpr int kGlyphHeight = 7;
inline constexpr int kGlyphAdvance = 6;  ///< glyph width plus one column of spacing

/// Seven row bitmaps for `c`. Characters outside printable ASCII render as
/// the replacement box so an unexpected byte is visible rather than silent.
const std::uint8_t* glyph_rows_5x7(char c) noexcept;

/// Pixel width of `text` at `scale` (no trailing spacing column).
int text_width_5x7(std::string_view text, int scale = 1) noexcept;

/// Draws `text` with its top-left corner at (x, y). Each glyph pixel becomes a
/// `scale` x `scale` block. Clipped at the image edge.
void draw_text_5x7(RasterImage& image, int x, int y, std::string_view text, Rgb8 colour,
                   int scale = 1) noexcept;

// ── Axis ticks ─────────────────────────────────────────────────────────────

/// A tick on a linear axis: the data value and its label.
struct AxisTick {
    double value = 0.0;
    std::string label;

    friend bool operator==(const AxisTick&, const AxisTick&) = default;
};

/// Largest "nice" step (1, 2 or 5 times a power of ten) that places at most
/// `max_ticks` ticks across [lo, hi]. Returns 0 for a degenerate range.
double nice_tick_step(double lo, double hi, int max_ticks) noexcept;

/// Ticks for a time axis in seconds over [start_s, end_s], at most
/// `max_ticks`, labelled with as many decimals as the step needs. The first
/// tick is the first multiple of the step at or after `start_s`.
std::vector<AxisTick> time_axis_ticks(double start_s, double end_s, int max_ticks);

/// Formats a frequency for a tick label: "20", "1k", "2.5k", "16k".
std::string format_frequency_label(double hz);

/// Formats seconds with `decimals` fractional digits, trimmed of trailing
/// zeros but never of the integer part ("0", "1.5", "12.25").
std::string format_seconds_label(double seconds, int decimals);

// ── Shared chrome: axes and footer around a plot ───────────────────────────

/// A label on the left axis at a unit position (0 = bottom of the plot,
/// 1 = top). The renderers compute positions on their own axis (frequency
/// scale, amplitude) and the composer only places text.
struct AxisLabel {
    double unit = 0.0;
    std::string label;
};

/// Text burned into the image around the plot. `footer` is written verbatim
/// on the bottom line; the time axis spans [start_s, end_s].
struct ImageChrome {
    std::string footer;
    double start_s = 0.0;
    double end_s = 0.0;
};

inline constexpr int kChromeLeftMargin = 40;    ///< room for left-axis labels
inline constexpr int kChromeRightMargin = 4;
inline constexpr int kChromeTopMargin = 4;
inline constexpr int kChromeTimeAxisHeight = 12;
inline constexpr int kChromeFooterHeight = 11;

/// Total image size for a plot of `plot_width` x `plot_height` once the
/// margins, time axis and footer are added. Callers that size the plot from a
/// requested image size invert this.
std::uint32_t chrome_image_width(std::uint32_t plot_width) noexcept;
std::uint32_t chrome_image_height(std::uint32_t plot_height) noexcept;

/// Places `plot` inside a framed image with left-axis labels, time ticks along
/// the bottom and the footer line. Deterministic: same inputs, same pixels.
RasterImage compose_plot_with_chrome(const RasterImage& plot, const std::vector<AxisLabel>& left,
                                     const ImageChrome& chrome);

}  // namespace pulp::test::audio
