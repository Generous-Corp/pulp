#include "design_cpp_binding_codegen.hpp"

#include "design_binding_metadata.hpp"
#include "design_cpp_codegen_internal.hpp"
#include "design_ir_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::view {
namespace {

using cpp_codegen::cpp_string_literal;
using cpp_codegen::emit_line;
using cpp_codegen::format_float;

std::string json_string_escape(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (unsigned char c : input) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    static constexpr char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0xf];
                    out += kHex[c & 0xf];
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

std::string json_string_literal(std::string_view input) {
    return "\"" + json_string_escape(input) + "\"";
}

void append_json_field(std::ostringstream& out,
                       bool& first,
                       std::string_view key,
                       std::string_view value) {
    if (!first)
        out << ",";
    out << "\n      " << json_string_literal(key) << ": " << json_string_literal(value);
    first = false;
}

void append_json_field_if_present(std::ostringstream& out,
                                  bool& first,
                                  std::string_view key,
                                  const std::optional<std::string>& value) {
    if (value && !value->empty())
        append_json_field(out, first, key, *value);
}

bool node_has_binding_manifest_metadata(const IRNode& node) {
    for (std::string_view key : {
             "pulpRouteId",
             "pulpRouteType",
             "pulpSourceFamily",
             "pulpSourcePath",
             "pulpParamKey",
             "pulpBindingModule",
             "pulpBindingParam",
             "pulpChoiceValue",
             "pulpChoiceLabel",
             "pulpParamKeyX",
             "pulpParamKeyY",
             "pulpBindingModuleX",
             "pulpBindingParamX",
             "pulpBindingModuleY",
             "pulpBindingParamY",
             "pulpMeterSource",
             "pulpMeterChannel",
             "pulpMeterValueKey",
             "pulpWaveformShape",
             "pulpValueKey",
             "pulpInitialValue",
             "pulpPlaceholder",
             "pulpFocusContract",
             "pulpPayloadContract",
             "pulpHostActionLabel",
             "pulpTypeLabel",
             "pulpDescription",
             "pulpEventContract",
             "pulpGestureContract",
             "pulpHostAction",
             "pulpStyleTokens",
             "pulpDefaultValueSource",
             "pulpFallbackReason",
         }) {
        if (auto value = attr(node, key); value && !value->empty())
            return true;
    }
    return false;
}

struct BindingHelperRoute {
    NativeWidgetKind kind = NativeWidgetKind::view;
    std::string anchor_id;
    std::string route_id;
    std::string param_key;
    std::string binding_module;
    std::string binding_param;
    std::string choice_value;
    std::string choice_label;
    std::string x_param_key;
    std::string y_param_key;
    std::string x_binding_module;
    std::string x_binding_param;
    std::string y_binding_module;
    std::string y_binding_param;
    std::string meter_source;
    std::string meter_channel;
    std::string meter_value_key;
    std::string waveform_shape;
    std::string value_key;
    std::string initial_value;
    std::string placeholder;
    std::string focus_contract;
    std::string host_action;
    std::string host_action_label;
    std::string payload_contract;
    std::string event_contract;
    std::string gesture_contract;
    int segment_count = 0;
    double stepper_min = 0.0;
    double stepper_max = 1.0;
    double stepper_step = 1.0;
};

struct ResolvedBindingRoute {
    std::string ir_path;
    const IRNode* ir_node = nullptr;
    const ResolvedNativeNode* resolved = nullptr;
    NativeBindingMetadata metadata;
    bool eligible_for_manifest = false;
    bool eligible_for_helper = false;
};

struct ResolvedBindingPlan {
    std::vector<ResolvedBindingRoute> routes;
};

void collect_resolved_binding_plan(ResolvedBindingPlan& plan,
                                   const IRNode& node,
                                   const ResolvedNativeNode& resolved,
                                   std::string_view ir_path) {
    ResolvedBindingRoute route;
    route.ir_path = std::string(ir_path);
    route.ir_node = &node;
    route.resolved = &resolved;
    route.metadata = NativeBindingMetadata::parse(node);
    route.eligible_for_manifest = node_has_binding_manifest_metadata(node);

    const auto& md = route.metadata;
    const bool has_single_param = md.param_key && !md.param_key->empty();
    const bool has_scalar_param_control =
        (resolved.kind == NativeWidgetKind::knob ||
         resolved.kind == NativeWidgetKind::fader ||
         resolved.kind == NativeWidgetKind::checkbox ||
         resolved.kind == NativeWidgetKind::toggle_button ||
         resolved.kind == NativeWidgetKind::segmented ||
         resolved.kind == NativeWidgetKind::stepper) &&
        has_single_param;
    const bool has_choice_param = resolved.kind == NativeWidgetKind::toggle_button &&
        has_single_param && md.choice_value && !md.choice_value->empty();
    const bool has_xy_params = resolved.kind == NativeWidgetKind::xy_pad &&
        md.x_param_key && !md.x_param_key->empty() &&
        md.y_param_key && !md.y_param_key->empty();
    const bool has_meter_input = resolved.kind == NativeWidgetKind::meter &&
        md.meter_source && !md.meter_source->empty() &&
        md.meter_channel && !md.meter_channel->empty();
    const bool has_waveform_input = resolved.kind == NativeWidgetKind::waveform &&
        has_single_param && md.waveform_shape && !md.waveform_shape->empty();
    const bool has_text_input = resolved.kind == NativeWidgetKind::text_editor &&
        md.value_key && !md.value_key->empty();
    const bool has_host_action = resolved.kind == NativeWidgetKind::text_button &&
        md.host_action && !md.host_action->empty();
    route.eligible_for_helper =
        md.route_id && !md.route_id->empty() &&
        (has_scalar_param_control || has_choice_param || has_xy_params || has_meter_input ||
         has_waveform_input || has_text_input || has_host_action) &&
        node.stable_anchor_id && !node.stable_anchor_id->empty();

    plan.routes.push_back(std::move(route));

    const auto count = std::min(node.children.size(), resolved.children.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto child_path = std::string(ir_path) + "/" + std::to_string(i);
        collect_resolved_binding_plan(plan, node.children[i], resolved.children[i], child_path);
    }
}

void render_binding_manifest_entry(std::ostringstream& out,
                                   const ResolvedBindingRoute& route,
                                   bool& first_entry) {
    const IRNode& node = *route.ir_node;
    const ResolvedNativeNode& resolved = *route.resolved;
    const NativeBindingMetadata& md = route.metadata;
    if (!first_entry)
        out << ",";
    out << "\n    {";
    bool first_field = true;
    if (md.route_id && !md.route_id->empty()) {
        append_json_field(out, first_field, "id", *md.route_id);
    } else if (node.stable_anchor_id && !node.stable_anchor_id->empty()) {
        append_json_field(out, first_field, "id", *node.stable_anchor_id);
    } else if (!node.name.empty()) {
        append_json_field(out, first_field, "id", node.name);
    }
    append_json_field(out, first_field, "ir_path", route.ir_path);
    if (node.stable_anchor_id && !node.stable_anchor_id->empty())
        append_json_field(out, first_field, "anchor_id", *node.stable_anchor_id);
    append_json_field(out, first_field, "native_primitive", native_widget_kind_name(resolved.kind));
    append_json_field_if_present(out, first_field, "route_type", md.route_type);
    append_json_field_if_present(out, first_field, "source_family", md.source_family);
    append_json_field_if_present(out, first_field, "source_path", md.source_path);
    append_json_field_if_present(out, first_field, "param_key", md.param_key);
    append_json_field_if_present(out, first_field, "binding_module", md.binding_module);
    append_json_field_if_present(out, first_field, "binding_param", md.binding_param);
    append_json_field_if_present(out, first_field, "choice_value", md.choice_value);
    append_json_field_if_present(out, first_field, "choice_label", md.choice_label);
    append_json_field_if_present(out, first_field, "x_param_key", md.x_param_key);
    append_json_field_if_present(out, first_field, "y_param_key", md.y_param_key);
    append_json_field_if_present(out, first_field, "x_binding_module", md.x_binding_module);
    append_json_field_if_present(out, first_field, "x_binding_param", md.x_binding_param);
    append_json_field_if_present(out, first_field, "y_binding_module", md.y_binding_module);
    append_json_field_if_present(out, first_field, "y_binding_param", md.y_binding_param);
    append_json_field_if_present(out, first_field, "meter_source", md.meter_source);
    append_json_field_if_present(out, first_field, "meter_channel", md.meter_channel);
    append_json_field_if_present(out, first_field, "meter_value_key", md.meter_value_key);
    append_json_field_if_present(out, first_field, "waveform_shape", md.waveform_shape);
    append_json_field_if_present(out, first_field, "value_key", md.value_key);
    append_json_field_if_present(out, first_field, "initial_value", md.initial_value);
    append_json_field_if_present(out, first_field, "placeholder", md.placeholder);
    append_json_field_if_present(out, first_field, "focus_contract", md.focus_contract);
    append_json_field_if_present(out, first_field, "payload_contract", md.payload_contract);
    append_json_field_if_present(out, first_field, "host_action_label", md.host_action_label);
    append_json_field_if_present(out, first_field, "component_type_label", md.type_label);
    append_json_field_if_present(out, first_field, "description", md.description);
    append_json_field_if_present(out, first_field, "thumb_shape", md.thumb_shape);
    append_json_field_if_present(out, first_field, "thumb_width", md.thumb_width);
    append_json_field_if_present(out, first_field, "thumb_height", md.thumb_height);
    append_json_field_if_present(out, first_field, "thumb_corner_radius", md.thumb_corner_radius);
    append_json_field_if_present(out, first_field, "on_background_color", md.on_background_color);
    append_json_field_if_present(out, first_field, "off_background_color", md.off_background_color);
    append_json_field_if_present(out, first_field, "on_text_color", md.on_text_color);
    append_json_field_if_present(out, first_field, "off_text_color", md.off_text_color);
    append_json_field_if_present(out, first_field, "on_border_color", md.on_border_color);
    append_json_field_if_present(out, first_field, "off_border_color", md.off_border_color);
    append_json_field_if_present(out, first_field, "corner_radius", md.corner_radius);
    append_json_field_if_present(out, first_field, "font_size", md.font_size);
    append_json_field_if_present(out, first_field, "event_contract", md.event_contract);
    append_json_field_if_present(out, first_field, "gesture_contract", md.gesture_contract);
    append_json_field_if_present(out, first_field, "host_action", md.host_action);
    append_json_field_if_present(out, first_field, "style_tokens", md.style_tokens);
    append_json_field_if_present(out, first_field, "default_value_source", md.default_value_source);
    append_json_field_if_present(out, first_field, "fallback_reason", md.fallback_reason);
    out << "\n    }";
    first_entry = false;
}

std::string build_binding_manifest_json(const ResolvedBindingPlan& plan) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"pulp-native-cpp-binding-manifest-v1\",\n"
        << "  \"entries\": [";
    bool first_entry = true;
    for (const auto& route : plan.routes) {
        if (route.eligible_for_manifest)
            render_binding_manifest_entry(out, route, first_entry);
    }
    if (!first_entry)
        out << "\n  ";
    out << "]\n"
        << "}\n";
    return out.str();
}

