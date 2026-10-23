// CSS border line styles, as painted by View.
#pragma once

#include <pulp/view/view.hpp>

#include <optional>
#include <string_view>

namespace pulp::view {

/// The `View::BorderStyle` a CSS `border-style` / `outline-style` keyword
/// names, or nullopt for anything that is not one of the ten CSS keywords.
/// Exact, lowercase match, as computed styles report them.
std::optional<View::BorderStyle> border_style_from_css(std::string_view keyword);

/// The two shades a 3D border style paints a side colour in, as Chromium
/// resolves them. Lightening and darkening move the colour's largest channel
/// by a third of full scale, up or down, and scale the other channels with
/// it; alpha is kept. A colour no lighter than rgb(32,32,32) by relative
/// luminance is too dark to darken visibly, so it is lightened once first and
/// that becomes the dark shade. A colour lighter than rgb(235,235,235) is not
/// lightened. Derived from, and checked against, Chrome's render of 189
/// colours.
struct BorderShades {
    Color light;
    Color dark;
};
BorderShades border_shades(Color side_color);

/// Which edge of the box a border side is on.
enum class BorderEdge { top, right, bottom, left };

/// How a side of `width` CSS px splits into bands for `style`: an outer band,
/// a transparent gap, and an inner band, summing to `width`. A single-band
/// style puts the whole width in `outer`. `groove` / `ridge` split in half,
/// the top and left sides taking the odd pixel on the outside; `double` draws
/// two lines a third of the width each, rounded, with the rest as the gap.
/// Below 2px a groove or ridge, and below 3px a double border, is one solid
/// band.
struct BorderBands {
    float outer = 0.0f;
    float gap = 0.0f;
    float inner = 0.0f;
};
BorderBands border_bands(View::BorderStyle style, BorderEdge side, float width);

/// The colour `style` paints the outer or inner band of `side` in, for a side
/// whose specified colour is `side_color` and width `width`.
Color border_band_color(View::BorderStyle style, BorderEdge side, bool outer_band, Color side_color,
                        float width);

/// Whether `style` is painted in bands of per-side colour (`double` and the
/// four 3D styles) rather than as one stroke.
bool is_banded_border_style(View::BorderStyle style);

/// A box's corner radii, each corner with a horizontal and a vertical radius.
struct CornerRadii {
    float tl_x = 0.0f, tl_y = 0.0f;
    float tr_x = 0.0f, tr_y = 0.0f;
    float br_x = 0.0f, br_y = 0.0f;
    float bl_x = 0.0f, bl_y = 0.0f;

    /// Four circular corners.
    static CornerRadii circular(float tl, float tr, float br, float bl) {
        return {tl, tl, tr, tr, br, br, bl, bl};
    }
};

/// `radii` fitted to a `width` x `height` box the way CSS fits border radii
/// (CSS Backgrounds 3, "Overlapping Curves"): a negative radius is 0, and when
/// the two radii along any side sum to more than that side, every radius is
/// scaled by f = min over the four sides of (side length / sum of its two
/// radii), so adjacent curves meet instead of crossing. One factor for all
/// eight values keeps a circular corner circular, and radii that already fit
/// come back unchanged. The background, the clip and every border style shape
/// their corners from this, so a border's outer edge and the background under
/// it are the same curve.
CornerRadii fit_corner_radii(float width, float height, CornerRadii radii);

/// A banded border in its own border box, origin at the top-left corner.
struct BandedBorder {
    View::BorderStyle style = View::BorderStyle::outset;
    float width = 0.0f; ///< border box
    float height = 0.0f;
    float top = 0.0f; ///< side widths, already resolved and clamped
    float right = 0.0f;
    float bottom = 0.0f;
    float left = 0.0f;
    Color top_color;
    Color right_color;
    Color bottom_color;
    Color left_color;
    float radius_tl = 0.0f; ///< outer corner radii, fitted by fit_corner_radii
    float radius_tr = 0.0f;
    float radius_br = 0.0f;
    float radius_bl = 0.0f;
};

/// Paint `border`. Each band is the region between two rounded rectangles —
/// the outer one inset by the bands outside it, the inner one by the bands up
/// to its inside edge, with corner radii shrunk by the inset on each axis as
/// CSS shrinks them — so a rounded border keeps a curved inner edge. The band
/// is divided between sides by the line from each outer box corner through
/// the matching padding-box corner, and sides that share a colour are filled
/// as one shape, so no antialiased seam runs down a mitre between them.
void paint_banded_border(canvas::Canvas& canvas, const BandedBorder& border);

} // namespace pulp::view
