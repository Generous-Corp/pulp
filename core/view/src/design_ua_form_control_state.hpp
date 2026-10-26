// SPDX-License-Identifier: MIT
#pragma once

// The HTML state of a theme-painted form control, without the widgets that
// paint it, so a code generator can read it without the widget headers.

#include <pulp/view/design_ir.hpp>

#include <optional>
#include <string>

namespace pulp::view {

enum class UaColorScheme; // pulp/view/widgets.hpp

/// The HTML state a theme-painted control is painted from, parsed once so the
/// native lane and the code generators build the same widget.
struct UaFormControlState {
    enum class Control { range, checkbox, radio, button };
    Control control = Control::checkbox;
    UaColorScheme scheme{};            ///< light unless the capture recorded `dark`
    std::optional<std::string> accent; ///< computed `accent-color`; absent for `auto` and a button
    bool disabled = false;
    float min = 0.0f, max = 100.0f, step = 1.0f, value = 50.0f; ///< range only
    bool checked = false;                                       ///< checkbox and radio
    int radio_group = 0;                                        ///< radio; 0 = no group
};

/// The state of a theme-painted range, checkbox, radio or button, or nullopt when the
/// node is not one: no `native_theme` verdict of "native", or no skin for its
/// `ua_control`.
std::optional<UaFormControlState> ua_form_control_state(const IRNode& node);

/// "light" or "dark", as the bridge's setUaSkin() takes it.
const char* ua_color_scheme_name(UaColorScheme scheme);

} // namespace pulp::view