std::vector<BindingHelperRoute> build_binding_helper_routes(const ResolvedBindingPlan& plan) {
    std::vector<BindingHelperRoute> routes;
    for (const auto& route : plan.routes) {
        if (!route.eligible_for_helper)
            continue;
        const NativeBindingMetadata& md = route.metadata;
        const auto semantics = imported_widget_semantics(*route.ir_node, *route.resolved);
        routes.push_back(BindingHelperRoute{
            .kind = route.resolved->kind,
            .anchor_id = *route.ir_node->stable_anchor_id,
            .route_id = *md.route_id,
            .param_key = md.param_key.value_or(std::string{}),
            .binding_module = md.binding_module.value_or(std::string{}),
            .binding_param = md.binding_param.value_or(std::string{}),
            .choice_value = md.choice_value.value_or(std::string{}),
            .choice_label = md.choice_label.value_or(std::string{}),
            .x_param_key = md.x_param_key.value_or(std::string{}),
            .y_param_key = md.y_param_key.value_or(std::string{}),
            .x_binding_module = md.x_binding_module.value_or(std::string{}),
            .x_binding_param = md.x_binding_param.value_or(std::string{}),
            .y_binding_module = md.y_binding_module.value_or(std::string{}),
            .y_binding_param = md.y_binding_param.value_or(std::string{}),
            .meter_source = md.meter_source.value_or(std::string{}),
            .meter_channel = md.meter_channel.value_or(std::string{}),
            .meter_value_key = md.meter_value_key.value_or(std::string{}),
            .waveform_shape = md.waveform_shape.value_or(std::string{}),
            .value_key = md.value_key.value_or(std::string{}),
            .initial_value = md.initial_value.value_or(std::string{}),
            .placeholder = md.placeholder.value_or(std::string{}),
            .focus_contract = md.focus_contract.value_or(std::string{}),
            .host_action = md.host_action.value_or(std::string{}),
            .host_action_label = md.host_action_label.value_or(std::string{}),
            .payload_contract = md.payload_contract.value_or(std::string{}),
            .event_contract = md.event_contract.value_or(std::string{}),
            .gesture_contract = md.gesture_contract.value_or(std::string{}),
            .segment_count = static_cast<int>(semantics.segments.size()),
            .stepper_min = route.ir_node->audio_min,
            .stepper_max = route.ir_node->audio_max,
            .stepper_step = semantics.stepper_step,
        });
    }
    return routes;
}

