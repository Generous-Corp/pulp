#pragma once

// How a render surface maps glyph coverage to pixels.
//
// Skia applies a contrast and gamma "preblend" to glyph masks, and it reads
// both values from the surface the text is drawn into: SkFont has no gamma
// field, a saveLayer inherits its parent's values, and the strike cache keys
// glyphs on them. A text-raster choice is therefore a property of a SURFACE,
// fixed when the surface is created, and every surface a frame's text can pass
// through (the frame target, offscreen and scene targets, retained layers) has
// to be created with the same one, or text painted through a cached layer
// comes out a different weight from the same text painted directly.
//
// Measured on macOS (CoreText through Skia, raster and DPR 2) against Chrome:
//   - `legacy` (Skia's defaults: contrast 0.5, sRGB gamma) with hinting slight
//     reproduces Chromium's `-webkit-font-smoothing: auto` exactly.
//   - `chromium` (contrast 0, gamma 1) with hinting none reproduces
//     `-webkit-font-smoothing: antialiased` exactly, about 23% lighter than
//     `legacy`. An `auto` node drawn on a `chromium` surface is about 2% heavier
//     than Chromium's.
// No single surface reproduces both modes, so a surface that holds any
// `antialiased` text is created `chromium` (select_text_raster_policy).
//
// Hinting is per font, not per surface, and is chosen by the text painter.

#include <cstdint>
#include <span>

namespace pulp::canvas {

/// Glyph preblend a surface is created with. `legacy` is Skia's default and
/// what every surface uses unless its creator asks otherwise.
enum class TextRasterPolicy : std::uint8_t {
    legacy,
    chromium,
};

/// CSS `-webkit-font-smoothing` as it affects glyph rasterization on macOS.
/// `subpixel-antialiased` and `auto` both resolve to `auto` there.
enum class FontSmoothing : std::uint8_t {
    auto_smoothing,
    antialiased,
};

/// Surface text contrast and gamma for a policy, the two values SkSurfaceProps
/// carries. `chromium` is the tuple measured to reproduce Chromium's
/// `antialiased` glyphs; at gamma 1 the contrast value is inert, so it is
/// recorded as 0.
struct TextRasterTuple {
    float contrast;
    float gamma;
};

/// Skia's compile-time defaults (SK_GAMMA_CONTRAST, SK_GAMMA_EXPONENT = sRGB).
inline constexpr TextRasterTuple kLegacyTextRaster{0.5f, 0.0f};
inline constexpr TextRasterTuple kChromiumTextRaster{0.0f, 1.0f};

constexpr TextRasterTuple text_raster_tuple(TextRasterPolicy policy) {
    return policy == TextRasterPolicy::chromium ? kChromiumTextRaster : kLegacyTextRaster;
}

/// The policy for a surface holding text nodes with these smoothing modes:
/// `chromium` when ANY node is `antialiased`, otherwise `legacy`.
///
/// The rule is asymmetric on purpose. An `antialiased` node on a `legacy`
/// surface paints about 21% too much ink, which is visible; an `auto` node on a
/// `chromium` surface paints about 2% too much, which is not. A surface with no
/// text stays `legacy`.
constexpr TextRasterPolicy select_text_raster_policy(std::span<const FontSmoothing> nodes) {
    for (const FontSmoothing smoothing : nodes) {
        if (smoothing == FontSmoothing::antialiased)
            return TextRasterPolicy::chromium;
    }
    return TextRasterPolicy::legacy;
}

} // namespace pulp::canvas
