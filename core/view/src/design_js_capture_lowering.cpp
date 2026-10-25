#include "design_js_capture_lowering.hpp"

#include "design_import_form_controls.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace pulp::view {

// Defined in design_codegen.cpp: escapes text for a single-quoted JS literal.
std::string js_single_quote_escape(const std::string& s);

namespace {

/// Shortest text that reads back as the same float, so the value the bridge
/// receives is the one the native lane computed.
std::string js_number(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    return buffer;
}

std::optional<float> attribute_number(const IRNode& node, const char* key) {
    const auto it = node.attributes.find(key);
    if (it == node.attributes.end() || it->second.empty())
        return std::nullopt;
    char* end = nullptr;
    const float value = std::strtof(it->second.c_str(), &end);
    if (end == it->second.c_str())
        return std::nullopt;
    return value;
}

} // namespace

bool emit_js_ua_form_control(std::ostream& out, const IRNode& node, const std::string& indent,
                             const std::string& id, const std::string& parent) {
    const auto state = ua_form_control_state(node);
    if (!state)
        return false;
    // The widget each control the native lane lowers becomes in ui.js. A control
    // this lowering has no case for (a theme-painted button, once the capture
    // records one) stays the plain frame the generic path builds rather than
    // becoming some other widget, so a new Control needs its case added here.
    // No default: the compiler names an unhandled Control.
    const char* factory = nullptr;
    switch (state->control) {
    case UaFormControlState::Control::range:
        factory = "createRangeSlider(";
        break;
    case UaFormControlState::Control::checkbox:
    case UaFormControlState::Control::radio:
        factory = "createCheckbox(";
        break;
    }
    if (!factory)
        return false;
    const bool range = state->control == UaFormControlState::Control::range;
    const std::string target = "'" + id + "'";
    out << indent << factory << target << ", " << parent << ");\n";
    out << indent << "setUaSkin(" << target << ", 'chromium_refresh', '"
        << (state->scheme == UaColorScheme::dark ? "dark" : "light") << "');\n";
    if (state->accent)
        out << indent << "setAccentColor(" << target << ", '"
            << js_single_quote_escape(*state->accent) << "');\n";
    if (range) {
        // As make_ua_range_slider(): the range is set before the step and the
        // value, so neither is quantised against the 0..1 default on the way in.
        out << indent << "setStep(" << target << ", 0);\n";
        out << indent << "setMax(" << target << ", " << js_number(state->max) << ");\n";
        out << indent << "setMin(" << target << ", " << js_number(state->min) << ");\n";
        out << indent << "setStep(" << target << ", " << js_number(state->step) << ");\n";
        out << indent << "setValue(" << target << ", " << js_number(state->value) << ");\n";
    } else {
        if (state->control == UaFormControlState::Control::radio) {
            out << indent << "setCheckboxKind(" << target << ", 'radio');\n";
            out << indent << "setRadioGroup(" << target << ", " << state->radio_group << ");\n";
        }
        out << indent << "setValue(" << target << ", " << (state->checked ? 1 : 0) << ");\n";
    }
    if (state->disabled)
        out << indent << "setEnabled(" << target << ", false);\n";
    return true;
}

void emit_js_captured_paint_rect(std::ostream& out, const IRNode& node, const std::string& indent,
                                 const std::string& id) {
    const float dpr = attribute_number(node, "browser_box_paint_dpr").value_or(0.0f);
    const float left = attribute_number(node, "browser_box_paint_left_px").value_or(0.0f);
    const float top = attribute_number(node, "browser_box_paint_top_px").value_or(0.0f);
    const float right = attribute_number(node, "browser_box_paint_right_px").value_or(0.0f);
    const float bottom = attribute_number(node, "browser_box_paint_bottom_px").value_or(0.0f);
    if (!(std::isfinite(dpr) && dpr > 0.0f && std::isfinite(left) && std::isfinite(top) &&
          std::isfinite(right) && std::isfinite(bottom) && right > left && bottom > top))
        return;
    out << indent << "setCapturedPaintRect('" << id << "', " << js_number(left / dpr) << ", "
        << js_number(top / dpr) << ", " << js_number(right / dpr) << ", " << js_number(bottom / dpr)
        << ", " << js_number(dpr) << ");\n";
}

} // namespace pulp::view