void emit_binding_context_helpers(std::ostringstream& out,
                                  const CppExportOptions& opts,
                                  const std::vector<BindingHelperRoute>& routes) {
    out << "namespace {\n"
        << "pulp::view::View* find_imported_view_by_anchor(pulp::view::View& root, std::string_view anchor, int& matches) {\n"
        << "    pulp::view::View* first = nullptr;\n"
        << "    if (root.anchor_id() == anchor) { first = &root; ++matches; }\n"
        << "    for (std::size_t i = 0; i < root.child_count(); ++i) {\n"
        << "        if (auto* found = find_imported_view_by_anchor(*root.child_at(i), anchor, matches); first == nullptr) first = found;\n"
        << "    }\n"
        << "    return first;\n"
        << "}\n"
        << "}  // namespace\n\n";

    out << "void " << opts.binding_function_name
        << "(pulp::view::View& root, pulp::view::NativeImportBindingContext& ctx) {\n";
    if (routes.empty()) {
        emit_line(out, 1, opts.indent_spaces, "(void)root;");
        emit_line(out, 1, opts.indent_spaces, "(void)ctx;");
        out << "}\n";
        return;
    }

    auto emit_descriptor = [&](const BindingHelperRoute& route, int depth) {
        emit_line(out, depth, opts.indent_spaces, "pulp::view::NativeImportBindingDescriptor{");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.route_id) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.param_key) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.binding_module) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.binding_param) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.event_contract) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.gesture_contract));
        emit_line(out, depth, opts.indent_spaces, "});");
    };

    auto emit_xy_descriptor = [&](const BindingHelperRoute& route, int depth) {
        emit_line(out, depth, opts.indent_spaces, "pulp::view::NativeImportXYPadBindingDescriptor{");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.route_id) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.x_param_key) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.y_param_key) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.x_binding_module) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.x_binding_param) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.y_binding_module) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.y_binding_param) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.event_contract) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.gesture_contract));
        emit_line(out, depth, opts.indent_spaces, "});");
    };

    auto emit_choice_descriptor = [&](const BindingHelperRoute& route, int depth) {
        emit_line(out, depth, opts.indent_spaces, "pulp::view::NativeImportChoiceBindingDescriptor{");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.route_id) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.param_key) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.choice_value) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.choice_label) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.event_contract) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.gesture_contract));
        emit_line(out, depth, opts.indent_spaces, "});");
    };

    auto emit_segmented_descriptor = [&](const BindingHelperRoute& route, int depth) {
        emit_line(out, depth, opts.indent_spaces, "pulp::view::NativeImportSegmentedBindingDescriptor{");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.route_id) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.param_key) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.binding_module) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.binding_param) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, std::to_string(route.segment_count));
        emit_line(out, depth, opts.indent_spaces, "});");
    };

    auto emit_stepper_descriptor = [&](const BindingHelperRoute& route, int depth) {
        emit_line(out, depth, opts.indent_spaces, "pulp::view::NativeImportStepperBindingDescriptor{");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.route_id) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.param_key) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.binding_module) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.binding_param) + ",");
        emit_line(out, depth + 1, opts.indent_spaces,
                  format_float(static_cast<float>(route.stepper_min)) + ",");
        emit_line(out, depth + 1, opts.indent_spaces,
                  format_float(static_cast<float>(route.stepper_max)) + ",");
        emit_line(out, depth + 1, opts.indent_spaces,
                  format_float(static_cast<float>(route.stepper_step)));
        emit_line(out, depth, opts.indent_spaces, "});");
    };

    auto emit_meter_descriptor = [&](const BindingHelperRoute& route, int depth) {
        emit_line(out, depth, opts.indent_spaces, "pulp::view::NativeImportMeterBindingDescriptor{");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.route_id) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.meter_source) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.meter_channel) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.meter_value_key) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.event_contract));
        emit_line(out, depth, opts.indent_spaces, "});");
    };

    auto emit_waveform_descriptor = [&](const BindingHelperRoute& route, int depth) {
        emit_line(out, depth, opts.indent_spaces, "pulp::view::NativeImportWaveformBindingDescriptor{");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.route_id) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.param_key) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.waveform_shape) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.event_contract));
        emit_line(out, depth, opts.indent_spaces, "});");
    };

    auto emit_text_descriptor = [&](const BindingHelperRoute& route, int depth) {
        emit_line(out, depth, opts.indent_spaces, "pulp::view::NativeImportTextBindingDescriptor{");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.route_id) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.value_key) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.initial_value) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.placeholder) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.event_contract) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.focus_contract));
        emit_line(out, depth, opts.indent_spaces, "});");
    };

    auto emit_host_action_descriptor = [&](const BindingHelperRoute& route, int depth) {
        emit_line(out, depth, opts.indent_spaces, "pulp::view::NativeImportHostActionDescriptor{");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.route_id) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.host_action) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.host_action_label) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.payload_contract) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.event_contract) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.gesture_contract));
        emit_line(out, depth, opts.indent_spaces, "});");
    };

    std::size_t route_index = 0;
    for (const auto& route : routes) {
        if (route.kind != NativeWidgetKind::knob &&
            route.kind != NativeWidgetKind::fader &&
            route.kind != NativeWidgetKind::meter &&
            route.kind != NativeWidgetKind::checkbox &&
            route.kind != NativeWidgetKind::toggle_button &&
            route.kind != NativeWidgetKind::segmented &&
            route.kind != NativeWidgetKind::stepper &&
            route.kind != NativeWidgetKind::xy_pad &&
            route.kind != NativeWidgetKind::waveform &&
            route.kind != NativeWidgetKind::text_editor &&
            route.kind != NativeWidgetKind::text_button) {
            ++route_index;
            continue;
        }
        const std::string route_var = "route_" + std::to_string(route_index++);
        emit_line(out, 1, opts.indent_spaces, "int " + route_var + "_match_count = 0;");
        emit_line(out, 1, opts.indent_spaces,
                  "if (auto* view = find_imported_view_by_anchor(root, " +
                      cpp_string_literal(route.anchor_id) + ", " + route_var +
                      "_match_count); view != nullptr && " + route_var +
                      "_match_count == 1 && ctx.claim_import_binding(*view, " +
                      cpp_string_literal(route.route_id) + ")) {");
        if (route.kind == NativeWidgetKind::knob) {
            emit_line(out, 2, opts.indent_spaces,
                      "if (auto* knob = dynamic_cast<pulp::view::Knob*>(view)) {");
            emit_line(out, 3, opts.indent_spaces, "ctx.bind_knob(*knob,");
            emit_descriptor(route, 3);
        } else {
            if (route.kind == NativeWidgetKind::segmented) {
                emit_line(out, 2, opts.indent_spaces,
                          "if (auto* segmented = dynamic_cast<pulp::view::SegmentedControl*>(view)) {");
                emit_line(out, 3, opts.indent_spaces, "ctx.bind_segmented(*segmented,");
                emit_segmented_descriptor(route, 3);
                emit_line(out, 2, opts.indent_spaces, "}");
                emit_line(out, 1, opts.indent_spaces, "}");
                continue;
            }
            if (route.kind == NativeWidgetKind::stepper) {
                emit_line(out, 2, opts.indent_spaces,
                          "if (auto* stepper = dynamic_cast<pulp::view::Stepper*>(view)) {");
                emit_line(out, 3, opts.indent_spaces, "ctx.bind_stepper(*stepper,");
                emit_stepper_descriptor(route, 3);
                emit_line(out, 2, opts.indent_spaces, "}");
                emit_line(out, 1, opts.indent_spaces, "}");
                continue;
            }
            if (route.kind == NativeWidgetKind::checkbox) {
                emit_line(out, 2, opts.indent_spaces,
                          "if (auto* checkbox = dynamic_cast<pulp::view::Checkbox*>(view)) {");
                emit_line(out, 3, opts.indent_spaces, "ctx.bind_checkbox(*checkbox,");
                emit_descriptor(route, 3);
                emit_line(out, 2, opts.indent_spaces, "}");
                emit_line(out, 1, opts.indent_spaces, "}");
                continue;
            }
            if (route.kind == NativeWidgetKind::xy_pad) {
                emit_line(out, 2, opts.indent_spaces,
                          "if (auto* pad = dynamic_cast<pulp::view::XYPad*>(view)) {");
                emit_line(out, 3, opts.indent_spaces, "ctx.bind_xy_pad(*pad,");
                emit_xy_descriptor(route, 3);
                emit_line(out, 2, opts.indent_spaces, "}");
                emit_line(out, 1, opts.indent_spaces, "}");
                continue;
            }
            if (route.kind == NativeWidgetKind::meter) {
                emit_line(out, 2, opts.indent_spaces,
                          "if (auto* meter = dynamic_cast<pulp::view::Meter*>(view)) {");
                emit_line(out, 3, opts.indent_spaces, "ctx.bind_meter(*meter,");
                emit_meter_descriptor(route, 3);
                emit_line(out, 2, opts.indent_spaces, "}");
                emit_line(out, 1, opts.indent_spaces, "}");
                continue;
            }
            if (route.kind == NativeWidgetKind::waveform) {
                emit_line(out, 2, opts.indent_spaces,
                          "if (auto* waveform = dynamic_cast<pulp::view::WaveformView*>(view)) {");
                emit_line(out, 3, opts.indent_spaces, "ctx.bind_waveform_display(*waveform,");
                emit_waveform_descriptor(route, 3);
                emit_line(out, 2, opts.indent_spaces, "}");
                emit_line(out, 1, opts.indent_spaces, "}");
                continue;
            }
            if (route.kind == NativeWidgetKind::text_editor) {
                emit_line(out, 2, opts.indent_spaces,
                          "if (auto* editor = dynamic_cast<pulp::view::TextEditor*>(view)) {");
                emit_line(out, 3, opts.indent_spaces, "ctx.bind_text_editor(*editor,");
                emit_text_descriptor(route, 3);
                emit_line(out, 2, opts.indent_spaces, "}");
                emit_line(out, 1, opts.indent_spaces, "}");
                continue;
            }
            if (route.kind == NativeWidgetKind::text_button) {
                emit_line(out, 2, opts.indent_spaces,
                          "if (auto* button = dynamic_cast<pulp::view::TextButton*>(view)) {");
                emit_line(out, 3, opts.indent_spaces, "ctx.bind_host_action(*button,");
                emit_host_action_descriptor(route, 3);
                emit_line(out, 2, opts.indent_spaces, "}");
                emit_line(out, 1, opts.indent_spaces, "}");
                continue;
            }
            if (route.kind == NativeWidgetKind::toggle_button) {
                emit_line(out, 2, opts.indent_spaces,
                          "if (auto* button = dynamic_cast<pulp::view::ToggleButton*>(view)) {");
                if (!route.choice_value.empty()) {
                    emit_line(out, 3, opts.indent_spaces, "ctx.bind_choice_button(*button,");
                    emit_choice_descriptor(route, 3);
                } else {
                    emit_line(out, 3, opts.indent_spaces, "ctx.bind_toggle_button(*button,");
                    emit_descriptor(route, 3);
                }
                emit_line(out, 2, opts.indent_spaces, "}");
                emit_line(out, 1, opts.indent_spaces, "}");
                continue;
            }
            emit_line(out, 2, opts.indent_spaces,
                      "if (auto* fader = dynamic_cast<pulp::view::Fader*>(view)) {");
            emit_line(out, 3, opts.indent_spaces, "ctx.bind_fader(*fader,");
            emit_descriptor(route, 3);
        }
        emit_line(out, 2, opts.indent_spaces, "}");
        emit_line(out, 1, opts.indent_spaces, "}");
    }
    out << "}\n";
}

}  // namespace

BindingCodegenArtifacts generate_cpp_binding_artifacts(
    const IRNode& root,
    const ResolvedNativeNode& resolved,
    const CppExportOptions& opts) {
    ResolvedBindingPlan plan;
    collect_resolved_binding_plan(plan, root, resolved, "root");

    BindingCodegenArtifacts artifacts;
    artifacts.binding_manifest = build_binding_manifest_json(plan);
    if (!opts.emit_binding_context_helpers)
        return artifacts;

    auto routes = build_binding_helper_routes(plan);
    artifacts.has_helpers = !routes.empty();
    if (artifacts.has_helpers) {
        std::ostringstream helper_source;
        emit_binding_context_helpers(helper_source, opts, routes);
        artifacts.helper_source = helper_source.str();
    }
    return artifacts;
}

}  // namespace pulp::view
