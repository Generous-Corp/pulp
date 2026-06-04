#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <pulp/canvas/canvas.hpp>

namespace pulp::view {

class View;

/// Color parser callback: maps a single CSS color token (hex / rgb() / rgba())
/// to a canvas::Color. When omitted, apply_css_background_gradient() uses a
/// built-in parser covering #RGB / #RRGGBB / #RRGGBBAA, rgb(), rgba(), and
/// `transparent`. The JS WidgetBridge passes its own richer parser so named
/// colors continue to resolve there.
using CssColorParser = std::function<canvas::Color(const std::string&)>;

/// Parse a CSS `linear-gradient(...)`, `radial-gradient(...)`, or
/// `conic-gradient(...)` string and apply it to `v`'s background via the
/// View::set_background_gradient_* API. Returns true iff a gradient was
/// recognized and at least one color stop was applied.
///
/// This is the single shared CSS-gradient path. The JS WidgetBridge, the native
/// design-import materializer (apply_visual_style), and baked C++ codegen all
/// route through it so the three import lanes resolve gradients identically.
bool apply_css_background_gradient(View& v, std::string_view css,
                                   const CssColorParser& parse_color = {});

}  // namespace pulp::view
