// CSS `font-optical-sizing: auto` for paragraph text.
#pragma once

#include "include/core/SkFontArguments.h"
#include "modules/skparagraph/include/TextStyle.h"

namespace pulp::canvas {

/// Ask every face a paragraph style resolves to for its `opsz` axis at the
/// font size in CSS px, which is what `font-optical-sizing: auto` (the CSS
/// default) does. Faces without the axis ignore the coordinate; faces with it
/// clamp it to their range and keep every other axis where it was.
inline void apply_auto_optical_size(skia::textlayout::TextStyle& style,
                                    float css_px) {
    if (!(css_px > 0.0f)) return;
    const SkFontArguments::VariationPosition::Coordinate coordinate{
        SkSetFourByteTag('o', 'p', 's', 'z'), css_px};
    SkFontArguments args;
    args.setVariationDesignPosition({&coordinate, 1});
    style.setFontArguments(args);  // copies the coordinate
}

}  // namespace pulp::canvas
