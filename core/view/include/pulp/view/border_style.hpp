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
Color border_band_color(View::BorderStyle style, BorderEdge side,
                        bool outer_band, Color side_color, float width);

}  // namespace pulp::view
