#include "design_import_native_common.hpp"
#include "design_ir_helpers.hpp"

#include "design_binding_metadata.hpp"

#include <pulp/view/buttons.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/css_gradient.hpp>
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/svg_path_widget.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/widgets/svg_line.hpp>
#include <pulp/view/widgets/svg_rect.hpp>

#include <pulp/runtime/base64.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::view {
namespace {

bool ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

std::string node_id(const IRNode& node, std::string_view path) {
    if (node.stable_anchor_id && !node.stable_anchor_id->empty())
        return *node.stable_anchor_id;
    if (auto id = attr(node, "id"); id && !id->empty())
        return *id;
    if (node.source_node_id && !node.source_node_id->empty())
        return *node.source_node_id;
    if (!node.name.empty())
        return node.name;
    return std::string(path);
}

ImportDiagnostic diagnostic(ImportDiagnosticSeverity severity,
                            ImportDiagnosticKind kind,
                            std::string code,
                            std::string path,
                            std::string message,
                            const IRNode& node,
                            std::optional<std::string> property = std::nullopt) {
    ImportDiagnostic out;
    out.severity = severity;
    out.kind = kind;
    out.code = std::move(code);
    out.path = std::move(path);
    out.message = std::move(message);
    out.property = std::move(property);
    if (node.stable_anchor_id && !node.stable_anchor_id->empty())
        out.anchor_id = *node.stable_anchor_id;
    return out;
}

bool has_text(const std::optional<std::string>& value) {
    return value && !value->empty();
}

std::string_view text_or_empty(const std::optional<std::string>& value) {
    return value ? std::string_view(*value) : std::string_view{};
}

struct AnchorLookupResult {
    View* first = nullptr;
    std::size_t matches = 0;
};

struct BindingClaim {
    const View* view = nullptr;
    std::uint64_t view_instance_id = 0;
    std::weak_ptr<const std::uint64_t> view_lifetime;
    std::string route_id;
};

std::mutex& binding_claims_mutex() {
    static auto* mutex = new std::mutex();
    return *mutex;
}

std::unordered_map<const NativeImportBindingContext*, std::vector<BindingClaim>>& binding_claims_by_context() {
    // Process-lifetime storage keeps NativeImportBindingContext destruction
    // safe for static/global contexts during process shutdown.
    static auto* claims = new std::unordered_map<const NativeImportBindingContext*, std::vector<BindingClaim>>();
    return *claims;
}

void collect_imported_views_by_anchor(View& root,
                                      std::string_view anchor,
                                      AnchorLookupResult& result) {
    if (root.anchor_id() == anchor) {
        if (result.first == nullptr)
            result.first = &root;
        ++result.matches;
    }
    for (std::size_t i = 0; i < root.child_count(); ++i) {
        collect_imported_views_by_anchor(*root.child_at(i), anchor, result);
    }
}

AnchorLookupResult find_imported_views_by_anchor(View& root, std::string_view anchor) {
    AnchorLookupResult result;
    collect_imported_views_by_anchor(root, anchor, result);
    return result;
}

NativeImportBindingDescriptor scalar_descriptor(const NativeBindingMetadata& md) {
    return NativeImportBindingDescriptor{
        .route_id = text_or_empty(md.route_id),
        .param_key = text_or_empty(md.param_key),
        .binding_module = text_or_empty(md.binding_module),
        .binding_param = text_or_empty(md.binding_param),
        .event_contract = text_or_empty(md.event_contract),
        .gesture_contract = text_or_empty(md.gesture_contract)};
}

bool has_route_required_binding_payload(const NativeBindingMetadata& md) {
    return has_text(md.param_key) ||
           has_text(md.x_param_key) ||
           has_text(md.y_param_key) ||
           has_text(md.meter_source) ||
           has_text(md.meter_channel) ||
           has_text(md.meter_value_key) ||
           has_text(md.waveform_shape) ||
           has_text(md.value_key) ||
           has_text(md.host_action);
}

bool has_binding_payload(const NativeBindingMetadata& md) {
    return has_route_required_binding_payload(md) || has_text(md.initial_value);
}

void append_binding_diagnostic(std::vector<ImportDiagnostic>* diagnostics,
                               const IRNode& node,
                               std::string_view path,
                               std::string code,
                               std::string message,
                               std::optional<std::string> property = std::nullopt) {
    if (diagnostics == nullptr) return;
    diagnostics->push_back(diagnostic(
        ImportDiagnosticSeverity::warning,
        ImportDiagnosticKind::unsupported_property,
        std::move(code),
        std::string(path),
        std::move(message),
        node,
        std::move(property)));
}

bool bind_imported_view(View& view,
                        const NativeBindingMetadata& md,
                        NativeImportBindingContext& ctx) {
    if (auto* knob = dynamic_cast<Knob*>(&view); knob && has_text(md.param_key)) {
        ctx.bind_knob(*knob, scalar_descriptor(md));
        return true;
    }
    if (auto* fader = dynamic_cast<Fader*>(&view); fader && has_text(md.param_key)) {
        ctx.bind_fader(*fader, scalar_descriptor(md));
        return true;
    }
    if (auto* checkbox = dynamic_cast<Checkbox*>(&view); checkbox && has_text(md.param_key)) {
        ctx.bind_checkbox(*checkbox, scalar_descriptor(md));
        return true;
    }
    if (auto* pad = dynamic_cast<XYPad*>(&view);
        pad && has_text(md.x_param_key) && has_text(md.y_param_key)) {
        ctx.bind_xy_pad(*pad,
                        NativeImportXYPadBindingDescriptor{
                            .route_id = text_or_empty(md.route_id),
                            .x_param_key = text_or_empty(md.x_param_key),
                            .y_param_key = text_or_empty(md.y_param_key),
                            .x_binding_module = text_or_empty(md.x_binding_module),
                            .x_binding_param = text_or_empty(md.x_binding_param),
                            .y_binding_module = text_or_empty(md.y_binding_module),
                            .y_binding_param = text_or_empty(md.y_binding_param),
                            .event_contract = text_or_empty(md.event_contract),
                            .gesture_contract = text_or_empty(md.gesture_contract)});
        return true;
    }
    if (auto* meter = dynamic_cast<Meter*>(&view);
        meter && has_text(md.meter_source) && has_text(md.meter_channel)) {
        ctx.bind_meter(*meter,
                       NativeImportMeterBindingDescriptor{
                           .route_id = text_or_empty(md.route_id),
                           .meter_source = text_or_empty(md.meter_source),
                           .channel = text_or_empty(md.meter_channel),
                           .value_key = text_or_empty(md.meter_value_key),
                           .event_contract = text_or_empty(md.event_contract)});
        return true;
    }
    if (auto* waveform = dynamic_cast<WaveformView*>(&view);
        waveform && has_text(md.param_key) && has_text(md.waveform_shape)) {
        ctx.bind_waveform_display(*waveform,
                                  NativeImportWaveformBindingDescriptor{
                                      .route_id = text_or_empty(md.route_id),
                                      .param_key = text_or_empty(md.param_key),
                                      .shape = text_or_empty(md.waveform_shape),
                                      .event_contract = text_or_empty(md.event_contract)});
        return true;
    }
    if (auto* editor = dynamic_cast<TextEditor*>(&view);
        editor && (has_text(md.value_key) || has_text(md.initial_value))) {
        ctx.bind_text_editor(*editor,
                             NativeImportTextBindingDescriptor{
                                 .route_id = text_or_empty(md.route_id),
                                 .value_key = text_or_empty(md.value_key),
                                 .initial_value = text_or_empty(md.initial_value),
                                 .placeholder = text_or_empty(md.placeholder),
                                 .event_contract = text_or_empty(md.event_contract),
                                 .focus_contract = text_or_empty(md.focus_contract)});
        return true;
    }
    if (auto* text_button = dynamic_cast<TextButton*>(&view);
        text_button && has_text(md.host_action)) {
        ctx.bind_host_action(*text_button,
                             NativeImportHostActionDescriptor{
                                 .route_id = text_or_empty(md.route_id),
                                 .action = text_or_empty(md.host_action),
                                 .label = text_or_empty(md.host_action_label),
                                 .payload_contract = text_or_empty(md.payload_contract),
                                 .event_contract = text_or_empty(md.event_contract),
                                 .gesture_contract = text_or_empty(md.gesture_contract)});
        return true;
    }
    if (auto* toggle = dynamic_cast<ToggleButton*>(&view);
        toggle && has_text(md.param_key)) {
        if (has_text(md.choice_value)) {
            ctx.bind_choice_button(*toggle,
                                   NativeImportChoiceBindingDescriptor{
                                       .route_id = text_or_empty(md.route_id),
                                       .param_key = text_or_empty(md.param_key),
                                       .choice_value = text_or_empty(md.choice_value),
                                       .choice_label = text_or_empty(md.choice_label),
                                       .event_contract = text_or_empty(md.event_contract),
                                       .gesture_contract = text_or_empty(md.gesture_contract)});
        } else {
            ctx.bind_toggle_button(*toggle, scalar_descriptor(md));
        }
        return true;
    }
    return false;
}

bool can_bind_imported_view(View& view, const NativeBindingMetadata& md) {
    if (dynamic_cast<Knob*>(&view) && has_text(md.param_key))
        return true;
    if (dynamic_cast<Fader*>(&view) && has_text(md.param_key))
        return true;
    if (dynamic_cast<Checkbox*>(&view) && has_text(md.param_key))
        return true;
    if (dynamic_cast<XYPad*>(&view) && has_text(md.x_param_key) && has_text(md.y_param_key))
        return true;
    if (dynamic_cast<Meter*>(&view) && has_text(md.meter_source) && has_text(md.meter_channel))
        return true;
    if (dynamic_cast<WaveformView*>(&view) && has_text(md.param_key) && has_text(md.waveform_shape))
        return true;
    if (dynamic_cast<TextEditor*>(&view) && (has_text(md.value_key) || has_text(md.initial_value)))
        return true;
    if (dynamic_cast<TextButton*>(&view) && has_text(md.host_action))
        return true;
    if (dynamic_cast<ToggleButton*>(&view) && has_text(md.param_key))
        return true;
    return false;
}

void bind_imported_node_by_anchor(View& root,
                                  const IRNode& node,
                                  NativeImportBindingContext& ctx,
                                  std::string_view path,
                                  std::vector<ImportDiagnostic>* diagnostics) {
    const auto md = NativeBindingMetadata::parse(node);
    if (has_binding_payload(md)) {
        if (!has_text(md.route_id)) {
            if (has_route_required_binding_payload(md)) {
                append_binding_diagnostic(
                    diagnostics,
                    node,
                    path,
                    "native-binding-missing-route",
                    "binding metadata has no pulpRouteId, so no binding callback was installed",
                    "pulpRouteId");
            }
        } else if (!node.stable_anchor_id || node.stable_anchor_id->empty()) {
            append_binding_diagnostic(
                diagnostics,
                node,
                path,
                "native-binding-missing-anchor",
                "binding metadata cannot bind without a stable anchor",
                "stable_anchor_id");
        } else {
            const auto matches = find_imported_views_by_anchor(root, *node.stable_anchor_id);
            if (matches.matches > 1) {
                append_binding_diagnostic(
                    diagnostics,
                    node,
                    path,
                    "native-binding-duplicate-anchor",
                    "binding anchor '" + *node.stable_anchor_id +
                        "' matched multiple materialized native views, so no binding callback was installed",
                    "stable_anchor_id");
            } else if (matches.first != nullptr) {
                if (!can_bind_imported_view(*matches.first, md)) {
                    append_binding_diagnostic(
                        diagnostics,
                        node,
                        path,
                        "native-binding-not-applied",
                        "binding metadata did not match the materialized widget type or required fields");
                } else if (!ctx.claim_import_binding(*matches.first, *md.route_id)) {
                    // claim_import_binding() happens before callback installation;
                    // keep can_bind_imported_view() in sync with bind_imported_view().
                    append_binding_diagnostic(
                        diagnostics,
                        node,
                        path,
                        "native-binding-already-applied",
                        "binding metadata for route '" + *md.route_id +
                            "' was already applied to this materialized native view",
                        "pulpRouteId");
                } else if (!bind_imported_view(*matches.first, md, ctx)) {
                    append_binding_diagnostic(
                        diagnostics,
                        node,
                        path,
                        "native-binding-not-applied",
                        "binding metadata did not match the materialized widget type or required fields");
                }
            } else {
                append_binding_diagnostic(
                    diagnostics,
                    node,
                    path,
                    "native-binding-anchor-not-found",
                    "no materialized native view found for binding anchor '" +
                        *node.stable_anchor_id + "'");
            }
        }
    }

    for (std::size_t i = 0; i < node.children.size(); ++i) {
        std::ostringstream child_path;
        child_path << path << "/children[" << i << "]";
        bind_imported_node_by_anchor(root, node.children[i], ctx, child_path.str(), diagnostics);
    }
}

std::optional<NativeWidgetKind> kind_from_audio(AudioWidgetType audio_widget) {
    switch (audio_widget) {
        case AudioWidgetType::knob: return NativeWidgetKind::knob;
        case AudioWidgetType::fader: return NativeWidgetKind::fader;
        case AudioWidgetType::meter: return NativeWidgetKind::meter;
        case AudioWidgetType::xy_pad: return NativeWidgetKind::xy_pad;
        case AudioWidgetType::waveform: return NativeWidgetKind::waveform;
        case AudioWidgetType::spectrum: return NativeWidgetKind::spectrum;
        case AudioWidgetType::none: break;
    }
    return std::nullopt;
}

NativeWidgetKind input_kind(const IRNode& node,
                            std::string_view path,
                            std::vector<ImportDiagnostic>& diagnostics) {
    auto type = attr(node, "type");
    if (!type) type = attr(node, "inputType");
    if (!type) type = attr(node, "html_type");

    const auto subtype = lower_copy(type.value_or("text"));
    if (subtype == "range") return NativeWidgetKind::fader;
    if (subtype == "checkbox") return NativeWidgetKind::checkbox;
    if (subtype == "text" || subtype == "search" || subtype == "email" ||
        subtype == "password" || subtype == "number") {
        return NativeWidgetKind::text_editor;
    }

    diagnostics.push_back(diagnostic(
        ImportDiagnosticSeverity::warning,
        ImportDiagnosticKind::unsupported_property,
        "native-unsupported-input-type",
        std::string(path),
        "input[type=\"" + subtype + "\"] falls back to TextEditor",
        node,
        "type"));
    return NativeWidgetKind::text_editor;
}

std::optional<NativeWidgetKind> kind_from_type(const IRNode& node,
                                               std::string_view path,
                                               std::vector<ImportDiagnostic>& diagnostics) {
    const auto type = lower_copy(node.type);
    if (type.empty()) return std::nullopt;

    if (type == "frame" || type == "view" || type == "div" || type == "section" ||
        type == "group" || type == "container" || type == "stack") {
        return NativeWidgetKind::view;
    }
    if (type == "text" || type == "label" || type == "span" || type == "p")
        return NativeWidgetKind::label;
    if (type == "button" || type == "text_button")
        return NativeWidgetKind::text_button;
    if (type == "toggle_button" || type == "toggle" || type == "switch")
        return NativeWidgetKind::toggle_button;
    if (type == "textarea" || type == "text_editor")
        return NativeWidgetKind::text_editor;
    if (type == "checkbox")
        return NativeWidgetKind::checkbox;
    if (type == "input")
        return input_kind(node, path, diagnostics);
    // Ink & Signal design-system vocabulary: a ComboBox in the library (and the
    // common web select/dropdown idioms) all materialize as the native ComboBox.
    // combo_box had no recognized type string before this — a real gap.
    if (type == "combobox" || type == "combo_box" || type == "dropdown" ||
        type == "select")
        return NativeWidgetKind::combo_box;
    if (type == "slider" || type == "range")
        return NativeWidgetKind::fader;
    // The 1-D PanControl is a linear value control; map it onto the native Fader
    // until it earns a dedicated NativeWidgetKind (see vocabulary note below).
    if (type == "pan" || type == "panner")
        return NativeWidgetKind::fader;
    // A Badge is a compact text pill; the faithful native carrier is a Label.
    if (type == "badge" || type == "chip" || type == "tag" || type == "pill")
        return NativeWidgetKind::label;
    // Structural containers from the design system collapse onto the generic
    // native View (their children import recursively).
    if (type == "panel" || type == "sidebar" || type == "side_panel" ||
        type == "toolbar" || type == "channel_strip" || type == "card")
        return NativeWidgetKind::view;
    if (type == "knob")
        return NativeWidgetKind::knob;
    if (type == "fader")
        return NativeWidgetKind::fader;
    if (type == "meter")
        return NativeWidgetKind::meter;
    if (type == "xy_pad" || type == "xypad")
        return NativeWidgetKind::xy_pad;
    if (type == "waveform")
        return NativeWidgetKind::waveform;
    if (type == "spectrum")
        return NativeWidgetKind::spectrum;
    if (type == "image" || type == "img")
        return NativeWidgetKind::image_view;
    if (type == "canvas")
        return NativeWidgetKind::canvas;
    if (type == "svg_path" || type == "path")
        return NativeWidgetKind::svg_path;
    if (type == "svg_rect" || type == "rect")
        return NativeWidgetKind::svg_rect;
    if (type == "svg_line" || type == "line")
        return NativeWidgetKind::svg_line;

    return std::nullopt;
}

std::optional<std::string> text_for_node(const IRNode& node, NativeWidgetKind kind) {
    if (!node.text_content.empty()) return node.text_content;
    if ((kind == NativeWidgetKind::knob || kind == NativeWidgetKind::fader ||
         kind == NativeWidgetKind::meter || kind == NativeWidgetKind::xy_pad ||
         kind == NativeWidgetKind::waveform || kind == NativeWidgetKind::spectrum) &&
        !node.audio_label.empty()) {
        return node.audio_label;
    }
    if (auto label = attr(node, "aria-label"); label && !label->empty()) return *label;
    if (auto label = attr(node, "label"); label && !label->empty()) return *label;
    return std::nullopt;
}

using AssetIndex = std::unordered_map<std::string, const IRAssetRef*>;

AssetIndex index_assets(const IRAssetManifest& manifest) {
    AssetIndex out;
    for (const auto& asset : manifest.assets) {
        if (!asset.asset_id.empty())
            out.emplace(asset.asset_id, &asset);
    }
    return out;
}

void append_asset_diagnostics(const IRNode& node,
                              std::string_view path,
                              const AssetIndex& assets,
                              std::vector<ImportDiagnostic>& diagnostics) {
    std::vector<std::pair<std::string, std::string>> asset_attributes;
    for (const auto& [key, value] : node.attributes) {
        if (ends_with(key, "AssetId") && !value.empty())
            asset_attributes.emplace_back(key, value);
    }
    std::sort(asset_attributes.begin(), asset_attributes.end());

    for (const auto& [key, value] : asset_attributes) {

        auto found = assets.find(value);
        if (found == assets.end()) {
            diagnostics.push_back(diagnostic(
                ImportDiagnosticSeverity::warning,
                ImportDiagnosticKind::unresolved_asset,
                "native-missing-asset",
                std::string(path),
                "asset id '" + value + "' is not present in the asset manifest",
                node,
                key));
            continue;
        }

        for (auto asset_diagnostic : found->second->diagnostics) {
            if (asset_diagnostic.path.empty()) asset_diagnostic.path = std::string(path);
            if (!asset_diagnostic.property) asset_diagnostic.property = key;
            if (!asset_diagnostic.anchor_id && node.stable_anchor_id)
                asset_diagnostic.anchor_id = *node.stable_anchor_id;
            diagnostics.push_back(std::move(asset_diagnostic));
        }
    }
}

void append_unsupported_property_diagnostics(const IRNode& node,
                                             std::string_view path,
                                             std::vector<ImportDiagnostic>& diagnostics) {
    auto add = [&](const char* property, const std::optional<std::string>& value) {
        if (!value || value->empty()) return;
        diagnostics.push_back(diagnostic(
            ImportDiagnosticSeverity::warning,
            ImportDiagnosticKind::unsupported_property,
            "native-unsupported-property",
            std::string(path),
            std::string(property) + " is not represented by the native resolver yet",
            node,
            property));
    };

    add("backgroundGradient", node.style.background_gradient);
    if (!node.style.box_shadow.empty())
        add("boxShadow", box_shadow_to_css(node.style.box_shadow));
    add("filter", node.style.filter);
    add("backdropFilter", node.style.backdrop_filter);
    add("transform", node.style.transform);

    if (node.style.position &&
        (*node.style.position == "fixed" || *node.style.position == "sticky")) {
        diagnostics.push_back(diagnostic(
            ImportDiagnosticSeverity::warning,
            ImportDiagnosticKind::unsupported_property,
            "native-unsupported-property",
            std::string(path),
            "position '" + *node.style.position + "' is not represented by the native resolver yet",
            node,
            "position"));
    }
}

// The first text node anywhere under `node` with non-empty content (a dropdown's
// selected value, a search field's placeholder) — returns the node for its style.
const IRNode* first_text_descendant_node(const IRNode& node) {
    for (const auto& c : node.children)
        if (lower_copy(c.type) == "text" && !c.text_content.empty())
            return &c;
    for (const auto& c : node.children)
        if (auto* t = first_text_descendant_node(c)) return t;
    return nullptr;
}

std::optional<std::string> first_text_descendant(const IRNode& node) {
    if (auto* t = first_text_descendant_node(node)) return t->text_content;
    return std::nullopt;
}

// True when this frame is a search field: it wraps a "Search" text (the
// placeholder), typically with a magnifier icon and a background pill. The WHOLE
// box becomes the input, not just the inner text cell.
bool is_search_container(const IRNode& node) {
    if (lower_copy(node.type) != "frame") return false;
    for (const auto& c : node.children) {
        const auto cn = lower_copy(c.name);
        if (lower_copy(c.type) == "text" &&
            (cn == "search" || cn == "searchbox" || cn == "search field"))
            return true;
    }
    return false;
}

// A "Dropdown"-named frame is a REAL dropdown only when it carries a selected
// value AND a single down-chevron. ELYSIUM reuses the "Dropdown" name for two
// other things that must NOT become ComboBoxes:
//   - an unconfigured design-system TEMPLATE whose value is the literal word
//     "Dropdown" (the stray "VST Style" placeholder);
//   - a prev/next preset CYCLER, whose icon is a WIDE "< >" pair (e.g. the
//     42×16 "Frame 41" on the ENVELOPE/FILTER/FX-RACK headers) rather than a
//     square down-chevron. Those stay static (faithful to the design) until a
//     real cycler interaction exists.
// An unconfigured design-system "Dropdown" template: a frame named "Dropdown"
// whose value is the literal word "Dropdown" (ELYSIUM's "VST Style" placeholder).
// The design never shows it; render NOTHING so it can't surface as a stray
// element between panels.
bool is_unconfigured_dropdown_template(const IRNode& node) {
    if (lower_copy(node.type) != "frame" || lower_copy(node.name) != "dropdown")
        return false;
    const auto value = first_text_descendant(node);
    return value && lower_copy(*value) == "dropdown";
}

bool looks_like_real_dropdown(const IRNode& node) {
    const auto value = first_text_descendant(node);
    if (!value || lower_copy(*value) == "dropdown") return false;
    for (const auto& c : node.children) {
        if (lower_copy(c.type) != "image") continue;
        const float w = c.style.width.value_or(0.0f);
        const float h = c.style.height.value_or(0.0f);
        if (w > 0.0f && h > 0.0f && w / h <= 1.8f) return true;  // square chevron
    }
    return false;
}

// Design-import widget recognition by Figma layer name. Designers label these
// containers explicitly (ELYSIUM's FX RACK "Dropdown" frames, the "Search"
// field), so the name is a reliable, source-honest signal — no content
// guessing. Returns nullopt when the node is not one of these.
std::optional<NativeWidgetKind> kind_from_name(const IRNode& node) {
    const auto name = lower_copy(node.name);
    const auto type = lower_copy(node.type);
    if (type == "frame" && name == "dropdown" && looks_like_real_dropdown(node))
        return NativeWidgetKind::combo_box;
    // A search field → an editable TextEditor sized to the whole box. Promote
    // the CONTAINER (not the inner text cell) so the field spans the box and the
    // placeholder isn't truncated.
    if (is_search_container(node))
        return NativeWidgetKind::text_editor;
    return std::nullopt;
}

ResolvedNativeNode resolve_node(const IRNode& node,
                                std::string_view path,
                                const AssetIndex& assets) {
    ResolvedNativeNode out;
    if (auto audio_kind = kind_from_audio(node.audio_widget)) {
        out.kind = *audio_kind;
    } else if (auto name_kind = kind_from_name(node)) {
        out.kind = *name_kind;
    } else if (auto type_kind = kind_from_type(node, path, out.diagnostics)) {
        out.kind = *type_kind;
    } else {
        out.kind = NativeWidgetKind::view;
        out.diagnostics.push_back(diagnostic(
            ImportDiagnosticSeverity::warning,
            ImportDiagnosticKind::unknown,
            "native-unsupported-node",
            std::string(path),
            "node type '" + node.type + "' falls back to View",
            node));
    }

    out.id = node_id(node, path);
    out.text = text_for_node(node, out.kind);
    append_unsupported_property_diagnostics(node, path, out.diagnostics);
    append_asset_diagnostics(node, path, assets, out.diagnostics);

    out.children.reserve(node.children.size());
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        std::ostringstream child_path;
        child_path << path << "/children[" << i << "]";
        out.children.push_back(resolve_node(node.children[i], child_path.str(), assets));
    }
    return out;
}

