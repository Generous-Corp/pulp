// SPDX-License-Identifier: MIT
#include "browser_capture_controls.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace pulp::import_design::detail {

double px_value(const std::string& value) {
    if (value.empty())
        return 0.0;
    try {
        return std::stod(value);
    } catch (const std::exception&) {
        return 0.0;
    }
}

std::string normalized_input_type(std::string raw) {
    // Every `type` keyword HTML defines. KNOWN_INPUT_TYPES in
    // browser_capture/form_control_theme.mjs and in
    // import-validation/importer_differential/form_controls.py repeat it;
    // test/test_importer_differential_lab.py asserts the three agree.
    static const std::set<std::string> kKnownInputTypes = {
        "hidden",   "text",  "search", "tel",      "url",   "email",
        "password", "date",  "month",  "week",     "time",  "datetime-local",
        "number",   "range", "color",  "checkbox", "radio", "file",
        "submit",   "image", "reset",  "button"};
    for (auto& c : raw) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return kKnownInputTypes.count(raw) != 0 ? raw : std::string("text");
}

bool record_ua_control_state(const CapturedStyleIndex& index, int node_index,
                             const std::map<std::string, std::string>& computed,
                             decltype(pulp::view::IRNode::attributes)& attributes) {
    // A <button>, and an <input> of a button type, is the theme's push button;
    // its content stays the node's children (a button input's label is the
    // #control-value run).
    const bool is_button_element = index.tag_name(node_index) == "button";
    const auto type = is_button_element
                          ? std::string("button")
                          : normalized_input_type(index.attribute(node_index, "type"));
    const bool push_button =
        is_button_element || type == "button" || type == "submit" || type == "reset";
    if (!push_button && type != "range" && type != "checkbox" && type != "radio")
        return false;
    attributes["ua_control"] = push_button ? std::string("button") : type;
    if (type == "range") {
        // Raw HTML strings: the widget applies HTML's own defaults and
        // sanitising, so an absent or invalid value is not guessed here.
        for (const char* name : {"min", "max", "step"}) {
            if (index.has_attribute(node_index, name))
                attributes[std::string("ua_") + name] = index.attribute(node_index, name);
        }
        // The live value; a script or a drag can leave it away from `value`.
        if (auto value = index.control_value(node_index); !value.empty())
            attributes["ua_value"] = std::move(value);
    } else if (!push_button) {
        attributes["ua_checked"] = index.control_checked(node_index) ? "1" : "0";
        if (type == "radio" && index.has_attribute(node_index, "name"))
            attributes["ua_radio_group"] = index.attribute(node_index, "name");
    }
    if (index.has_attribute(node_index, "disabled"))
        attributes["ua_disabled"] = "1";
    if (const auto accent = computed.find("accent-color");
        accent != computed.end() && accent->second != "auto" && !accent->second.empty())
        attributes["ua_accent"] = accent->second;
    if (const auto scheme = computed.find("color-scheme"); scheme != computed.end())
        attributes["ua_color_scheme"] = scheme->second;
    return true;
}

void drop_theme_replaced_box_paint(pulp::view::IRStyle& style) {
    style.background_color.reset();
    style.background_gradient.reset();
    style.background_image.reset();
    style.border.reset();
    style.border_color.reset();
    style.border_width.reset();
    style.border_style.reset();
    for (auto* color : {&style.border_top_color, &style.border_right_color,
                        &style.border_bottom_color, &style.border_left_color})
        color->reset();
    for (auto* width : {&style.border_top_width, &style.border_right_width,
                        &style.border_bottom_width, &style.border_left_width})
        width->reset();
}

