// SPDX-License-Identifier: MIT
#pragma once

// Paint that a captured element draws from outside its own CSS box tree: the
// string a form control renders from its state, and the raster plane a
// captured element (a `<canvas>` snapshot) hangs inside its box. Both are
// placed from the element's computed border and padding, and both are pure
// functions of the snapshot, so whole-tree lowering only decides WHEN to emit
// them.

#include "browser_capture_styles.hpp"
#include "browser_capture_tree.hpp"

#include <pulp/view/design_ir.hpp>

#include <map>
#include <optional>
#include <string>

namespace pulp::import_design::detail {

/// A parsed CSS length in px, or 0 for anything else. Chrome serializes
/// computed border widths as `<n>px`, so this is the whole grammar that
/// reaches here.
double px_value(const std::string& value);

/// The state an `<input>`'s raw `type` attribute selects. HTML matches the
/// attribute ASCII case-insensitively without trimming, and a missing, empty or
/// unknown value is the Text state: Chromium paints `type="bogus"` and
/// `type=" password "` as plain text fields.
std::string normalized_input_type(std::string raw);

/// What a form control draws from its own state, and where, or nothing.
///
/// Chrome renders an `<input>`'s value and a closed `<select>`'s selected
/// option in a user-agent shadow tree that DOMSnapshot does not serialize, so
/// the string reaches the capture only as control state and the box it sits in
/// only as the control's content box. Both placements are CSS semantics rather
/// than theme: a single-line text field and a button-type input lay their
/// string out in the content box, vertically centred. A themed (`appearance`
/// other than `none`) select is the exception — the theme reserves its own room
/// for the arrow, which no computed length records — so it is refused rather
/// than guessed.
struct ControlValueText {
    std::string text;
    CapturedBox content_box;
};

std::optional<ControlValueText>
control_value_text(const CapturedStyleIndex& index, int node_index, const CapturedBox& box,
                   const std::map<std::string, std::string>& computed);

/// The state the native theme paints a themed range, checkbox or radio from,
/// recorded as `ua_*` attributes on its lowered node: `ua_control` (the input
/// type), the HTML `ua_min` / `ua_max` / `ua_step` / `ua_value` of a range, the
/// live `ua_checked` of a checkbox or radio and the `ua_radio_group` its `name`
/// puts it in, `ua_disabled`, and the computed `ua_accent` (absent for `auto`)
/// and `ua_color_scheme`. Other input types record nothing; returns whether
/// the node is one of the three.
bool record_ua_control_state(const CapturedStyleIndex& index, int node_index,
                             const std::map<std::string, std::string>& computed,
                             decltype(pulp::view::IRNode::attributes)& attributes);

/// Clears the background and border a theme-painted control's computed style
/// reports. They are the user-agent defaults the native theme replaces (an
/// unstyled range reports a white background it never draws); author box
/// styling would have dropped the theme, so none of it is the author's.
void drop_theme_replaced_box_paint(pulp::view::IRStyle& style);

/// The image node for a viewport-plane raster of a captured element.
///
/// Squeezing the whole plane into the element's box would draw the page in
/// miniature, so the element stays a box (its own border and background paint
/// as usual) and the plane hangs inside it at the page origin, clipped to the
/// content box where Chromium draws the bitmap. Children are placed against
/// the padding box, so the plane's page origin sits at minus that box's origin.
pulp::view::IRNode captured_viewport_plane(const std::string& owner_name, const std::string& asset,
                                           const CapturedLayerExtent& layer, const CapturedBox& box,
                                           const std::map<std::string, std::string>& computed);

} // namespace pulp::import_design::detail