std::optional<float> parse_float(std::string_view value) {
    const std::string text(value);
    char* end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    if (end == text.c_str()) return std::nullopt;
    while (*end != '\0') {
        if (!std::isspace(static_cast<unsigned char>(*end))) return std::nullopt;
        ++end;
    }
    return parsed;
}

std::optional<float> attr_float(const IRNode& node, std::string_view key) {
    auto value = attr(node, key);
    if (!value) return std::nullopt;
    return parse_float(*value);
}

void apply_imported_image_sizing(View& view, const IRNode& node) {
    const auto sizing = imported_image_sizing_override(node);
    if (!sizing) return;

    auto& flex = view.flex();
    flex.preferred_width = sizing->width;
    flex.preferred_height = sizing->height;
    flex.dim_width = {sizing->width, DimensionUnit::px};
    flex.dim_height = {sizing->height, DimensionUnit::px};
    if (sizing->left) view.set_left(*sizing->left);
    if (sizing->top) view.set_top(*sizing->top);
}

// The native lane wants a canvas Color rather than the raw 0..255 quad.
std::optional<Color> parse_hex_color(std::string_view value) {
    auto rgba = parse_hex_color_rgba(value);
    if (!rgba) return std::nullopt;
    return Color::rgba8(static_cast<uint8_t>((*rgba)[0]),
                        static_cast<uint8_t>((*rgba)[1]),
                        static_cast<uint8_t>((*rgba)[2]),
                        static_cast<uint8_t>((*rgba)[3]));
}

