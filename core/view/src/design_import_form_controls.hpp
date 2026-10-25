// SPDX-License-Identifier: MIT
#pragma once

// Theme-painted form controls in a browser capture. An imported range,
// checkbox or radio whose Chromium native theme painted it (the capture's
// `native_theme` verdict) carries no CSS paint to lower, so it becomes the
// matching widget in its `chromium_refresh` skin, driven by the HTML state the
// capture recorded as `ua_*` attributes. Every other control, and every widget
// Pulp builds itself, keeps the platform skin.

#include "design_import_native_common.hpp"

#include <pulp/view/design_ir.hpp>
#include <pulp/view/widgets.hpp>

#include <memory>
#include <optional>
#include <string>

namespace pulp::view {

/// The HTML state a theme-painted control is painted from, parsed once so the
/// native lane and the baked C++ lane build the same widget.
struct UaFormControlState {
    enum class Control { range, checkbox, radio };
    Control control = Control::checkbox;
    UaColorScheme scheme = UaColorScheme::light;
    std::optional<std::string> accent; ///< computed `accent-color`, absent for `auto`
    bool disabled = false;
    float min = 0.0f, max = 100.0f, step = 1.0f, value = 50.0f; ///< range only
    bool checked = false;                                       ///< checkbox and radio
    int radio_group = 0;                                        ///< radio; 0 = no group
};

/// The state of a theme-painted range, checkbox or radio, or nullopt when the
/// node is not one: no `native_theme` verdict of "native", or no skin for its
/// `ua_control`.
std::optional<UaFormControlState> ua_form_control_state(const IRNode& node);

/// The widget kind a theme-painted control lowers to.
std::optional<NativeWidgetKind> ua_form_control_kind(const IRNode& node);

/// A `RangeSlider` in the `chromium_refresh` skin with the node's HTML range
/// state applied.
std::unique_ptr<RangeSlider> make_ua_range_slider(const IRNode& node);

/// Puts a lowered checkbox or radio in the `chromium_refresh` skin with the
/// node's state applied. A node without `ua_control` is left untouched.
void apply_ua_checkbox(Checkbox& checkbox, const IRNode& node);

} // namespace pulp::view
