#pragma once

// The ui.js lowering of what a browser capture recorded about a node's own
// paint: a theme-painted range, checkbox or radio, and the CSS-px-snapped rect
// a box paints in. The native materializer and the baked C++ exporter apply
// the same facts from the same parse (design_import_form_controls.hpp), so the
// ui.js a plugin loads builds the widget those lanes build.

#include <pulp/view/design_ir.hpp>

#include <ostream>
#include <string>

namespace pulp::view {

/// For a theme-painted range, checkbox or radio (ua_form_control_state()),
/// writes its create call under @p parent (a quoted id or `''`) and the bridge
/// calls that put it in the skin and HTML state Chromium painted, then returns
/// true. Writes nothing and returns false for any other node.
bool emit_js_ua_form_control(std::ostream& out, const IRNode& node, const std::string& indent,
                             const std::string& id, const std::string& parent);

/// For a theme-painted <button>, writes createButtonSurface() under @p parent,
/// laid out as a row or a column, and the scheme and disabled state the
/// surface is painted in, then returns true; the caller emits its children.
/// Writes nothing and returns false for any other node.
bool emit_js_ua_button_surface(std::ostream& out, const IRNode& node, const std::string& indent,
                               const std::string& id, const std::string& parent, bool row);

/// Writes setCapturedPaintRect() for a node that carries a valid captured
/// paint rect (`browser_box_paint_*`), with the values the native materializer
/// gives View::set_captured_box_paint_rect(); writes nothing otherwise.
void emit_js_captured_paint_rect(std::ostream& out, const IRNode& node, const std::string& indent,
                                 const std::string& id);

} // namespace pulp::view
