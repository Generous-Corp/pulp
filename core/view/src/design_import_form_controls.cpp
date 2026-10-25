// SPDX-License-Identifier: MIT
#include "design_import_form_controls.hpp"

#include "design_ir_helpers.hpp"

#include <pulp/view/css_gradient.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace pulp::view {
namespace {

// A valid floating-point number in HTML's sense is what Chromium has already
// accepted, so strict full-string parsing is enough; anything else falls back
// to the HTML default exactly as the browser did.
std::optional<float> html_number(const IRNode& node, std::string_view key) {
    const auto text = attr(node, key);
    if (!text || text->empty())
        return std::nullopt;
    char* end = nullptr;
    const float value = std::strtof(text->c_str(), &end);
    if (end != text->c_str() + text->size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

UaColorScheme scheme_of(const IRNode& node) {
    // `normal`, `light` and `light dark` resolve to light in the capture's
    // light-preferring browser; only a scheme that offers dark without light
    // paints the dark palette.
    const auto scheme = attr(node, "ua_color_scheme").value_or("");
    const bool dark = scheme.find("dark") != std::string::npos;
    const bool light = scheme.find("light") != std::string::npos;
    return dark && !light ? UaColorScheme::dark : UaColorScheme::light;
}

} // namespace

std::optional<NativeWidgetKind> ua_form_control_kind(const IRNode& node) {
    const auto state = ua_form_control_state(node);
    if (!state)
        return std::nullopt;
    switch (state->control) {
    case UaFormControlState::Control::range:
        return NativeWidgetKind::range_slider;
    case UaFormControlState::Control::button:
        return NativeWidgetKind::button_surface;
    case UaFormControlState::Control::checkbox:
    case UaFormControlState::Control::radio:
        break;
    }
    return NativeWidgetKind::checkbox;
}

std::optional<UaFormControlState> ua_form_control_state(const IRNode& node) {
    if (attr(node, "native_theme").value_or("") != "native")
        return std::nullopt;
    const auto control = attr(node, "ua_control").value_or("");
    UaFormControlState state;
    if (control == "range")
        state.control = UaFormControlState::Control::range;
    else if (control == "checkbox")
        state.control = UaFormControlState::Control::checkbox;
    else if (control == "radio")
        state.control = UaFormControlState::Control::radio;
    else if (control == "button")
        state.control = UaFormControlState::Control::button;
    else
        return std::nullopt;
    state.scheme = scheme_of(node);
    state.disabled = attr(node, "ua_disabled").has_value();
    if (state.control == UaFormControlState::Control::button)
        return state;
    if (const auto accent = attr(node, "ua_accent"); accent && css_color_syntax_supported(*accent))
        state.accent = *accent;
    if (state.control == UaFormControlState::Control::range) {
        // HTML's defaults: min 0, max 100, step 1, and `step="any"` for none.
        state.min = html_number(node, "ua_min").value_or(0.0f);
        state.max = html_number(node, "ua_max").value_or(100.0f);
        if (attr(node, "ua_step").value_or("") == "any")
            state.step = 0.0f;
        else if (const auto step = html_number(node, "ua_step"); step && *step > 0.0f)
            state.step = *step;
        // The live value Chromium painted; without one, HTML's default is the
        // midpoint, which the slider then clamps and steps as the browser did.
        const float midpoint =
            state.max < state.min ? state.min : state.min + (state.max - state.min) / 2.0f;
        state.value = html_number(node, "ua_value").value_or(midpoint);
        return state;
    }
    state.checked = attr(node, "ua_checked").value_or("0") == "1";
    if (state.control == UaFormControlState::Control::radio) {
        // Radios sharing a `name` exclude each other. The id only has to match
        // among siblings and be the same in every lane, so it is an FNV-1a hash
        // of the name, never 0 (which means no group).
        if (const auto group = attr(node, "ua_radio_group"); group && !group->empty()) {
            std::uint32_t hash = 2166136261u;
            for (const unsigned char c : *group)
                hash = (hash ^ c) * 16777619u;
            state.radio_group = static_cast<int>(hash & 0x7fffffffu) | 1;
        }
    }
    return state;
}

std::unique_ptr<RangeSlider> make_ua_range_slider(const IRNode& node) {
    auto slider = std::make_unique<RangeSlider>();
    const auto state = ua_form_control_state(node);
    if (!state)
        return slider;
    slider->set_skin(UaSkin::chromium_refresh);
    slider->set_color_scheme(state->scheme);
    if (state->accent)
        slider->set_accent_color(parse_css_color(*state->accent));
    // The range is set before the step and the value so neither is quantised
    // against Pulp's 0..1 default on the way in.
    slider->set_step(0.0f);
    slider->set_max(state->max);
    slider->set_min(state->min);
    slider->set_step(state->step);
    slider->set_value(state->value, Notify::none);
    if (state->disabled)
        slider->set_enabled(false);
    return slider;
}

void apply_ua_checkbox(Checkbox& checkbox, const IRNode& node) {
    const auto state = ua_form_control_state(node);
    if (!state || state->control == UaFormControlState::Control::range ||
        state->control == UaFormControlState::Control::button)
        return;
    checkbox.set_skin(UaSkin::chromium_refresh);
    checkbox.set_color_scheme(state->scheme);
    if (state->accent)
        checkbox.set_accent_color(parse_css_color(*state->accent));
    if (state->control == UaFormControlState::Control::radio) {
        checkbox.set_kind(Checkbox::Kind::radio);
        checkbox.set_radio_group(state->radio_group);
    }
    checkbox.set_checked(state->checked);
    if (state->disabled)
        checkbox.set_enabled(false);
}

std::unique_ptr<ButtonSurface> make_ua_button_surface(const IRNode& node) {
    auto surface = std::make_unique<ButtonSurface>();
    const auto state = ua_form_control_state(node);
    if (!state || state->control != UaFormControlState::Control::button)
        return surface;
    surface->set_color_scheme(state->scheme);
    if (state->disabled)
        surface->set_enabled(false);
    return surface;
}

} // namespace pulp::view
