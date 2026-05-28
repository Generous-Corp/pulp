#include "design_import_native_common.hpp"

#include <pulp/view/buttons.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/svg_path_widget.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/widgets/svg_line.hpp>
#include <pulp/view/widgets/svg_rect.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <memory>
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
    if (type == "toggle_button")
        return NativeWidgetKind::toggle_button;
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

bool attr_bool(const IRNode& node, std::string_view key, bool fallback = false) {
    auto value = attr(node, key);
    if (!value) return fallback;
    const auto lower = lower_copy(*value);
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") return true;
    if (lower == "false" || lower == "0" || lower == "no" || lower == "off") return false;
    return fallback;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::optional<Color> parse_hex_color(std::string_view value) {
    if (value.empty() || value.front() != '#') return std::nullopt;

    auto pair = [](char high, char low) -> std::optional<uint8_t> {
        const int h = hex_digit(high);
        const int l = hex_digit(low);
        if (h < 0 || l < 0) return std::nullopt;
        return static_cast<uint8_t>((h << 4) | l);
    };

    if (value.size() == 4 || value.size() == 5) {
        const int r = hex_digit(value[1]);
        const int g = hex_digit(value[2]);
        const int b = hex_digit(value[3]);
        const int a = value.size() == 5 ? hex_digit(value[4]) : 15;
        if (r < 0 || g < 0 || b < 0 || a < 0) return std::nullopt;
        return Color::rgba8(static_cast<uint8_t>((r << 4) | r),
                            static_cast<uint8_t>((g << 4) | g),
                            static_cast<uint8_t>((b << 4) | b),
                            static_cast<uint8_t>((a << 4) | a));
    }

    if (value.size() == 7 || value.size() == 9) {
        auto r = pair(value[1], value[2]);
        auto g = pair(value[3], value[4]);
        auto b = pair(value[5], value[6]);
        auto a = value.size() == 9 ? pair(value[7], value[8]) : std::optional<uint8_t>(255);
        if (!r || !g || !b || !a) return std::nullopt;
        return Color::rgba8(*r, *g, *b, *a);
    }

    return std::nullopt;
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

std::optional<std::string> first_asset_id(const IRNode& node) {
    for (std::string_view key : {"srcAssetId", "backgroundImageAssetId", "hrefAssetId"}) {
        auto value = attr(node, key);
        if (value && !value->empty()) return value;
    }

    std::vector<std::pair<std::string, std::string>> candidates;
    for (const auto& [key, value] : node.attributes) {
        if (ends_with(key, "AssetId") && !value.empty())
            candidates.emplace_back(key, value);
    }
    std::sort(candidates.begin(), candidates.end());
    if (!candidates.empty()) return candidates.front().second;
    return std::nullopt;
}

std::string asset_uri(const IRAssetRef& asset) {
    if (asset.local_path && !asset.local_path->empty())
        return "file://" + *asset.local_path;
    if (!asset.original_uri.empty() &&
        (asset.original_uri.rfind("data:", 0) == 0 ||
         asset.original_uri.rfind("resource:", 0) == 0 ||
         asset.original_uri.rfind("memory:", 0) == 0)) {
        return asset.original_uri;
    }
    return {};
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
    if (auto label = resolved.text; label && !label->empty())
        view.set_access_label(*label);
}

void apply_layout(View& view, const IRNode& node, std::optional<LayoutDirection> parent_direction) {
    if (node.layout.display && *node.layout.display == "grid") {
        view.set_layout_mode(LayoutMode::grid);
        auto& grid = view.grid();
        if (auto it = node.attributes.find("pulpGridTemplateColumns"); it != node.attributes.end())
            grid.template_columns = GridStyle::parse_template(it->second);
        if (auto it = node.attributes.find("pulpGridTemplateRows"); it != node.attributes.end())
            grid.template_rows = GridStyle::parse_template(it->second);
        grid.column_gap = node.layout.column_gap.value_or(node.layout.gap);
        grid.row_gap = node.layout.row_gap.value_or(node.layout.gap);
    }

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

void apply_visual_style(View& view, const IRStyle& style) {
    if (style.background_color) {
        if (auto color = parse_hex_color(*style.background_color))
            view.set_background_color(*color);
    }
    if (style.background_repeat)
        view.set_background_repeat(*style.background_repeat);
    if (style.color) {
        if (auto color = parse_hex_color(*style.color))
            view.set_inheritable_text_color(*color);
    }
    if (style.opacity)
        view.set_opacity(*style.opacity);
    if (style.border_radius)
        view.set_border_radius(*style.border_radius);
    if (style.border_width)
        view.set_border_width(*style.border_width);
    if (style.border_color) {
        if (auto color = parse_hex_color(*style.border_color))
            view.set_border_color(*color);
    }
    if (style.border_top_width) view.set_border_top_width(*style.border_top_width);
    if (style.border_right_width) view.set_border_right_width(*style.border_right_width);
    if (style.border_bottom_width) view.set_border_bottom_width(*style.border_bottom_width);
    if (style.border_left_width) view.set_border_left_width(*style.border_left_width);
    if (style.border_top_color) {
        if (auto color = parse_hex_color(*style.border_top_color))
            view.set_border_top_color(*color);
    }
    if (style.border_right_color) {
        if (auto color = parse_hex_color(*style.border_right_color))
            view.set_border_right_color(*color);
    }
    if (style.border_bottom_color) {
        if (auto color = parse_hex_color(*style.border_bottom_color))
            view.set_border_bottom_color(*color);
    }
    if (style.border_left_color) {
        if (auto color = parse_hex_color(*style.border_left_color))
            view.set_border_left_color(*color);
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
        if (auto overflow = parse_overflow(*style.overflow)) view.set_overflow(*overflow);
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
        if (auto color = parse_hex_color(*style.color)) label.set_text_color(*color);
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
}

void apply_svg_paint(SvgPathWidget& path, const IRNode& node) {
    if (auto fill = attr(node, "fill")) {
        if (*fill == "none") path.clear_fill();
        else if (auto color = parse_hex_color(*fill)) path.set_fill_color(*color);
    }
    if (auto stroke = attr(node, "stroke")) {
        if (*stroke == "none") path.clear_stroke();
        else if (auto color = parse_hex_color(*stroke)) path.set_stroke_color(*color);
    }
    if (auto stroke_width = attr_float(node, "stroke-width"))
        path.set_stroke_width(*stroke_width);
}

void apply_svg_paint(SvgRectWidget& rect, const IRNode& node) {
    if (auto fill = attr(node, "fill")) {
        if (*fill == "none") rect.clear_fill();
        else if (auto color = parse_hex_color(*fill)) rect.set_fill_color(*color);
    }
    if (auto stroke = attr(node, "stroke")) {
        if (*stroke == "none") rect.clear_stroke();
        else if (auto color = parse_hex_color(*stroke)) rect.set_stroke_color(*color);
    }
    if (auto stroke_width = attr_float(node, "stroke-width"))
        rect.set_stroke_width(*stroke_width);
}

void apply_svg_paint(SvgLineWidget& line, const IRNode& node) {
    if (auto stroke = attr(node, "stroke")) {
        if (*stroke == "none") line.clear_stroke();
        else if (auto color = parse_hex_color(*stroke)) line.set_stroke_color(*color);
    }
    if (auto stroke_width = attr_float(node, "stroke-width"))
        line.set_stroke_width(*stroke_width);
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
            if (semantics.text_placeholder)
                editor->placeholder = *semantics.text_placeholder;
            if (semantics.text_value)
                editor->set_text(*semantics.text_value);
            return editor;
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
                if (auto parsed = parse_hex_color(*semantics.toggle_on_background_color)) button->set_on_background_color(*parsed);
            }
            if (semantics.toggle_off_background_color) {
                if (auto parsed = parse_hex_color(*semantics.toggle_off_background_color)) button->set_off_background_color(*parsed);
            }
            if (semantics.toggle_on_text_color) {
                if (auto parsed = parse_hex_color(*semantics.toggle_on_text_color)) button->set_on_text_color(*parsed);
            }
            if (semantics.toggle_off_text_color) {
                if (auto parsed = parse_hex_color(*semantics.toggle_off_text_color)) button->set_off_text_color(*parsed);
            }
            if (semantics.toggle_on_border_color) {
                if (auto parsed = parse_hex_color(*semantics.toggle_on_border_color)) button->set_on_border_color(*parsed);
            }
            if (semantics.toggle_off_border_color) {
                if (auto parsed = parse_hex_color(*semantics.toggle_off_border_color)) button->set_off_border_color(*parsed);
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
    auto view = make_widget(node, resolved, manifest, options, path, diagnostics);
    apply_identity(*view, node, resolved);
    apply_layout(*view, node, parent_direction);
    apply_visual_style(*view, node.style);

    const auto count = std::min(node.children.size(), resolved.children.size());
    for (std::size_t i = 0; i < count; ++i) {
        std::ostringstream child_path;
        child_path << path << "/children[" << i << "]";
        view->add_child(materialize_node(node.children[i],
                                         resolved.children[i],
                                         manifest,
                                         options,
                                         child_path.str(),
                                         node.layout.direction,
                                         diagnostics));
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

const char* native_widget_kind_name(NativeWidgetKind kind) {
    switch (kind) {
        case NativeWidgetKind::view: return "view";
        case NativeWidgetKind::label: return "label";
        case NativeWidgetKind::text_button: return "text_button";
        case NativeWidgetKind::text_editor: return "text_editor";
        case NativeWidgetKind::checkbox: return "checkbox";
        case NativeWidgetKind::toggle_button: return "toggle_button";
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

    auto non_empty_attr = [&](std::string_view key) -> std::optional<std::string> {
        auto value = attr(node, key);
        if (value && !value->empty()) return value;
        return std::nullopt;
    };

    out.text_placeholder = non_empty_attr("pulpPlaceholder");
    if (auto value = non_empty_attr("pulpInitialValue"))
        out.text_value = value;
    else if (auto value = non_empty_attr("value"))
        out.text_value = value;
    else if (!out.text.empty())
        out.text_value = out.text;

    out.checked = attr_bool(node, "checked");
    out.toggle_on = out.checked || attr_bool(node, "value");
    out.toggle_on_background_color = non_empty_attr("pulpOnBackgroundColor");
    out.toggle_off_background_color = non_empty_attr("pulpOffBackgroundColor");
    out.toggle_on_text_color = non_empty_attr("pulpOnTextColor");
    out.toggle_off_text_color = non_empty_attr("pulpOffTextColor");
    out.toggle_on_border_color = non_empty_attr("pulpOnBorderColor");
    out.toggle_off_border_color = non_empty_attr("pulpOffBorderColor");
    out.toggle_corner_radius = attr_float(node, "pulpCornerRadius");
    out.toggle_font_size = attr_float(node, "pulpFontSize");

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

    out.widget_schema = non_empty_attr("pulpWidgetSchema");
    if (auto show_label = attr(node, "pulpShowInternalLabel");
        show_label && lower_copy(*show_label) == "false") {
        out.show_internal_label = false;
    }

    if (auto shape = attr(node, "pulpThumbShape")) {
        const auto lower = lower_copy(*shape);
        if (lower == "rectangle" || lower == "rect" || lower == "rounded_rect")
            out.fader_thumb_shape = ImportedFaderThumbShape::rectangle;
        else if (lower == "circle" || lower == "round" || lower == "dot")
            out.fader_thumb_shape = ImportedFaderThumbShape::circle;
    }
    out.fader_thumb_width = attr_float(node, "pulpThumbWidth");
    out.fader_thumb_height = attr_float(node, "pulpThumbHeight");
    out.fader_thumb_corner_radius = attr_float(node, "pulpThumbCornerRadius");

    out.x_value = attr_float(node, "x").value_or(0.5f);
    out.y_value = attr_float(node, "y").value_or(0.5f);
    out.x_label = non_empty_attr("xLabel");
    out.y_label = non_empty_attr("yLabel");
    out.waveform_shape = non_empty_attr("pulpWaveformShape");

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

} // namespace pulp::view
