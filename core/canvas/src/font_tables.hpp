// Size-dependent face adjustments browsers make from a font's own tables:
// the `opsz` variation axis (CSS `font-optical-sizing: auto`) and the AAT
// `trak` table. Internal to pulp::canvas; Skia text paths only.
#pragma once

#include <cstddef>
#include <vector>

#include "include/core/SkFontArguments.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkTypeface.h"
#include "modules/skparagraph/include/TextStyle.h"

namespace pulp::canvas {

// Optical sizing has two entry points because Pulp's two Skia text paths
// pick faces differently. SkFont draws (Canvas2D measure/paint, the
// per-glyph fallback) carry one resolved face, so it is cloned up front.
// SkParagraph matches its faces by family name inside the FontCollection, so
// there is no face to clone yet; the coordinate travels on the TextStyle and
// SkParagraph clones each face it matches.

/// The `opsz` coordinate both paths ask for at `css_px`: the size rounded to
/// a quarter CSS px. Faces and paragraphs are cached per coordinate, so the
/// rounding is what keeps sizes that animate or auto-fit from minting a new
/// clone per frame; a quarter pixel is well inside what optical sizing can
/// show.
float optical_size_coordinate(float css_px);

/// The variation position `face` takes for text set at `css_px` under
/// `font-optical-sizing: auto`: the face's current coordinates with `opsz`
/// set to `optical_size_coordinate(css_px)`, clamped to the axis range. Empty
/// when the face has no `opsz` axis.
std::vector<SkFontArguments::VariationPosition::Coordinate>
auto_optical_size_position(const SkTypeface* face, float css_px);

/// `face` at its `auto_optical_size_position`. A face without an `opsz` axis
/// is returned unchanged. Clones are cached per face and size, so paint paths
/// can call this per draw.
sk_sp<SkTypeface> typeface_at_auto_optical_size(sk_sp<SkTypeface> face, float css_px);

/// Ask every face a paragraph style resolves to for its `opsz` axis at the
/// coordinate `primary` (the face the style's family list resolves to) takes
/// at `css_px`. Leaves the style alone when `primary` has no `opsz` axis. The
/// arguments carry only the `opsz` coordinate, because they apply to every
/// face the paragraph matches; SkParagraph clones each matched face with
/// them, and the clone starts from that face's current position, so a weight
/// instance keeps its weight. Faces without the axis ignore the coordinate;
/// faces with it clamp it to their range.
void apply_auto_optical_size(skia::textlayout::TextStyle& style, const SkTypeface* primary,
                             float css_px);

/// The tracking a face's AAT `trak` table asks for at `css_px`, in CSS px per
/// glyph: its normal (0) track, interpolated linearly between the table's
/// sizes and held at the end values outside them, scaled from font units.
/// Zero when the face has no usable `trak` table. Browsers on macOS apply it,
/// half before and half after each glyph, which is how the system UI font
/// tightens at text sizes and opens up at display sizes.
float aat_tracking_px(const SkTypeface* face, float css_px);

/// Entries each least-recently-used cache above may hold: optical-size clones
/// per face and coordinate, and parsed `trak` tables per face.
inline constexpr std::size_t kFontTableCacheCapacity = 256;

struct FontTableCacheSizes {
    std::size_t optical_size_clones = 0;
    std::size_t track_tables = 0;
};
FontTableCacheSizes font_table_cache_sizes();

} // namespace pulp::canvas