FlexJustify to_flex_justify(LayoutAlign align) {
    switch (align) {
        case LayoutAlign::flex_end: return FlexJustify::end_;
        case LayoutAlign::center: return FlexJustify::center;
        case LayoutAlign::space_between: return FlexJustify::space_between;
        case LayoutAlign::space_around: return FlexJustify::space_around;
        case LayoutAlign::stretch:
        case LayoutAlign::flex_start:
            return FlexJustify::start;
    }
    return FlexJustify::start;
}

FlexAlign to_flex_align(LayoutAlign align) {
    switch (align) {
        case LayoutAlign::flex_end: return FlexAlign::end;
        case LayoutAlign::center: return FlexAlign::center;
        case LayoutAlign::stretch: return FlexAlign::stretch;
        case LayoutAlign::flex_start:
        case LayoutAlign::space_between:
        case LayoutAlign::space_around:
            return FlexAlign::start;
    }
    return FlexAlign::start;
}

std::optional<FlexAlign> parse_flex_align(std::string_view value) {
    const auto lower = lower_copy(std::string(value));
    if (lower == "auto") return FlexAlign::auto_;
    if (lower == "flex-start" || lower == "start") return FlexAlign::start;
    if (lower == "flex-end" || lower == "end") return FlexAlign::end;
    if (lower == "center") return FlexAlign::center;
    if (lower == "stretch") return FlexAlign::stretch;
    if (lower == "baseline") return FlexAlign::baseline;
    return std::nullopt;
}

std::optional<View::Overflow> parse_overflow(std::string_view value) {
    const auto lower = lower_copy(std::string(value));
    if (lower == "hidden" || lower == "clip") return View::Overflow::hidden;
    if (lower == "visible") return View::Overflow::visible;
    if (lower == "scroll" || lower == "auto") return View::Overflow::scroll;
    return std::nullopt;
}

std::optional<View::Position> parse_position(std::string_view value) {
    const auto lower = lower_copy(std::string(value));
    if (lower == "static") return View::Position::static_;
    if (lower == "relative") return View::Position::relative;
    if (lower == "absolute") return View::Position::absolute;
    if (lower == "fixed") return View::Position::fixed;
    if (lower == "sticky") return View::Position::sticky;
    return std::nullopt;
}

LabelAlign parse_label_align(std::string_view value) {
    const auto lower = lower_copy(std::string(value));
    if (lower == "center") return LabelAlign::center;
    if (lower == "right" || lower == "end") return LabelAlign::right;
    if (lower == "auto") return LabelAlign::auto_;
    if (lower == "justify") return LabelAlign::justify;
    if (lower == "match-parent") return LabelAlign::match_parent;
    return LabelAlign::left;
}

float normalized_audio_default(const IRNode& node) {
    if (node.audio_max > node.audio_min)
        return std::clamp((node.audio_default - node.audio_min) /
                              (node.audio_max - node.audio_min),
                          0.0f,
                          1.0f);
    return std::clamp(node.audio_default, 0.0f, 1.0f);
}

float normalized_audio_value(const IRNode& node) {
    if (auto value = attr_float(node, "value")) return std::clamp(*value, 0.0f, 1.0f);
    return normalized_audio_default(node);
}

void append_resolved_diagnostics(const ResolvedNativeNode& node,
                                 std::vector<ImportDiagnostic>& diagnostics) {
    diagnostics.insert(diagnostics.end(), node.diagnostics.begin(), node.diagnostics.end());
    for (const auto& child : node.children)
        append_resolved_diagnostics(child, diagnostics);
}

// ── Faithful-vector import: resolve an SVG asset to its document text ─────────
// Percent-decode a `data:` payload (`%3C` → `<`, `+` stays literal in data URIs).
std::string svg_percent_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

// resolve_svg_document is defined below in the named pulp::view namespace (it is
// exported via design_import_native_common.hpp so the C++ codegen reuses it).
// make_faithful_svg_frame (this anonymous namespace) calls it through that
// header declaration.

// Translate the IR's typed interactive overlays into DesignFrameView's element
// list. Only `knob` exists today; new kinds map here as they land.
std::vector<DesignFrameElement> to_frame_elements(
    const std::vector<IRInteractiveElement>& elements) {
    std::vector<DesignFrameElement> out;
    out.reserve(elements.size());
    for (const auto& e : elements) {
        DesignFrameElement el;
        switch (e.kind) {
            case InteractiveElementKind::fader:
                el.kind = DesignFrameElement::Kind::fader; break;
            case InteractiveElementKind::toggle:
                el.kind = DesignFrameElement::Kind::toggle; break;
            case InteractiveElementKind::dropdown:
                el.kind = DesignFrameElement::Kind::dropdown; break;
            case InteractiveElementKind::text_field:
                el.kind = DesignFrameElement::Kind::text_field; break;
            case InteractiveElementKind::tab_group:
                el.kind = DesignFrameElement::Kind::tab_group; break;
            case InteractiveElementKind::stepper:
                el.kind = DesignFrameElement::Kind::stepper; break;
            case InteractiveElementKind::swap:
                el.kind = DesignFrameElement::Kind::swap; break;
            case InteractiveElementKind::action:
                el.kind = DesignFrameElement::Kind::action; break;
            case InteractiveElementKind::xy_pad:
                el.kind = DesignFrameElement::Kind::xy_pad; break;
            case InteractiveElementKind::value_label:
                el.kind = DesignFrameElement::Kind::value_label; break;
            case InteractiveElementKind::custom:
                el.kind = DesignFrameElement::Kind::custom; break;
            case InteractiveElementKind::knob:
                el.kind = DesignFrameElement::Kind::knob; break;
        }
        // knob (SVG-patch) data
        el.cx = e.cx;
        el.cy = e.cy;
        el.hit_radius = e.hit_radius;
        el.needle_d = e.svg_patch_d;
        el.value = e.default_value;
        el.flash = e.flash;  // toggle: press-flash command vs sticky flip
        // overlay-control data (dropdown / text_field / tab_group)
        el.x = e.x; el.y = e.y; el.w = e.w; el.h = e.h;
        el.placeholder = e.placeholder;
        el.options = e.options;
        el.selected_index = e.selected_index;
        el.bg_color = e.bg_color;
        // swap / action / xy_pad / value_label data
        el.target_frame = e.target_frame;
        el.action = e.action;
        el.text = e.text;
        el.value_left_align = e.value_left_align;
        el.value_y = e.default_value_y;
        // custom native control — the factory id + opaque props.
        el.factory_id = e.factory_id;
        el.custom_props = e.custom_props;
        // Carry the design-source node id to the live element so the inspector's
        // Wiring lens can map a control back to its Figma node.
        if (e.source_node_id) el.source_node_id = *e.source_node_id;
        // Host-param binding key: lets DesignFrameView resolve this control
        // against the framework-agnostic HostParamSurface (element_for_param_key
        // / sync_from_host_params) when host-param routing is enabled.
        el.param_key = e.param_key;
        out.push_back(std::move(el));
    }
    return out;
}

