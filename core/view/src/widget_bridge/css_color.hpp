#pragma once

#include <pulp/canvas/canvas.hpp>

#include <string_view>

namespace pulp::view {

// CSS Color Level 4 parser — accepts hex (#RGB / #RRGGBB / #RRGGBBAA),
// rgb() / rgba(), hsl() / hsla(), the common named-color subset, and
// `transparent`. Unrecognized input yields opaque white. This mirrors the
// colors the widget bridge accepts from scripted CSS.
canvas::Color parse_bridge_css_color(std::string_view str);

} // namespace pulp::view
