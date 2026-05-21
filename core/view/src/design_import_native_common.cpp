#include "design_import_native_common.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::view {
namespace {

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

std::optional<std::string> attr(const IRNode& node, std::string_view key) {
    if (auto it = node.attributes.find(std::string(key)); it != node.attributes.end())
        return it->second;
    return std::nullopt;
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
    if (type == "textarea" || type == "text_editor")
        return NativeWidgetKind::text_editor;
    if (type == "checkbox")
        return NativeWidgetKind::checkbox;
    if (type == "input")
        return input_kind(node, path, diagnostics);
    if (type == "slider" || type == "range")
        return NativeWidgetKind::fader;
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
    add("boxShadow", node.style.box_shadow);
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

ResolvedNativeNode resolve_node(const IRNode& node,
                                std::string_view path,
                                const AssetIndex& assets) {
    ResolvedNativeNode out;
    if (auto audio_kind = kind_from_audio(node.audio_widget)) {
        out.kind = *audio_kind;
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

} // namespace

const char* native_widget_kind_name(NativeWidgetKind kind) {
    switch (kind) {
        case NativeWidgetKind::view: return "view";
        case NativeWidgetKind::label: return "label";
        case NativeWidgetKind::text_button: return "text_button";
        case NativeWidgetKind::text_editor: return "text_editor";
        case NativeWidgetKind::checkbox: return "checkbox";
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

ResolvedNativeNode resolve_design_ir_native(const DesignIR& ir,
                                            const IRAssetManifest& manifest) {
    auto resolved = resolve_node(ir.root, "$", index_assets(manifest));
    resolved.diagnostics.insert(resolved.diagnostics.end(),
                                ir.diagnostics.begin(),
                                ir.diagnostics.end());
    return resolved;
}

ResolvedNativeNode resolve_design_ir_native_json(std::string_view frozen_design_ir_json,
                                                 const IRAssetManifest& manifest) {
    auto ir = parse_design_ir_json(std::string(frozen_design_ir_json));
    return resolve_design_ir_native(ir, manifest.assets.empty() ? ir.asset_manifest : manifest);
}

} // namespace pulp::view