// Build a DesignFrameView for a faithful_svg node, or nullptr (with a
// diagnostic) when its SVG asset can't be resolved — letting the caller fall
// back to normal materialization.
std::unique_ptr<View> make_faithful_svg_frame(const IRNode& node,
                                              const IRAssetManifest& manifest,
                                              std::string_view path,
                                              std::vector<ImportDiagnostic>& diagnostics) {
    const std::string asset_id = node.svg_asset_id.value_or("");
    const IRAssetRef* asset = asset_id.empty() ? nullptr : manifest.resolve(asset_id);
    std::string svg = asset ? resolve_svg_document(*asset) : std::string{};
    if (svg.empty()) {
        diagnostics.push_back(diagnostic(
            ImportDiagnosticSeverity::warning,
            ImportDiagnosticKind::unresolved_asset,
            "native-materialize-faithful-svg-unresolved",
            std::string(path),
            asset_id.empty()
                ? "faithful_svg node has no svg_asset_id"
                : "faithful_svg asset '" + asset_id + "' could not be resolved to an SVG document",
            node,
            asset_id.empty() ? std::optional<std::string>("svg_asset_id") : std::nullopt));
        return nullptr;
    }
    // A custom control whose factory isn't registered renders inert (the baked
    // SVG underneath still shows). Diagnose it so the gap is SEEN and never
    // becomes a silent knob.
    auto diagnose_custom_controls = [&](const IRNode& n) {
        for (const auto& ie : n.interactive_elements) {
            if (ie.kind != InteractiveElementKind::custom) continue;
            if (ie.factory_id.empty() || !has_design_control_factory(ie.factory_id)) {
                diagnostics.push_back(diagnostic(
                    ImportDiagnosticSeverity::warning,
                    ImportDiagnosticKind::unsupported_property,
                    "native-materialize-custom-factory-unregistered",
                    std::string(path),
                    ie.factory_id.empty()
                        ? "custom interactive element has no factory_id (renders inert)"
                        : "custom interactive element factory '" + ie.factory_id +
                              "' is not registered (renders inert)",
                    n,
                    std::nullopt));
            }
        }
    };
    diagnose_custom_controls(node);

    auto frame = std::make_unique<DesignFrameView>(std::move(svg),
                                                   to_frame_elements(node.interactive_elements));

    // Frames 1..N: the node's alternate states, in capture order. A `swap`
    // element addresses frames POSITIONALLY, so every alternate must produce
    // exactly one add_frame call in order — an alternate whose SVG failed to
    // resolve is added blank (with its overlays) and diagnosed, never skipped,
    // because skipping would silently re-point every later swap target.
    for (std::size_t i = 0; i < node.alternate_frames.size(); ++i) {
        const IRNode& alt = node.alternate_frames[i];
        const std::string alt_id = alt.svg_asset_id.value_or("");
        const IRAssetRef* alt_asset = alt_id.empty() ? nullptr : manifest.resolve(alt_id);
        std::string alt_svg = alt_asset ? resolve_svg_document(*alt_asset) : std::string{};
        if (alt_svg.empty()) {
            diagnostics.push_back(diagnostic(
                ImportDiagnosticSeverity::warning,
                ImportDiagnosticKind::unresolved_asset,
                "native-materialize-faithful-svg-frame-unresolved",
                std::string(path),
                "frame " + std::to_string(i + 1) + ": " +
                    (alt_id.empty()
                         ? "alternate frame has no svg_asset_id"
                         : "alternate frame asset '" + alt_id +
                               "' could not be resolved to an SVG document") +
                    " (added blank to keep swap target indices stable)",
                alt,
                alt_id.empty() ? std::optional<std::string>("svg_asset_id") : std::nullopt));
        }
        diagnose_custom_controls(alt);
        frame->add_frame(std::move(alt_svg), to_frame_elements(alt.interactive_elements));
    }

    // If any control carries a host-param binding key, self-wire gestures to the
    // framework-agnostic HostParamSurface so an imported design with bound
    // controls drives host parameters directly. Controls with an empty key still
    // fall back to on_value_changed, so an all-unbound frame stays inert here.
    auto bound = [](const IRNode& n) {
        return std::any_of(n.interactive_elements.begin(), n.interactive_elements.end(),
                           [](const IRInteractiveElement& e) { return !e.param_key.empty(); });
    };
    const bool any_bound =
        bound(node) || std::any_of(node.alternate_frames.begin(),
                                   node.alternate_frames.end(), bound);
    if (any_bound) frame->route_changes_to_host_params(true);
    return frame;
}

