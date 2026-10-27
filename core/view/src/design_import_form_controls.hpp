// SPDX-License-Identifier: MIT
#pragma once

// Theme-painted form controls in a browser capture. An imported range,
// checkbox, radio or push button (a <button>, or an input of type button,
// submit or reset) whose Chromium native theme painted it (the
// capture's `native_theme` verdict) carries no CSS paint to lower, so it
// becomes the matching widget in its `chromium_refresh` skin, or a
// ButtonSurface for a button, driven by the HTML state the capture recorded as
// `ua_*` attributes. Every other control, and every widget Pulp builds itself,
// keeps the platform skin.

#include "design_import_native_common.hpp"
#include "design_ua_form_control_state.hpp"

#include <pulp/view/design_ir.hpp>
#include <pulp/view/widgets.hpp>

#include <memory>
#include <optional>
#include <string>

namespace pulp::view {

/// The widget kind a theme-painted control lowers to.
std::optional<NativeWidgetKind> ua_form_control_kind(const IRNode& node);

/// A `RangeSlider` in the `chromium_refresh` skin with the node's HTML range
/// state applied.
std::unique_ptr<RangeSlider> make_ua_range_slider(const IRNode& node);

/// Puts a lowered checkbox or radio in the `chromium_refresh` skin with the
/// node's state applied. A node without `ua_control` is left untouched.
void apply_ua_checkbox(Checkbox& checkbox, const IRNode& node);

/// A ButtonSurface with the node's scheme and disabled state applied. Its
/// content (the button's text and icons) stays the node's children.
std::unique_ptr<ButtonSurface> make_ua_button_surface(const IRNode& node);

} // namespace pulp::view
