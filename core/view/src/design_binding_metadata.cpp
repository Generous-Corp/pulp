// design_binding_metadata.cpp — single parse/serialize path for the `pulp*`
// native-binding attribute contract. See design_binding_metadata.hpp.

#include "design_binding_metadata.hpp"

#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace pulp::view {

namespace {

// Reads attribute `key` off `node`. Returns the raw value (including empty
// strings) when present, std::nullopt when absent — mirrors the original
// attr() helpers in the consumers.
std::optional<std::string> read_attr(const IRNode& node, std::string_view key) {
    auto it = node.attributes.find(std::string(key));
    if (it == node.attributes.end())
        return std::nullopt;
    return it->second;
}

// Identical to the consumers' parse_float(): strtof, tolerate trailing
// whitespace, nullopt on non-numeric / empty input.
std::optional<float> parse_float(const std::string& text) {
    char* end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    if (end == text.c_str())
        return std::nullopt;
    while (*end != '\0') {
        if (!std::isspace(static_cast<unsigned char>(*end)))
            return std::nullopt;
        ++end;
    }
    return parsed;
}

std::optional<float> parse_float_opt(const std::optional<std::string>& raw) {
    if (!raw)
        return std::nullopt;
    return parse_float(*raw);
}

std::string lower_copy(std::string value) {
    for (auto& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

// set_contract_attr semantics from design_import_v0_tsx.cpp: skip empty
// values; only write when the node has no existing non-empty value for `key`.
void set_contract_attr(IRNode& node, const char* key, const std::optional<std::string>& value) {
    if (!value || value->empty())
        return;
    auto it = node.attributes.find(key);
    if (it == node.attributes.end() || it->second.empty())
        node.attributes[key] = *value;
}

}  // namespace

NativeBindingMetadata NativeBindingMetadata::parse(const IRNode& node) {
    NativeBindingMetadata md;

    md.route_id = read_attr(node, "pulpRouteId");
    md.route_type = read_attr(node, "pulpRouteType");
    md.source_family = read_attr(node, "pulpSourceFamily");
    md.source_path = read_attr(node, "pulpSourcePath");

    md.param_key = read_attr(node, "pulpParamKey");
    md.binding_module = read_attr(node, "pulpBindingModule");
    md.binding_param = read_attr(node, "pulpBindingParam");

    md.choice_value = read_attr(node, "pulpChoiceValue");
    md.choice_label = read_attr(node, "pulpChoiceLabel");

    md.x_param_key = read_attr(node, "pulpParamKeyX");
    md.y_param_key = read_attr(node, "pulpParamKeyY");
    md.x_binding_module = read_attr(node, "pulpBindingModuleX");
    md.x_binding_param = read_attr(node, "pulpBindingParamX");
    md.y_binding_module = read_attr(node, "pulpBindingModuleY");
    md.y_binding_param = read_attr(node, "pulpBindingParamY");

    md.meter_source = read_attr(node, "pulpMeterSource");
    md.meter_channel = read_attr(node, "pulpMeterChannel");
    md.meter_value_key = read_attr(node, "pulpMeterValueKey");
    md.waveform_shape = read_attr(node, "pulpWaveformShape");

    md.value_key = read_attr(node, "pulpValueKey");
    md.initial_value = read_attr(node, "pulpInitialValue");
    md.placeholder = read_attr(node, "pulpPlaceholder");
    md.default_value_source = read_attr(node, "pulpDefaultValueSource");

    md.focus_contract = read_attr(node, "pulpFocusContract");
    md.payload_contract = read_attr(node, "pulpPayloadContract");
    md.host_action = read_attr(node, "pulpHostAction");
    md.host_action_label = read_attr(node, "pulpHostActionLabel");
    md.type_label = read_attr(node, "pulpTypeLabel");
    md.description = read_attr(node, "pulpDescription");
    md.event_contract = read_attr(node, "pulpEventContract");
    md.gesture_contract = read_attr(node, "pulpGestureContract");
    md.style_tokens = read_attr(node, "pulpStyleTokens");
    md.fallback_reason = read_attr(node, "pulpFallbackReason");
    md.widget_schema = read_attr(node, "pulpWidgetSchema");

    md.grid_template_columns = read_attr(node, "pulpGridTemplateColumns");
    md.grid_template_rows = read_attr(node, "pulpGridTemplateRows");

    md.thumb_width = read_attr(node, "pulpThumbWidth");
    md.thumb_height = read_attr(node, "pulpThumbHeight");
    md.thumb_corner_radius = read_attr(node, "pulpThumbCornerRadius");
    md.corner_radius = read_attr(node, "pulpCornerRadius");
    md.font_size = read_attr(node, "pulpFontSize");

    md.thumb_shape = read_attr(node, "pulpThumbShape");
    md.on_background_color = read_attr(node, "pulpOnBackgroundColor");
    md.off_background_color = read_attr(node, "pulpOffBackgroundColor");
    md.on_text_color = read_attr(node, "pulpOnTextColor");
    md.off_text_color = read_attr(node, "pulpOffTextColor");
    md.on_border_color = read_attr(node, "pulpOnBorderColor");
    md.off_border_color = read_attr(node, "pulpOffBorderColor");

    md.show_internal_label = read_attr(node, "pulpShowInternalLabel");
    md.hit_testable = read_attr(node, "pulpHitTestable");

    return md;
}

void NativeBindingMetadata::serialize(IRNode& node) const {
    set_contract_attr(node, "pulpRouteId", route_id);
    set_contract_attr(node, "pulpRouteType", route_type);
    set_contract_attr(node, "pulpSourceFamily", source_family);
    set_contract_attr(node, "pulpSourcePath", source_path);

    set_contract_attr(node, "pulpParamKey", param_key);
    set_contract_attr(node, "pulpBindingModule", binding_module);
    set_contract_attr(node, "pulpBindingParam", binding_param);

    set_contract_attr(node, "pulpChoiceValue", choice_value);
    set_contract_attr(node, "pulpChoiceLabel", choice_label);

    set_contract_attr(node, "pulpParamKeyX", x_param_key);
    set_contract_attr(node, "pulpParamKeyY", y_param_key);
    set_contract_attr(node, "pulpBindingModuleX", x_binding_module);
    set_contract_attr(node, "pulpBindingParamX", x_binding_param);
    set_contract_attr(node, "pulpBindingModuleY", y_binding_module);
    set_contract_attr(node, "pulpBindingParamY", y_binding_param);

    set_contract_attr(node, "pulpMeterSource", meter_source);
    set_contract_attr(node, "pulpMeterChannel", meter_channel);
    set_contract_attr(node, "pulpMeterValueKey", meter_value_key);
    set_contract_attr(node, "pulpWaveformShape", waveform_shape);

    set_contract_attr(node, "pulpValueKey", value_key);
    set_contract_attr(node, "pulpInitialValue", initial_value);
    set_contract_attr(node, "pulpPlaceholder", placeholder);
    set_contract_attr(node, "pulpDefaultValueSource", default_value_source);

    set_contract_attr(node, "pulpFocusContract", focus_contract);
    set_contract_attr(node, "pulpPayloadContract", payload_contract);
    set_contract_attr(node, "pulpHostAction", host_action);
    set_contract_attr(node, "pulpHostActionLabel", host_action_label);
    set_contract_attr(node, "pulpTypeLabel", type_label);
    set_contract_attr(node, "pulpDescription", description);
    set_contract_attr(node, "pulpEventContract", event_contract);
    set_contract_attr(node, "pulpGestureContract", gesture_contract);
    set_contract_attr(node, "pulpStyleTokens", style_tokens);
    set_contract_attr(node, "pulpFallbackReason", fallback_reason);
    set_contract_attr(node, "pulpWidgetSchema", widget_schema);

    set_contract_attr(node, "pulpGridTemplateColumns", grid_template_columns);
    set_contract_attr(node, "pulpGridTemplateRows", grid_template_rows);

    set_contract_attr(node, "pulpThumbWidth", thumb_width);
    set_contract_attr(node, "pulpThumbHeight", thumb_height);
    set_contract_attr(node, "pulpThumbCornerRadius", thumb_corner_radius);
    set_contract_attr(node, "pulpCornerRadius", corner_radius);
    set_contract_attr(node, "pulpFontSize", font_size);

    set_contract_attr(node, "pulpThumbShape", thumb_shape);
    set_contract_attr(node, "pulpOnBackgroundColor", on_background_color);
    set_contract_attr(node, "pulpOffBackgroundColor", off_background_color);
    set_contract_attr(node, "pulpOnTextColor", on_text_color);
    set_contract_attr(node, "pulpOffTextColor", off_text_color);
    set_contract_attr(node, "pulpOnBorderColor", on_border_color);
    set_contract_attr(node, "pulpOffBorderColor", off_border_color);

    set_contract_attr(node, "pulpShowInternalLabel", show_internal_label);
    set_contract_attr(node, "pulpHitTestable", hit_testable);
}

std::optional<float> NativeBindingMetadata::thumb_width_value() const {
    return parse_float_opt(thumb_width);
}

std::optional<float> NativeBindingMetadata::thumb_height_value() const {
    return parse_float_opt(thumb_height);
}

std::optional<float> NativeBindingMetadata::thumb_corner_radius_value() const {
    return parse_float_opt(thumb_corner_radius);
}

std::optional<float> NativeBindingMetadata::corner_radius_value() const {
    return parse_float_opt(corner_radius);
}

std::optional<float> NativeBindingMetadata::font_size_value() const {
    return parse_float_opt(font_size);
}

bool NativeBindingMetadata::show_internal_label_value() const {
    if (show_internal_label && lower_copy(*show_internal_label) == "false")
        return false;
    return true;
}

}  // namespace pulp::view