bool has_missing_asset_diagnostic(const std::vector<ImportDiagnostic>& diagnostics,
                                  std::string_view path,
                                  std::string_view asset_id) {
    if (asset_id.empty()) return false;
    for (const auto& existing : diagnostics) {
        if (existing.path == path &&
            existing.kind == ImportDiagnosticKind::unresolved_asset &&
            existing.code == "native-missing-asset" &&
            existing.message.find(std::string(asset_id)) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::unique_ptr<View> make_asset_placeholder(const IRNode& node,
                                             std::string_view path,
                                             std::string_view asset_id,
                                             std::vector<ImportDiagnostic>& diagnostics) {
    if (!has_missing_asset_diagnostic(diagnostics, path, asset_id)) {
        diagnostics.push_back(diagnostic(
            ImportDiagnosticSeverity::warning,
            ImportDiagnosticKind::unresolved_asset,
            "native-materialize-unresolved-asset",
            std::string(path),
            asset_id.empty()
                ? "image node has no resolved asset id"
                : "asset id '" + std::string(asset_id) + "' could not be resolved for native materialization",
            node,
            asset_id.empty() ? std::optional<std::string>("srcAssetId") : std::nullopt));
    }
    return std::make_unique<View>();
}

void apply_identity(View& view, const IRNode& node, const ResolvedNativeNode& resolved) {
    view.set_id(resolved.id);
    if (node.stable_anchor_id && !node.stable_anchor_id->empty())
        view.set_anchor_id(*node.stable_anchor_id);
    if (auto hit_testable = attr(node, "pulpHitTestable");
        hit_testable && !attr_bool(node, "pulpHitTestable")) {
        view.set_hit_testable(false);
    }
    if (auto label = resolved.text; label && !label->empty())
        view.set_access_label(*label);
}

void apply_layout(View& view, const IRNode& node, std::optional<LayoutDirection> parent_direction) {
    // Grid signal mirrors the JS codegen: display:grid OR an explicit track
    // template. Templates come from the v0/TSX contract attributes when
    // present, else from the IR layout fields the Figma producers emit
    // (gridTemplateColumns/Rows) — without the fallback a Figma GRID frame
    // switched to grid mode with an empty column list and dropped every child.
    const bool is_grid =
        (node.layout.display && *node.layout.display == "grid") ||
        node.layout.grid_template_columns || node.layout.grid_template_rows;
    if (is_grid) {
        view.set_layout_mode(LayoutMode::grid);
        auto& grid = view.grid();
        if (auto it = node.attributes.find("pulpGridTemplateColumns"); it != node.attributes.end())
            grid.template_columns = GridStyle::parse_template(it->second);
        else if (node.layout.grid_template_columns)
            grid.template_columns = GridStyle::parse_template(*node.layout.grid_template_columns);
        if (auto it = node.attributes.find("pulpGridTemplateRows"); it != node.attributes.end())
            grid.template_rows = GridStyle::parse_template(it->second);
        else if (node.layout.grid_template_rows)
            grid.template_rows = GridStyle::parse_template(*node.layout.grid_template_rows);
        // A grid with no explicit columns gets a single implicit column
        // rather than vanishing — same fallback the JS codegen emits.
        if (grid.template_columns.empty())
            grid.template_columns = GridStyle::parse_template("1fr");
        grid.column_gap = node.layout.column_gap.value_or(node.layout.gap);
        grid.row_gap = node.layout.row_gap.value_or(node.layout.gap);
    }

    // Per-child grid placement: "N", "N / M", or "N / span S" line strings
    // (CSS 1-based). Named lines / span-only forms stay on auto-placement,
    // matching the JS codegen's emit_js_grid_placement.
    auto apply_grid_track = [&view](const std::optional<std::string>& value,
                                    int GridStyle::*start_field, int GridStyle::*end_field) {
        if (!value || value->empty()) return;
        auto trim = [](std::string s) {
            const auto a = s.find_first_not_of(" \t");
            const auto b = s.find_last_not_of(" \t");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        auto as_int = [](const std::string& t, int& out) {
            if (t.empty()) return false;
            char* end = nullptr;
            const long n = std::strtol(t.c_str(), &end, 10);
            if (end == t.c_str()) return false;
            out = static_cast<int>(n);
            return true;
        };
        const auto slash = value->find('/');
        const std::string lo = trim(slash == std::string::npos ? *value : value->substr(0, slash));
        const std::string hi = slash == std::string::npos ? std::string() : trim(value->substr(slash + 1));
        int lo_i = 0;
        if (!as_int(lo, lo_i)) return;
        view.grid().*start_field = lo_i;
        int hi_i = 0;
        if (as_int(hi, hi_i)) {
            view.grid().*end_field = hi_i;
        } else if (hi.rfind("span", 0) == 0) {
            int span = 0;
            if (as_int(trim(hi.substr(4)), span) && span > 0)
                view.grid().*end_field = lo_i + span;
        }
    };
    apply_grid_track(node.layout.grid_column, &GridStyle::grid_column_start, &GridStyle::grid_column_end);
    apply_grid_track(node.layout.grid_row, &GridStyle::grid_row_start, &GridStyle::grid_row_end);

    auto& flex = view.flex();
    flex.direction = node.layout.direction == LayoutDirection::row
        ? FlexDirection::row
        : FlexDirection::column;
    flex.justify_content = to_flex_justify(node.layout.justify);
    flex.align_items = to_flex_align(node.layout.align);
    flex.gap = node.layout.gap;
    if (node.layout.row_gap) flex.row_gap = *node.layout.row_gap;
    if (node.layout.column_gap) flex.column_gap = *node.layout.column_gap;
    flex.padding_top = node.layout.padding_top;
    flex.padding_right = node.layout.padding_right;
    flex.padding_bottom = node.layout.padding_bottom;
    flex.padding_left = node.layout.padding_left;
    if (node.layout.margin_top) flex.margin_top = *node.layout.margin_top;
    if (node.layout.margin_right) flex.margin_right = *node.layout.margin_right;
    if (node.layout.margin_bottom) flex.margin_bottom = *node.layout.margin_bottom;
    if (node.layout.margin_left) flex.margin_left = *node.layout.margin_left;
    if (node.layout.flex_grow) flex.flex_grow = *node.layout.flex_grow;
    if (node.layout.flex_shrink) flex.flex_shrink = *node.layout.flex_shrink;
    if (node.layout.flex_basis) {
        const auto dim = Dimension::parse(*node.layout.flex_basis);
        flex.dim_flex_basis = dim;
        if (dim.unit == DimensionUnit::px) flex.flex_basis = dim.value;
    }
    if (node.layout.order) flex.order = *node.layout.order;
    if (node.layout.aspect_ratio) flex.aspect_ratio = *node.layout.aspect_ratio;
    if (node.layout.wrap) flex.flex_wrap = FlexWrap::wrap;
    if (node.layout.align_self) {
        if (auto align = parse_flex_align(*node.layout.align_self)) flex.align_self = *align;
    }
    if (node.layout.align_content) {
        const auto align = lower_copy(*node.layout.align_content);
        if (align == "space-between") {
            flex.align_content_space = FlexStyle::AlignContentSpace::space_between;
        } else if (align == "space-around") {
            flex.align_content_space = FlexStyle::AlignContentSpace::space_around;
        } else if (align == "space-evenly") {
            flex.align_content_space = FlexStyle::AlignContentSpace::space_evenly;
        } else if (auto parsed = parse_flex_align(align)) {
            flex.align_content = *parsed;
        }
    }

    if (node.style.width) {
        flex.preferred_width = *node.style.width;
        flex.dim_width = {*node.style.width, DimensionUnit::px};
    }
    if (node.style.height) {
        flex.preferred_height = *node.style.height;
        flex.dim_height = {*node.style.height, DimensionUnit::px};
    }
    if (node.style.min_width) {
        flex.min_width = *node.style.min_width;
        flex.dim_min_width = {*node.style.min_width, DimensionUnit::px};
    }
    if (node.style.min_height) {
        flex.min_height = *node.style.min_height;
        flex.dim_min_height = {*node.style.min_height, DimensionUnit::px};
    }
    if (node.style.max_width) {
        flex.max_width = *node.style.max_width;
        flex.dim_max_width = {*node.style.max_width, DimensionUnit::px};
    }
    if (node.style.max_height) {
        flex.max_height = *node.style.max_height;
        flex.dim_max_height = {*node.style.max_height, DimensionUnit::px};
    }

    const bool parent_is_row = parent_direction && *parent_direction == LayoutDirection::row;
    const bool parent_is_column = parent_direction && *parent_direction == LayoutDirection::column;
    const bool has_explicit_align_self = node.layout.align_self.has_value();
    if (node.layout.width_mode == SizingMode::fill && !node.style.width) {
        if (!parent_direction || parent_is_row) {
            flex.flex_grow = std::max(flex.flex_grow, 1.0f);
        } else if (parent_is_column && !has_explicit_align_self) {
            flex.align_self = FlexAlign::stretch;
        }
    }
    if (node.layout.height_mode == SizingMode::fill && !node.style.height) {
        if (!parent_direction || parent_is_column) {
            flex.flex_grow = std::max(flex.flex_grow, 1.0f);
        } else if (parent_is_row && !has_explicit_align_self) {
            flex.align_self = FlexAlign::stretch;
        }
    }
    if (node.layout.width_mode == SizingMode::hug && !node.style.width)
        flex.dim_width = {0.0f, DimensionUnit::auto_};
    if (node.layout.height_mode == SizingMode::hug && !node.style.height)
        flex.dim_height = {0.0f, DimensionUnit::auto_};
}

// Every CSS color a design can write, from ONE place.
//
// This exists because the fallback used to live at a single call site. Prefer
// the hex fast path, then the shared CSS parser for rgb()/rgba()/transparent:
// Figma demotes a hairline stroke (the FILTER & EQ grid `Line`s) to a 1px frame
// whose fill is the stroke color — often rgba(171,171,171,0.1) — which
// parse_hex_color drops, leaving the grid invisible.
//
// That fallback was attached to `background_color` alone, while SEVEN paint
// sites here call parse_hex_color: background, text `color`, `border_color`, and
// the four per-side border colors. So a design saying `color: rgba(...)` got
// its background right and silently lost its TEXT and every BORDER. One branch
// learned the lesson; its six siblings never heard it — the same shape as
// setBoxShadow being emitted from the frame branch alone, so that 16 declared
// shadows became 1.
//
// A helper rather than six more fallbacks, because the bug is that the
// knowledge lived at a call site: the eighth paint site added later would have
// repeated it.
static std::optional<Color> parse_any_css_color(const std::string& value) {
    if (auto color = parse_hex_color(value)) return color;
    if (value.rfind("rgb", 0) == 0 || value.rfind("hsl", 0) == 0 || value == "transparent")
        return parse_css_color(value);
    return std::nullopt;
}

void apply_visual_style(View& view, const IRStyle& style,
                        bool skip_border = false) {
    if (style.background_color) {
        if (auto color = parse_any_css_color(*style.background_color))
            view.set_background_color(*color);
    }
    // A CSS background-gradient (the light "hero" panels and the cube/prism/
    // cylinder illustration fills in real Figma imports) paints over the solid
    // background_color. Dropping it was the dominant ELYSIUM dark/light parity
    // gap — see core/view/src/css_gradient.cpp for the shared parser.
    if (style.background_gradient && !style.background_gradient->empty())
        apply_css_background_gradient(view, *style.background_gradient);
    if (style.background_repeat)
        view.set_background_repeat(*style.background_repeat);
    if (style.background_size)
        view.set_background_size(*style.background_size);
    // object-fit is honored by ImageView::paint; on plain Views it sits in the
    // same style_extras slot the bridge writes, so applying it unconditionally
    // keeps the native tree in lockstep with the JS lane's setObjectFit.
    if (style.object_fit)
        view.set_object_fit(*style.object_fit);
    if (style.color) {
        if (auto color = parse_any_css_color(*style.color))
            view.set_inheritable_text_color(*color);
    }
    if (style.opacity)
        view.set_opacity(*style.opacity);
    if (style.border_radius)
        view.set_border_radius(*style.border_radius);
    // A rasterized-vector image (a Figma vector/line exported as a PNG) carries
    // the source stroke as border_color/border_width, but the stroke is already
    // baked into the raster. Drawing it again paints a spurious box outline —
    // the visible bug was a bright purple rectangle around the FILTER & EQ curve
    // (Vector 3, stroke #7e6aff). Skip the border for those nodes.
    if (!skip_border) {
        if (style.border_width)
            view.set_border_width(*style.border_width);
        if (style.border_color) {
            if (auto color = parse_any_css_color(*style.border_color))
                view.set_border_color(*color);
        }
        if (style.border_top_width) view.set_border_top_width(*style.border_top_width);
        if (style.border_right_width) view.set_border_right_width(*style.border_right_width);
        if (style.border_bottom_width) view.set_border_bottom_width(*style.border_bottom_width);
        if (style.border_left_width) view.set_border_left_width(*style.border_left_width);
        if (style.border_top_color) {
            if (auto color = parse_any_css_color(*style.border_top_color))
                view.set_border_top_color(*color);
        }
        if (style.border_right_color) {
            if (auto color = parse_any_css_color(*style.border_right_color))
                view.set_border_right_color(*color);
        }
        if (style.border_bottom_color) {
            if (auto color = parse_any_css_color(*style.border_bottom_color))
                view.set_border_bottom_color(*color);
        }
        if (style.border_left_color) {
            if (auto color = parse_any_css_color(*style.border_left_color))
                view.set_border_left_color(*color);
        }
        // CSS border-style keyword → View::BorderStyle, same table the bridge's
        // setBorderStyle uses. A Figma dashPattern lowers to "dashed" at the
        // producers; without this the native materializer painted it solid
        // while the JS-bridge lane dashed it. Unknown keywords keep the solid
        // default (View degrades double/groove/... to solid anyway).
        if (style.border_style) {
            const std::string& bs = *style.border_style;
            if (bs == "dashed")      view.set_border_style(View::BorderStyle::dashed);
            else if (bs == "dotted") view.set_border_style(View::BorderStyle::dotted);
            else if (bs == "none")   view.set_border_style(View::BorderStyle::none);
            else if (bs == "hidden") view.set_border_style(View::BorderStyle::hidden);
        }
    }
    if (style.border_top_left_radius) view.set_corner_radius_tl(*style.border_top_left_radius);
    if (style.border_top_right_radius) view.set_corner_radius_tr(*style.border_top_right_radius);
    if (style.border_bottom_right_radius) view.set_corner_radius_br(*style.border_bottom_right_radius);
    if (style.border_bottom_left_radius) view.set_corner_radius_bl(*style.border_bottom_left_radius);
    if (style.font_family) view.set_inheritable_font_family(*style.font_family);
    if (style.font_size) view.set_inheritable_font_size(*style.font_size);
    if (style.font_weight) view.set_inheritable_font_weight(*style.font_weight);
    if (style.letter_spacing) view.set_inheritable_letter_spacing(*style.letter_spacing);
    if (style.text_align) view.set_inheritable_text_align(static_cast<int>(parse_label_align(*style.text_align)));
    if (style.overflow) {
        if (auto overflow = parse_overflow(*style.overflow)) {
            view.set_overflow(*overflow);
            // Imported clipping containers opt into circle-marker clip
            // tolerance so an XY-pad-style value dot positioned at an edge value
            // is admitted rather than chopped by the strict clip. Native trees
            // leave this off (default) and clip strictly.
            if (*overflow == View::Overflow::hidden ||
                *overflow == View::Overflow::scroll)
                view.set_clip_marker_tolerance(true);
        }
    }
    if (style.position) {
        if (auto position = parse_position(*style.position)) view.set_position(*position);
    }
    if (style.top) view.set_top(*style.top);
    if (style.right) view.set_right(*style.right);
    if (style.bottom) view.set_bottom(*style.bottom);
    if (style.left) view.set_left(*style.left);
    if (style.z_index) view.set_z_index(*style.z_index);
}

void apply_label_style(Label& label, const IRStyle& style) {
    if (style.font_family) label.set_font_family(*style.font_family);
    if (style.font_size) label.set_font_size(*style.font_size);
    if (style.font_weight) label.set_font_weight(*style.font_weight);
    if (style.font_style && lower_copy(*style.font_style) == "italic") label.set_font_style(1);
    if (style.letter_spacing) label.set_letter_spacing(*style.letter_spacing);
    if (style.line_height) label.set_line_height(*style.line_height);
    if (style.text_align) label.set_text_align(parse_label_align(*style.text_align));
    if (style.color) {
        if (auto color = parse_any_css_color(*style.color)) label.set_text_color(*color);
    }
    if (style.text_transform) {
        const auto value = lower_copy(*style.text_transform);
        if (value == "uppercase") label.set_text_transform(Label::TextTransform::uppercase);
        else if (value == "lowercase") label.set_text_transform(Label::TextTransform::lowercase);
        else if (value == "capitalize") label.set_text_transform(Label::TextTransform::capitalize);
    }
    if (style.text_decoration) {
        const auto value = lower_copy(*style.text_decoration);
        if (value.find("underline") != std::string::npos)
            label.set_text_decoration(Label::TextDecoration::underline);
        else if (value.find("line-through") != std::string::npos)
            label.set_text_decoration(Label::TextDecoration::line_through);
        else if (value.find("overline") != std::string::npos)
            label.set_text_decoration(Label::TextDecoration::overline);
    }
    if (style.white_space && lower_copy(*style.white_space) != "nowrap")
        label.set_multi_line(true);
    // Vertically center a single-line label whose design slot is taller than its
    // font (e.g. an 8px "SEARCH" in a 17px box, tab digits in a 20px button).
    // Figma centers text in a fixed-height frame but the IR drops
    // textAlignVertical, so derive it from slot-vs-font. The web-compat codegen
    // emits the SAME rule as `verticalAlign:middle` (design_codegen.cpp), so
    // both render paths converge on Label::set_vertical_align(center) and the
    // screenshot-parity invariant holds. The Label default is top.
    if (style.height && style.font_size &&
        *style.height > *style.font_size * 1.15f)
        label.set_vertical_align(canvas::TextVerticalAlign::center);
}

void apply_svg_paint(SvgPathWidget& path, const IRNode& node) {
    if (auto fill = attr(node, "fill")) {
        if (*fill == "none") path.clear_fill();
        else if (auto color = parse_any_css_color(*fill)) path.set_fill_color(*color);
    }
    if (auto stroke = attr(node, "stroke")) {
        if (*stroke == "none") path.clear_stroke();
        else if (auto color = parse_any_css_color(*stroke)) path.set_stroke_color(*color);
    }
    if (auto stroke_width = attr_float(node, "stroke-width"))
        path.set_stroke_width(*stroke_width);
}

void apply_svg_paint(SvgRectWidget& rect, const IRNode& node) {
    if (auto fill = attr(node, "fill")) {
        if (*fill == "none") rect.clear_fill();
        else if (auto color = parse_any_css_color(*fill)) rect.set_fill_color(*color);
    }
    if (auto stroke = attr(node, "stroke")) {
        if (*stroke == "none") rect.clear_stroke();
        else if (auto color = parse_any_css_color(*stroke)) rect.set_stroke_color(*color);
    }
    if (auto stroke_width = attr_float(node, "stroke-width"))
        rect.set_stroke_width(*stroke_width);
}

void apply_svg_paint(SvgLineWidget& line, const IRNode& node) {
    if (auto stroke = attr(node, "stroke")) {
        if (*stroke == "none") line.clear_stroke();
        else if (auto color = parse_any_css_color(*stroke)) line.set_stroke_color(*color);
    }
    if (auto stroke_width = attr_float(node, "stroke-width"))
        line.set_stroke_width(*stroke_width);
}

// Skin a knob with its captured body disc when hoist_captured_art_knobs +
// enrich have stamped an absolute asset_path (+ png_natural + opaque-core rect).
// A single-frame sprite renders the design's disc and core-fits it to the box;
// the engine overlays the native rotating notch, so the knob stays interactive.
// No-op when the knob carries no captured-art metadata (a default synth knob).
void apply_captured_art_knob_skin(Knob& knob, const IRNode& node) {
    auto skin = attr(node, "asset_path");
    if (!skin || skin->empty()) return;
    const float pw = attr_float(node, "png_natural_w").value_or(0.0f);
    const float ph = attr_float(node, "png_natural_h").value_or(0.0f);
    if (pw <= 0.0f || ph <= 0.0f) return;
    auto strip = std::make_shared<SpriteStrip>();
    strip->load_from_file(*skin, static_cast<int>(pw), static_cast<int>(ph), 1,
                          SpriteStrip::Orientation::vertical);
    knob.set_sprite_strip(std::move(strip));
    const float cw = attr_float(node, "art_core_w").value_or(0.0f);
    const float ch = attr_float(node, "art_core_h").value_or(0.0f);
    if (cw > 0.0f && ch > 0.0f)
        knob.set_sprite_core(attr_float(node, "art_core_x").value_or(0.0f),
                             attr_float(node, "art_core_y").value_or(0.0f), cw, ch);
    // Design's own pointer geometry (Figma "Vector 7"), captured by
    // hoist_captured_art_knobs as fractions of the disc half-extent. When
    // present, Knob::paint draws THIS pointer over the static disc — pivoting at
    // the disc core center on the value arc — instead of the synthetic notch, so
    // it rides the disc's baked min/center/max reference ticks.
    if (auto r_out = attr_float(node, "knob_ind_r_out")) {
        const float r_in = attr_float(node, "knob_ind_r_in").value_or(0.0f);
        const float w = attr_float(node, "knob_ind_w").value_or(0.0f);
        Color color = Color::rgba(0.92f, 0.92f, 0.92f, 1.0f);
        if (auto hex = attr(node, "knob_ind_color"))
            if (auto parsed = parse_any_css_color(*hex)) color = *parsed;
        knob.set_captured_indicator(r_in, *r_out, w, color);
    }
}

std::unique_ptr<View> make_widget(const IRNode& node,
                                  const ResolvedNativeNode& resolved,
                                  const IRAssetManifest& manifest,
                                  const NativeMaterializeOptions& options,
                                  std::string_view path,
                                  std::vector<ImportDiagnostic>& diagnostics) {
    const auto semantics = imported_widget_semantics(node, resolved);
    const auto& text = semantics.text;
    switch (resolved.kind) {
        case NativeWidgetKind::label: {
            auto label = std::make_unique<Label>(text);
            apply_label_style(*label, node.style);
            return label;
        }
        case NativeWidgetKind::text_button:
            return std::make_unique<TextButton>(text);
        case NativeWidgetKind::text_editor: {
            auto editor = std::make_unique<TextEditor>();
            if (semantics.text_placeholder) {
                editor->placeholder = *semantics.text_placeholder;
            } else if (!node.text_content.empty()) {
                editor->placeholder = node.text_content;
            } else if (const auto* t = first_text_descendant_node(node)) {
                // A promoted search CONTAINER: the inner "SEARCH" text is the
                // placeholder (replaced by a caret on tap); inherit its font size.
                editor->placeholder = t->text_content;
                if (t->style.font_size) editor->set_font_size(*t->style.font_size);
                // Inset the text past the leading magnifier icon (the kept image
                // child) so the placeholder/caret don't overlap it.
                for (const auto& c : node.children) {
                    if (lower_copy(c.type) != "image") continue;
                    const float right = c.style.left.value_or(0.0f) +
                                        c.style.width.value_or(0.0f);
                    editor->set_content_inset_left(right + 6.0f);
                    break;
                }
            }
            if (semantics.text_value)
                editor->set_text(*semantics.text_value);
            return editor;
        }
        case NativeWidgetKind::combo_box: {
            // A captured "Dropdown" frame → an interactive ComboBox. Its text
            // child is the selected value, and that is the ONLY real option: a
            // static design defines no alternatives, so emit just the shown value
            // rather than fabricating "Option 2/3" placeholders. A design that
            // carries component variants would source the full list from them.
            auto combo = std::make_unique<ComboBox>();
            std::string selected = first_text_descendant(node).value_or(text);
            std::vector<std::string> items;
            if (!selected.empty()) items.push_back(selected);
            combo->set_items(std::move(items));
            combo->set_selected_silent(0);
            return combo;
        }
        case NativeWidgetKind::checkbox: {
            auto checkbox = std::make_unique<Checkbox>();
            checkbox->set_checked(semantics.checked);
            return checkbox;
        }
        case NativeWidgetKind::toggle_button: {
            auto button = std::make_unique<ToggleButton>();
            if (!text.empty()) button->set_label(text);
            button->set_on(semantics.toggle_on);
            if (semantics.toggle_on_background_color) {
                if (auto parsed = parse_any_css_color(*semantics.toggle_on_background_color)) button->set_on_background_color(*parsed);
            }
            if (semantics.toggle_off_background_color) {
                if (auto parsed = parse_any_css_color(*semantics.toggle_off_background_color)) button->set_off_background_color(*parsed);
            }
            if (semantics.toggle_on_text_color) {
                if (auto parsed = parse_any_css_color(*semantics.toggle_on_text_color)) button->set_on_text_color(*parsed);
            }
            if (semantics.toggle_off_text_color) {
                if (auto parsed = parse_any_css_color(*semantics.toggle_off_text_color)) button->set_off_text_color(*parsed);
            }
            if (semantics.toggle_on_border_color) {
                if (auto parsed = parse_any_css_color(*semantics.toggle_on_border_color)) button->set_on_border_color(*parsed);
            }
            if (semantics.toggle_off_border_color) {
                if (auto parsed = parse_any_css_color(*semantics.toggle_off_border_color)) button->set_off_border_color(*parsed);
            }
            if (semantics.toggle_corner_radius)
                button->set_corner_radius(*semantics.toggle_corner_radius);
            if (semantics.toggle_font_size)
                button->set_font_size(*semantics.toggle_font_size);
            return button;
        }
        case NativeWidgetKind::knob: {
            auto knob = std::make_unique<Knob>();
            if (!text.empty()) knob->set_label(text);
            knob->set_value(semantics.normalized_value);
            knob->set_default_value(semantics.normalized_default);
            if (semantics.widget_schema)
                knob->set_widget_schema(*semantics.widget_schema);
            if (!semantics.show_internal_label)
                knob->set_show_label(false);
            if (options.preview_mode) knob->set_render_style(WidgetRenderStyle::minimal);
            // Captured-art skin (design's disc + native notch overlay): keeps the
            // knob design-faithful AND interactive. See hoist_captured_art_knobs.
            apply_captured_art_knob_skin(*knob, node);
            return knob;
        }
        case NativeWidgetKind::fader: {
            auto fader = std::make_unique<Fader>();
            if (!text.empty()) fader->set_label(text);
            fader->set_value(semantics.normalized_value);
            if (semantics.horizontal)
                fader->set_orientation(Fader::Orientation::horizontal);
            if (semantics.widget_schema)
                fader->set_widget_schema(*semantics.widget_schema);
            if (semantics.fader_thumb_shape) {
                if (*semantics.fader_thumb_shape == ImportedFaderThumbShape::rectangle)
                    fader->set_thumb_shape(Fader::ThumbShape::rectangle);
                else if (*semantics.fader_thumb_shape == ImportedFaderThumbShape::circle)
                    fader->set_thumb_shape(Fader::ThumbShape::circle);
            }
            if (semantics.fader_thumb_width || semantics.fader_thumb_height) {
                fader->set_thumb_size(semantics.fader_thumb_width.value_or(0.0f),
                                      semantics.fader_thumb_height.value_or(0.0f));
            }
            if (semantics.fader_thumb_corner_radius)
                fader->set_thumb_corner_radius(*semantics.fader_thumb_corner_radius);
            if (options.preview_mode) fader->set_render_style(WidgetRenderStyle::minimal);
            return fader;
        }
        case NativeWidgetKind::meter: {
            auto meter = std::make_unique<Meter>();
            meter->set_level(semantics.normalized_value, semantics.peak_value);
            if (semantics.horizontal)
                meter->set_orientation(Meter::Orientation::horizontal);
            if (options.preview_mode) meter->set_render_style(WidgetRenderStyle::minimal);
            return meter;
        }
        case NativeWidgetKind::xy_pad: {
            auto pad = std::make_unique<XYPad>();
            pad->set_x(semantics.x_value);
            pad->set_y(semantics.y_value);
            if (semantics.x_label) pad->set_x_label(*semantics.x_label);
            if (semantics.y_label) pad->set_y_label(*semantics.y_label);
            return pad;
        }
        case NativeWidgetKind::waveform: {
            auto waveform = std::make_unique<WaveformView>();
            if (semantics.waveform_shape)
                waveform->set_preview_shape(*semantics.waveform_shape);
            return waveform;
        }
        case NativeWidgetKind::spectrum:
            return std::make_unique<SpectrumView>();
        case NativeWidgetKind::image_view: {
            const auto asset_id = first_asset_id(node);
            if (!asset_id)
                return make_asset_placeholder(node, path, "", diagnostics);
            const IRAssetRef* asset = manifest.resolve(*asset_id);
            if (asset == nullptr)
                return make_asset_placeholder(node, path, *asset_id, diagnostics);
            auto uri = asset_uri(*asset);
            if (uri.empty())
                return make_asset_placeholder(node, path, *asset_id, diagnostics);
            auto image = std::make_unique<ImageView>();
            image->set_image_source(uri);
            // Forward the importer-sampled per-shape gradient. Storing it is
            // inert until a fill value is driven (set_fill_value), so it never
            // alters a plainly-rendered image — the shape-fill stays opt-in.
            // When post-import wiring DOES drive the fill, each shape reveals
            // ITS own colors instead of one generic fill color.
            if (auto grad = attr(node, "shape_fill_gradient")) {
                std::vector<Color> stops;
                std::stringstream gs(*grad);
                std::string tok;
                while (std::getline(gs, tok, ','))
                    if (auto c = parse_any_css_color(tok)) stops.push_back(*c);
                if (stops.size() >= 2) image->set_fill_gradient(std::move(stops));
            }
            return image;
        }
        case NativeWidgetKind::canvas:
            return std::make_unique<CanvasWidget>();
        case NativeWidgetKind::svg_path: {
            auto svg = std::make_unique<SvgPathWidget>();
            if (auto path_data = attr(node, "d")) svg->set_path(*path_data);
            if (auto viewbox = attr(node, "viewBox")) {
                std::istringstream input(*viewbox);
                float min_x = 0.0f, min_y = 0.0f, width = 0.0f, height = 0.0f;
                if (input >> min_x >> min_y >> width >> height) svg->set_viewbox(width, height);
            } else if (node.style.width && node.style.height) {
                svg->set_viewbox(*node.style.width, *node.style.height);
            }
            apply_svg_paint(*svg, node);
            return svg;
        }
        case NativeWidgetKind::svg_rect: {
            auto rect = std::make_unique<SvgRectWidget>();
            rect->set_rect(attr_float(node, "x").value_or(0.0f),
                           attr_float(node, "y").value_or(0.0f),
                           attr_float(node, "width").value_or(node.style.width.value_or(0.0f)),
                           attr_float(node, "height").value_or(node.style.height.value_or(0.0f)));
            apply_svg_paint(*rect, node);
            return rect;
        }
        case NativeWidgetKind::svg_line: {
            auto line = std::make_unique<SvgLineWidget>();
            line->set_line(attr_float(node, "x1").value_or(0.0f),
                           attr_float(node, "y1").value_or(0.0f),
                           attr_float(node, "x2").value_or(0.0f),
                           attr_float(node, "y2").value_or(0.0f));
            apply_svg_paint(*line, node);
            return line;
        }
        case NativeWidgetKind::view:
            return std::make_unique<View>();
    }
    return std::make_unique<View>();
}

std::unique_ptr<View> materialize_node(const IRNode& node,
                                       const ResolvedNativeNode& resolved,
                                       const IRAssetManifest& manifest,
                                       const NativeMaterializeOptions& options,
                                       std::string_view path,
                                       std::optional<LayoutDirection> parent_direction,
                                       std::vector<ImportDiagnostic>& diagnostics) {
    // Faithful-vector lane: render this node's own SVG export via DesignFrameView
    // and overlay native interaction from the typed element list. Falls through
    // to normal materialization (with a diagnostic) if the SVG asset can't be
    // resolved, so a bad asset degrades rather than blanks.
    if (node.render_mode == NodeRenderMode::faithful_svg) {
        if (auto frame = make_faithful_svg_frame(node, manifest, path, diagnostics))
            return frame;
    }

    // An unconfigured "Dropdown" template renders nothing (a zero-size, inert
    // view) — it's a design-system placeholder the design never shows.
    if (is_unconfigured_dropdown_template(node)) {
        auto hidden = std::make_unique<View>();
        hidden->set_hit_testable(false);
        return hidden;
    }
    auto view = make_widget(node, resolved, manifest, options, path, diagnostics);
    apply_identity(*view, node, resolved);
    apply_layout(*view, node, parent_direction);
    apply_visual_style(*view, node.style,
                       /*skip_border=*/resolved.kind == NativeWidgetKind::image_view);
    if (resolved.kind == NativeWidgetKind::image_view)
        apply_imported_image_sizing(*view, node);

    // A ComboBox paints its own box + selected text + chevron, so the captured
    // "Dropdown" frame's text/chevron children must NOT be materialized on top
    // (they would double-render). Treat it as a leaf.
    if (resolved.kind == NativeWidgetKind::combo_box)
        return view;

    // A promoted search CONTAINER → a TextEditor that paints its own box +
    // placeholder. Keep only IMAGE children (the magnifier icon, an overlay);
    // drop the placeholder text + background-pill chrome the editor replaces.
    const bool editor_keeps_images_only =
        resolved.kind == NativeWidgetKind::text_editor && !node.children.empty();

    const auto count = std::min(node.children.size(), resolved.children.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (editor_keeps_images_only &&
            resolved.children[i].kind != NativeWidgetKind::image_view)
            continue;
        std::ostringstream child_path;
        child_path << path << "/children[" << i << "]";
        auto child = materialize_node(node.children[i],
                                      resolved.children[i],
                                      manifest,
                                      options,
                                      child_path.str(),
                                      node.layout.direction,
                                      diagnostics);
        if (native_kind_owns_imported_child_hits(resolved.kind)) {
            switch (promoted_widget_child_hit_policy(node.children[i], resolved.children[i])) {
                case PromotedChildHitPolicy::disabled:
                    child->set_hit_testable(false);
                    break;
                case PromotedChildHitPolicy::pass_through_self:
                    child->set_pointer_events(View::PointerEvents::box_none);
                    break;
                case PromotedChildHitPolicy::unchanged:
                    break;
            }
        }
        view->add_child(std::move(child));
    }
    return view;
}

std::unique_ptr<View> materialize_error_view(const char* message,
                                             std::vector<ImportDiagnostic>* diagnostics) {
    if (diagnostics != nullptr) {
        ImportDiagnostic diagnostic;
        diagnostic.severity = ImportDiagnosticSeverity::error;
        diagnostic.kind = ImportDiagnosticKind::fallback_used;
        diagnostic.code = "native-materialize-failed";
        diagnostic.path = "$";
        diagnostic.message = message;
        diagnostics->push_back(std::move(diagnostic));
    }
    auto view = std::make_unique<View>();
    view->set_id("native-materialize-error");
    return view;
}

} // namespace

// Exported (design_import_native_common.hpp). Defined in the named namespace so
// both the runtime materializer and the C++ codegen size an imported image and
// resolve hit ownership from one definition. Call the attr/lower helpers from
// the anonymous namespace above (visible throughout this translation unit).
std::optional<ImportedImageSizing> imported_image_sizing_override(const IRNode& node) {
    const float box_w = node.style.width.value_or(0.0f);
    const float box_h = node.style.height.value_or(0.0f);
    if (box_w <= 0.0f || box_h <= 0.0f) return std::nullopt;

    const float png_w = attr_float(node, "png_natural_w").value_or(0.0f);
    const float png_h = attr_float(node, "png_natural_h").value_or(0.0f);
    const float core_w = attr_float(node, "art_core_w").value_or(0.0f);
    const float core_h = attr_float(node, "art_core_h").value_or(0.0f);
    const float core_x = attr_float(node, "art_core_x").value_or(0.0f);
    const float core_y = attr_float(node, "art_core_y").value_or(0.0f);

    const bool have_core =
        png_w > 0.0f && png_h > 0.0f &&
        core_w > 0.0f && core_h > 0.0f;
    const bool is_bleed_sprite =
        node.style.render_bounds.has_value() ||
        (node.attributes.count("asset_bleed") && node.attributes.at("asset_bleed") == "1");

    ImportedImageSizing out;
    const bool absolute =
        node.style.position && lower_copy(*node.style.position) == "absolute";

    if (have_core) {
        const float scale = std::min(box_w / core_w, box_h / core_h);
        out.width = png_w * scale;
        out.height = png_h * scale;

        const float core_box_w = core_w * scale;
        const float core_box_h = core_h * scale;
        const float pad_x = (box_w - core_box_w) * 0.5f;
        const float pad_y = (box_h - core_box_h) * 0.5f;
        if (absolute && node.style.left)
            out.left = *node.style.left - core_x * scale + pad_x;
        if (absolute && node.style.top)
            out.top = *node.style.top - core_y * scale + pad_y;
        return out;
    }

    if (is_bleed_sprite && png_w > 0.0f && png_h > 0.0f) {
        const float png_aspect = png_w / png_h;
        if (box_w / box_h > png_aspect) {
            out.height = box_h;
            out.width = box_h * png_aspect;
        } else {
            out.width = box_w;
            out.height = box_w / png_aspect;
        }

        if (absolute && node.style.left)
            out.left = *node.style.left + (box_w - out.width) * 0.5f;
        if (absolute && node.style.top)
            out.top = *node.style.top + (box_h - out.height) * 0.5f;
        return out;
    }

    return std::nullopt;
}

bool is_interactive_native_kind(NativeWidgetKind kind) {
    switch (kind) {
        case NativeWidgetKind::text_button:
        case NativeWidgetKind::text_editor:
        case NativeWidgetKind::checkbox:
        case NativeWidgetKind::toggle_button:
        case NativeWidgetKind::combo_box:
        case NativeWidgetKind::knob:
        case NativeWidgetKind::fader:
        case NativeWidgetKind::xy_pad:
            return true;
        case NativeWidgetKind::view:
        case NativeWidgetKind::label:
        case NativeWidgetKind::meter:
        case NativeWidgetKind::waveform:
        case NativeWidgetKind::spectrum:
        case NativeWidgetKind::image_view:
        case NativeWidgetKind::canvas:
        case NativeWidgetKind::svg_path:
        case NativeWidgetKind::svg_rect:
        case NativeWidgetKind::svg_line:
            return false;
    }
    return false;
}

bool native_kind_owns_imported_child_hits(NativeWidgetKind kind) {
    switch (kind) {
        case NativeWidgetKind::text_button:
        case NativeWidgetKind::text_editor:
        case NativeWidgetKind::checkbox:
        case NativeWidgetKind::toggle_button:
        case NativeWidgetKind::combo_box:
        case NativeWidgetKind::knob:
        case NativeWidgetKind::fader:
        case NativeWidgetKind::meter:
        case NativeWidgetKind::xy_pad:
        case NativeWidgetKind::waveform:
        case NativeWidgetKind::spectrum:
            return true;
        case NativeWidgetKind::view:
        case NativeWidgetKind::label:
        case NativeWidgetKind::image_view:
        case NativeWidgetKind::canvas:
        case NativeWidgetKind::svg_path:
        case NativeWidgetKind::svg_rect:
        case NativeWidgetKind::svg_line:
            return false;
    }
    return false;
}

bool subtree_contains_interactive_hit_target(const IRNode& node,
                                             const ResolvedNativeNode& resolved) {
    if (auto hit_testable = attr(node, "pulpHitTestable"))
        return attr_bool(node, "pulpHitTestable");
    if (is_interactive_native_kind(resolved.kind))
        return true;

    const auto count = std::min(node.children.size(), resolved.children.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (subtree_contains_interactive_hit_target(node.children[i], resolved.children[i]))
            return true;
    }
    return false;
}

PromotedChildHitPolicy promoted_widget_child_hit_policy(const IRNode& child,
                                                        const ResolvedNativeNode& resolved_child) {
    if (auto hit_testable = attr(child, "pulpHitTestable"))
        return attr_bool(child, "pulpHitTestable")
            ? PromotedChildHitPolicy::unchanged
            : PromotedChildHitPolicy::disabled;
    if (is_interactive_native_kind(resolved_child.kind))
        return PromotedChildHitPolicy::unchanged;
    if (subtree_contains_interactive_hit_target(child, resolved_child))
        return PromotedChildHitPolicy::pass_through_self;
    return PromotedChildHitPolicy::disabled;
}

// Exported (design_import_native_common.hpp). Defined in the named namespace so
// both the runtime materializer and the C++ codegen lower a faithful_svg node
// from identical resolved bytes. Calls svg_percent_decode from the anonymous
// namespace above (visible throughout this translation unit).
std::string resolve_svg_document(const IRAssetRef& asset) {
    const std::string& uri = asset.original_uri;
    if (uri.rfind("data:", 0) == 0) {
        const auto comma = uri.find(',');
        if (comma == std::string::npos) return {};
        const std::string meta = uri.substr(5, comma - 5);
        const std::string payload = uri.substr(comma + 1);
        if (meta.find(";base64") != std::string::npos) {
            if (auto decoded = runtime::base64_decode(payload))
                return std::string(decoded->begin(), decoded->end());
            return {};
        }
        return svg_percent_decode(payload);
    }
    auto read_file = [](const std::string& path) -> std::string {
        std::ifstream f(path, std::ios::binary);
        if (!f) return {};
        return std::string(std::istreambuf_iterator<char>(f),
                           std::istreambuf_iterator<char>());
    };
    if (asset.local_path && !asset.local_path->empty())
        return read_file(*asset.local_path);
    if (uri.rfind("file://", 0) == 0)
        return read_file(uri.substr(7));
    return {};
}

const char* native_widget_kind_name(NativeWidgetKind kind) {
    switch (kind) {
        case NativeWidgetKind::view: return "view";
        case NativeWidgetKind::label: return "label";
        case NativeWidgetKind::text_button: return "text_button";
        case NativeWidgetKind::text_editor: return "text_editor";
        case NativeWidgetKind::checkbox: return "checkbox";
        case NativeWidgetKind::toggle_button: return "toggle_button";
        case NativeWidgetKind::combo_box: return "combo_box";
        case NativeWidgetKind::knob: return "knob";
        case NativeWidgetKind::fader: return "fader";
        case NativeWidgetKind::meter: return "meter";
        case NativeWidgetKind::xy_pad: return "xy_pad";
        case NativeWidgetKind::waveform: return "waveform";
        case NativeWidgetKind::spectrum: return "spectrum";
        case NativeWidgetKind::image_view: return "image_view";
        case NativeWidgetKind::canvas: return "canvas";
        case NativeWidgetKind::svg_path: return "svg_path";
        case NativeWidgetKind::svg_rect: return "svg_rect";
        case NativeWidgetKind::svg_line: return "svg_line";
    }
    return "view";
}

ImportedWidgetSemantics imported_widget_semantics(const IRNode& node,
                                                  const ResolvedNativeNode& resolved) {
    ImportedWidgetSemantics out;
    out.text = resolved.text.value_or(node.text_content);

    // Single typed parse of the pulp* binding contract; all pulp* reads below
    // go through this model. Non-pulp* attributes (checked/value/orientation/
    // x/y/jsxTag/...) stay on the raw attr() helpers.
    const auto md = NativeBindingMetadata::parse(node);

    auto non_empty = [](const std::optional<std::string>& value)
        -> std::optional<std::string> {
        if (value && !value->empty()) return value;
        return std::nullopt;
    };

    out.text_placeholder = non_empty(md.placeholder);
    if (auto value = non_empty(md.initial_value))
        out.text_value = value;
    else if (auto value = attr(node, "value"); value && !value->empty())
        out.text_value = value;
    else if (!out.text.empty()) {
        // Only treat display text as the editor's value for a <textarea>, whose
        // element body IS the value. For other controls the node's text_content
        // is typically an adjacent label/heading, not editable contents —
        // injecting it would prefill the editor with a label.
        const auto& family = md.source_family;
        const auto tag = attr(node, "jsxTag");
        const bool is_textarea =
            (family && lower_copy(*family) == "textarea") ||
            (tag && lower_copy(*tag) == "textarea");
        if (is_textarea)
            out.text_value = out.text;
    }

    out.checked = attr_bool(node, "checked");
    out.toggle_on = out.checked || attr_bool(node, "value");
    out.toggle_on_background_color = non_empty(md.on_background_color);
    out.toggle_off_background_color = non_empty(md.off_background_color);
    out.toggle_on_text_color = non_empty(md.on_text_color);
    out.toggle_off_text_color = non_empty(md.off_text_color);
    out.toggle_on_border_color = non_empty(md.on_border_color);
    out.toggle_off_border_color = non_empty(md.off_border_color);
    out.toggle_corner_radius = md.corner_radius_value();
    out.toggle_font_size = md.font_size_value();

    out.normalized_value = normalized_audio_value(node);
    out.normalized_default = normalized_audio_default(node);
    out.peak_value = attr_float(node, "peak").value_or(out.normalized_value);

    if (resolved.kind == NativeWidgetKind::fader) {
        if (auto orientation = attr(node, "orientation");
            orientation && lower_copy(*orientation) == "horizontal") {
            out.horizontal = true;
        } else if (auto type = attr(node, "type"); type && lower_copy(*type) == "range") {
            out.horizontal = true;
        } else if (node.style.width && node.style.height && *node.style.width >= *node.style.height) {
            out.horizontal = true;
        }
    } else if (resolved.kind == NativeWidgetKind::meter) {
        if (auto orientation = attr(node, "orientation");
            orientation && lower_copy(*orientation) == "horizontal") {
            out.horizontal = true;
        }
    }

    out.widget_schema = non_empty(md.widget_schema);
    out.show_internal_label = md.show_internal_label_value();

    if (md.thumb_shape) {
        const auto lower = lower_copy(*md.thumb_shape);
        if (lower == "rectangle" || lower == "rect" || lower == "rounded_rect")
            out.fader_thumb_shape = ImportedFaderThumbShape::rectangle;
        else if (lower == "circle" || lower == "round" || lower == "dot")
            out.fader_thumb_shape = ImportedFaderThumbShape::circle;
    }
    out.fader_thumb_width = md.thumb_width_value();
    out.fader_thumb_height = md.thumb_height_value();
    out.fader_thumb_corner_radius = md.thumb_corner_radius_value();

    out.x_value = attr_float(node, "x").value_or(0.5f);
    out.y_value = attr_float(node, "y").value_or(0.5f);
    out.x_label = non_empty(attr(node, "xLabel"));
    out.y_label = non_empty(attr(node, "yLabel"));
    out.waveform_shape = non_empty(md.waveform_shape);

    return out;
}

ResolvedNativeNode resolve_design_ir_native(const DesignIR& ir,
                                            const IRAssetManifest& manifest) {
    const auto& effective_manifest = manifest.assets.empty() ? ir.asset_manifest : manifest;
    auto resolved = resolve_node(ir.root, "$", index_assets(effective_manifest));
    resolved.diagnostics.insert(resolved.diagnostics.end(),
                                ir.diagnostics.begin(),
                                ir.diagnostics.end());
    return resolved;
}

ResolvedNativeNode resolve_design_ir_native_json(std::string_view frozen_design_ir_json,
                                                 const IRAssetManifest& manifest) {
    auto ir = parse_design_ir_json(std::string(frozen_design_ir_json));
    return resolve_design_ir_native(ir, manifest);
}

NativeImportBindingContext::~NativeImportBindingContext() {
    reset_import_binding_claims();
}

bool NativeImportBindingContext::claim_import_binding(View& view, std::string_view route_id) {
    std::lock_guard<std::mutex> lock(binding_claims_mutex());
    auto& claims = binding_claims_by_context()[this];
    claims.erase(std::remove_if(claims.begin(),
                                claims.end(),
                                [](const BindingClaim& claim) {
                                    return claim.view_lifetime.expired();
                                }),
                 claims.end());
    for (const auto& claim : claims) {
        if (claim.view == &view &&
            claim.view_instance_id == view.import_binding_instance_id() &&
            claim.route_id == route_id) {
            return false;
        }
    }
    claims.push_back(BindingClaim{
        .view = &view,
        .view_instance_id = view.import_binding_instance_id(),
        .view_lifetime = view.import_binding_lifetime_token(),
        .route_id = std::string(route_id)});
    return true;
}

void NativeImportBindingContext::reset_import_binding_claims() {
    std::lock_guard<std::mutex> lock(binding_claims_mutex());
    binding_claims_by_context().erase(this);
}

std::unique_ptr<View> build_native_view_tree(const DesignIR& ir,
                                             const IRAssetManifest& manifest,
                                             const NativeMaterializeOptions& options) {
    try {
        const auto& effective_manifest = manifest.assets.empty() ? ir.asset_manifest : manifest;
        auto resolved = resolve_design_ir_native(ir, effective_manifest);

        std::vector<ImportDiagnostic> materialize_diagnostics;
        append_resolved_diagnostics(resolved, materialize_diagnostics);
        auto root = materialize_node(ir.root,
                                     resolved,
                                     effective_manifest,
                                     options,
                                     "$",
                                     std::nullopt,
                                     materialize_diagnostics);
        if (options.apply_token_theme)
            root->set_theme(ir_tokens_to_theme(ir.tokens));
        if (options.diagnostics_out != nullptr) {
            options.diagnostics_out->insert(options.diagnostics_out->end(),
                                            materialize_diagnostics.begin(),
                                            materialize_diagnostics.end());
        }
        return root;
    } catch (const std::exception& e) {
        return materialize_error_view(e.what(), options.diagnostics_out);
    } catch (...) {
        return materialize_error_view("unknown native materialization failure",
                                      options.diagnostics_out);
    }
}

void bind_native_view_tree(View& root,
                           const DesignIR& ir,
                           NativeImportBindingContext& ctx,
                           const NativeImportBindingOptions& options) {
    bind_imported_node_by_anchor(root, ir.root, ctx, "$", options.diagnostics_out);
}

} // namespace pulp::view