std::optional<ControlValueText>
control_value_text(const CapturedStyleIndex& index, int node_index, const CapturedBox& box,
                   const std::map<std::string, std::string>& computed) {
    const auto tag = index.tag_name(node_index);
    std::string kind;
    if (tag == "input") {
        const auto type = normalized_input_type(index.attribute(node_index, "type"));
        // The input types whose value Chrome draws as a string in the
        // content box: the single-line text fields and the button-type
        // inputs. This is TEXT_INPUT_TYPES plus BUTTON_INPUT_TYPES in
        // browser_capture/form_control_theme.mjs without `password`: Chrome
        // paints a password value as one mask glyph per character, so drawing
        // the captured `inputValue` would both differ from the browser and
        // show the secret it hides. The picker types in
        // SHADOW_PART_INPUT_TYPES (import-validation/importer_differential/
        // form_controls.py) draw shadow-tree parts rather than a string and
        // belong to neither table. test/test_importer_differential_lab.py
        // asserts the three tables keep this relationship.
        static const std::set<std::string> kValueTypes = {
            "text", "search", "email", "url", "tel", "number", "button", "submit", "reset"};
        if (kValueTypes.count(type) == 0)
            return std::nullopt;
        kind = type;
    } else if (tag == "select") {
        if (!index.attribute(node_index, "multiple").empty())
            return std::nullopt;
        const auto size = index.attribute(node_index, "size");
        if (!size.empty() && px_value(size) > 1.0)
            return std::nullopt;
        const auto appearance = computed.find("appearance");
        if (appearance == computed.end() || appearance->second != "none")
            return std::nullopt;
        kind = "select";
    } else {
        return std::nullopt;
    }
    // A control under `-webkit-text-security` paints a mask glyph per
    // character, whatever its type, and its captured value is the secret the
    // mask hides. It is never drawn. A capture that predates the property
    // records no value for it and is read as unmasked, as it always was.
    if (const auto security = computed.find("-webkit-text-security");
        security != computed.end() && !security->second.empty() && security->second != "none")
        return std::nullopt;
    // Without padding the content box is unknown — a capture that predates it
    // would place the string on the border, so it is not drawn at all.
    for (const char* edge : {"padding-top", "padding-right", "padding-bottom", "padding-left"}) {
        if (computed.find(edge) == computed.end())
            return std::nullopt;
    }
    auto text = index.control_value(node_index);
    // A submit or reset button with no `value` attribute draws the user
    // agent's default label; one with an empty `value` draws nothing. The
    // default is localised in Chromium (the IDS_FORM_SUBMIT_LABEL and
    // IDS_FORM_RESET_LABEL resources), so these are the English strings,
    // which is what an en-US capture paints.
    if (text.empty() && tag == "input" && !index.has_attribute(node_index, "value")) {
        if (kind == "submit")
            text = "Submit";
        if (kind == "reset")
            text = "Reset";
    }
    if (text.empty())
        return std::nullopt;
    const auto length = [&](const char* name) {
        const auto found = computed.find(name);
        return found == computed.end() ? 0.0 : px_value(found->second);
    };
    const double left = length("border-left-width") + length("padding-left");
    const double right = length("border-right-width") + length("padding-right");
    const double top = length("border-top-width") + length("padding-top");
    const double bottom = length("border-bottom-width") + length("padding-bottom");
    ControlValueText result;
    result.text = std::move(text);
    result.content_box = {box.left + left, box.top + top, box.width - left - right,
                          box.height - top - bottom};
    if (result.content_box.width <= 0.0 || result.content_box.height <= 0.0)
        return std::nullopt;
    return result;
}

pulp::view::IRNode captured_viewport_plane(const std::string& owner_name, const std::string& asset,
                                           const CapturedLayerExtent& layer, const CapturedBox& box,
                                           const std::map<std::string, std::string>& computed) {
    const auto length = [&](const char* name) {
        const auto found = computed.find(name);
        return found == computed.end() ? 0.0 : px_value(found->second);
    };
    const double border_left = length("border-left-width");
    const double border_top = length("border-top-width");
    const double inset_left = border_left + length("padding-left");
    const double inset_top = border_top + length("padding-top");
    const double inset_right = length("border-right-width") + length("padding-right");
    const double inset_bottom = length("border-bottom-width") + length("padding-bottom");
    const double frame_left = box.left + border_left;
    const double frame_top = box.top + border_top;
    pulp::view::IRNode plane;
    plane.type = "image";
    plane.name = owner_name + "/layer";
    plane.attributes["asset_ref"] = asset;
    plane.attributes["captured_layer"] = "viewport";
    plane.attributes["paint_class"] = std::string(to_string(PaintClass::image_asset));
    plane.style.position = "absolute";
    plane.style.object_fit = "fill";
    plane.style.left = static_cast<float>(-frame_left);
    plane.style.top = static_cast<float>(-frame_top);
    plane.style.width = static_cast<float>(layer.width);
    plane.style.height = static_cast<float>(layer.height);
    pulp::view::IRStyle::ClipRect content{};
    content.x = static_cast<float>(box.left + inset_left);
    content.y = static_cast<float>(box.top + inset_top);
    content.width = static_cast<float>(std::max(0.0, box.width - inset_left - inset_right));
    content.height = static_cast<float>(std::max(0.0, box.height - inset_top - inset_bottom));
    plane.style.clip_rect = content;
    return plane;
}

} // namespace pulp::import_design::detail
