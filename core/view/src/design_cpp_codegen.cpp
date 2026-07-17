#include <pulp/runtime/base64.hpp>
#include <pulp/view/buttons.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/svg_path_widget.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/widgets/svg_line.hpp>
#include <pulp/view/widgets/svg_rect.hpp>

#include "design_binding_metadata.hpp"
#include "design_import_native_common.hpp"
#include "design_ir_helpers.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulp::view {
namespace {

std::string indent(int depth, int spaces) {
    return std::string(static_cast<std::size_t>(std::max(0, depth * spaces)), ' ');
}

std::string cpp_string_escape(std::string_view input) {
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
            case '\0': out += "\\000"; break;
            default:
                if (c < 0x20) {
                    static constexpr char kOctal[] = "01234567";
                    out += "\\";
                    out += kOctal[(c >> 6) & 0x7];
                    out += kOctal[(c >> 3) & 0x7];
                    out += kOctal[c & 0x7];
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

std::string cpp_string_literal(std::string_view input) {
    return "\"" + cpp_string_escape(input) + "\"";
}

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

std::string format_float(float value) {
    std::ostringstream out;
    out << std::setprecision(7) << value;
    auto text = out.str();
    if (text.find_first_of(".eE") == std::string::npos)
        text += ".0";
    text += "f";
    return text;
}

std::string sanitize_identifier(std::string_view input, std::string_view fallback = "node") {
    std::string out;
    out.reserve(input.size());
    bool previous_underscore = false;
    for (unsigned char c : input) {
        if (std::isalnum(c)) {
            out += static_cast<char>(std::tolower(c));
            previous_underscore = false;
        } else if (c == '_' || c == '-' || c == ' ' || c == '.') {
            if (!previous_underscore && !out.empty()) {
                out += '_';
                previous_underscore = true;
            }
        }
    }
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    if (out.empty())
        out = std::string(fallback);
    if (std::isdigit(static_cast<unsigned char>(out.front())))
        out.insert(out.begin(), '_');
    return out;
}

std::string pascal_identifier(std::string_view input, std::string_view fallback = "Token") {
    std::string out = "k";
    bool capitalize = true;
    for (unsigned char c : input) {
        if (std::isalnum(c)) {
            out += static_cast<char>(capitalize ? std::toupper(c) : c);
            capitalize = false;
        } else {
            capitalize = true;
        }
    }
    if (out == "k")
        out += fallback;
    if (out.size() > 1 && std::isdigit(static_cast<unsigned char>(out[1])))
        out.insert(out.begin() + 1, '_');
    return out;
}

std::string color_literal_expr(std::string_view value) {
    if (auto color = parse_hex_color_rgba(value)) {
        std::ostringstream out;
        out << "pulp::view::Color::rgba8("
            << (*color)[0] << ", " << (*color)[1] << ", "
            << (*color)[2] << ", " << (*color)[3] << ")";
        return out.str();
    }
    return {};
}

std::optional<float> parse_float(std::string_view value) {
    if (value.empty() || std::isspace(static_cast<unsigned char>(value.front())))
        return std::nullopt;
    std::string text(value);
    char* parsed_end = nullptr;
    errno = 0;
    const float out = std::strtof(text.c_str(), &parsed_end);
    if (parsed_end != text.c_str() + text.size() || errno == ERANGE || !std::isfinite(out))
        return std::nullopt;
    return out;
}

std::optional<float> attr_float(const IRNode& node, std::string_view key) {
    auto value = attr(node, key);
    if (!value)
        return std::nullopt;
    return parse_float(*value);
}

// The loadable URI for a manifest asset id; "" when the id does not resolve.
std::string asset_uri(const IRAssetManifest& manifest, std::string_view asset_id) {
    const auto* asset = manifest.resolve(asset_id);
    if (asset == nullptr)
        return {};
    return pulp::view::asset_uri(*asset);
}

std::string flex_direction_expr(LayoutDirection direction) {
    return direction == LayoutDirection::row
        ? "pulp::view::FlexDirection::row"
        : "pulp::view::FlexDirection::column";
}

std::string flex_justify_expr(LayoutAlign align) {
    switch (align) {
        case LayoutAlign::flex_end: return "pulp::view::FlexJustify::end_";
        case LayoutAlign::center: return "pulp::view::FlexJustify::center";
        case LayoutAlign::space_between: return "pulp::view::FlexJustify::space_between";
        case LayoutAlign::space_around: return "pulp::view::FlexJustify::space_around";
        case LayoutAlign::stretch: return "pulp::view::FlexJustify::start";
        case LayoutAlign::flex_start: return "pulp::view::FlexJustify::start";
    }
    return "pulp::view::FlexJustify::start";
}

std::string flex_align_expr(LayoutAlign align) {
    switch (align) {
        case LayoutAlign::flex_end: return "pulp::view::FlexAlign::end";
        case LayoutAlign::center: return "pulp::view::FlexAlign::center";
        case LayoutAlign::stretch: return "pulp::view::FlexAlign::stretch";
        case LayoutAlign::space_between:
        case LayoutAlign::space_around:
        case LayoutAlign::flex_start:
            return "pulp::view::FlexAlign::start";
    }
    return "pulp::view::FlexAlign::start";
}

std::string label_align_expr(std::string_view value) {
    std::string lower;
    lower.reserve(value.size());
    for (unsigned char c : value)
        lower += static_cast<char>(std::tolower(c));
    if (lower == "center") return "pulp::view::LabelAlign::center";
    if (lower == "right" || lower == "end") return "pulp::view::LabelAlign::right";
    if (lower == "justify") return "pulp::view::LabelAlign::justify";
    if (lower == "match-parent") return "pulp::view::LabelAlign::match_parent";
    if (lower == "auto") return "pulp::view::LabelAlign::auto_";
    return "pulp::view::LabelAlign::left";
}

bool is_container_node(const IRNode& node) {
    return !node.children.empty() || node.type == "frame";
}

bool is_structural_component_name(std::string_view name) {
    static const std::set<std::string_view> kNames = {
        "Header", "Sidebar", "Footer", "Toolbar", "Nav", "Main",
        "Section", "Aside", "Panel", "Content",
    };
    return kNames.count(name) != 0;
}

bool is_pascal_case(std::string_view name) {
    if (name.empty() || !std::isupper(static_cast<unsigned char>(name.front())))
        return false;
    for (unsigned char c : name) {
        if (!std::isalnum(c))
            return false;
    }
    return true;
}

struct Component {
    const IRNode* node = nullptr;
    const ResolvedNativeNode* resolved = nullptr;
    std::string function_name;
    std::string rule_comment;
    std::optional<LayoutDirection> parent_direction;
};

struct TokenSymbols {
    std::unordered_map<std::string, std::string> color_by_name;
    std::unordered_map<std::string, std::string> color_by_value;
    std::unordered_map<std::string, std::string> dimension_by_name;
    std::vector<std::pair<float, std::string>> dimension_by_value;
    std::unordered_map<std::string, std::string> string_by_name;
    std::unordered_map<std::string, std::string> string_by_value;
};

struct AssetSymbols {
    std::unordered_map<std::string, std::string> id_by_asset_id;
};

struct EmitContext {
    const CppExportOptions& opts;
    const IRAssetManifest& manifest;
    TokenSymbols tokens;
    AssetSymbols assets;
    std::vector<Component> components;
    std::unordered_map<const IRNode*, std::string> extracted;
    std::set<std::string> used_functions;
    std::set<std::string> used_token_names;
    std::set<std::string> used_asset_names;
};

std::string unique_function_name(EmitContext& ctx, std::string base) {
    if (base.empty())
        base = "build_component";
    std::string candidate = base;
    int suffix = 2;
    while (ctx.used_functions.count(candidate) != 0)
        candidate = base + "_" + std::to_string(suffix++);
    ctx.used_functions.insert(candidate);
    return candidate;
}

void collect_components(const IRNode& node,
                        const ResolvedNativeNode& resolved,
                        EmitContext& ctx,
                        std::optional<LayoutDirection> parent_direction,
                        bool root = false) {
    if (!root && ctx.opts.extract_named_components && is_container_node(node) && !node.name.empty()) {
        std::string rule;
        if (is_structural_component_name(node.name)) {
            rule = "structural name \"" + node.name + "\"";
        } else if (is_pascal_case(node.name)) {
            rule = "PascalCase container \"" + node.name + "\"";
        }
        if (!rule.empty()) {
            const auto fn = unique_function_name(ctx, "build_" + sanitize_identifier(node.name, "component"));
            ctx.extracted[&node] = fn;
            ctx.components.push_back(Component{&node, &resolved, fn, "auto-extracted: " + rule, parent_direction});
        }
    }

    const auto count = std::min(node.children.size(), resolved.children.size());
    for (std::size_t i = 0; i < count; ++i)
        collect_components(node.children[i], resolved.children[i], ctx, node.layout.direction);
}

void emit_line(std::ostringstream& out, int depth, int spaces, std::string_view text) {
    out << indent(depth, spaces) << text << "\n";
}

void emit_optional_float(std::ostringstream& out,
                         int depth,
                         const CppExportOptions& opts,
                         std::string_view target,
                         std::string_view field,
                         const std::optional<float>& value,
                         std::string_view expr = {}) {
    if (value)
        emit_line(out, depth, opts.indent_spaces,
                  std::string(target) + "." + std::string(field) + " = " +
                      (expr.empty() ? format_float(*value) : std::string(expr)) + ";");
}

std::string unique_symbol(std::set<std::string>& used, std::string base) {
    if (base.empty())
        base = "kValue";
    std::string candidate = base;
    int suffix = 2;
    while (used.count(candidate) != 0)
        candidate = base + std::to_string(suffix++);
    used.insert(candidate);
    return candidate;
}

TokenSymbols build_token_symbols(const DesignIR& ir, EmitContext& ctx) {
    TokenSymbols symbols;
    if (!ctx.opts.emit_named_tokens)
        return symbols;

    for (const auto& [name, value] : ir.tokens.colors) {
        const auto symbol = "tokens::" + unique_symbol(ctx.used_token_names, pascal_identifier(name, "Color"));
        symbols.color_by_name.emplace(name, symbol);
        if (parse_hex_color_rgba(value))
            symbols.color_by_value.emplace(value, symbol);
    }
    for (const auto& [name, value] : ir.tokens.dimensions) {
        const auto symbol = "tokens::" + unique_symbol(ctx.used_token_names, pascal_identifier(name, "Dim"));
        symbols.dimension_by_name.emplace(name, symbol);
        symbols.dimension_by_value.emplace_back(value, symbol);
    }
    for (const auto& [name, value] : ir.tokens.strings) {
        const auto symbol = "tokens::" + unique_symbol(ctx.used_token_names, pascal_identifier(name, "String"));
        symbols.string_by_name.emplace(name, symbol);
        symbols.string_by_value.emplace(value, symbol);
    }
    return symbols;
}

AssetSymbols build_asset_symbols(const IRAssetManifest& manifest, EmitContext& ctx) {
    AssetSymbols symbols;
    if (!ctx.opts.emit_asset_constants)
        return symbols;
    for (const auto& asset : manifest.assets) {
        if (asset.asset_id.empty())
            continue;
        const auto symbol = "assets::" + unique_symbol(
            ctx.used_asset_names,
            pascal_identifier(asset.asset_id.empty() ? asset.original_uri : asset.asset_id, "Asset"));
        symbols.id_by_asset_id.emplace(asset.asset_id, symbol);
    }
    return symbols;
}

std::string color_expr(const EmitContext& ctx, std::string_view value) {
    if (ctx.opts.emit_named_tokens) {
        auto found = ctx.tokens.color_by_value.find(std::string(value));
        if (found != ctx.tokens.color_by_value.end())
            return found->second;
    }
    return color_literal_expr(value);
}

std::string float_expr(const EmitContext& ctx, float value) {
    if (ctx.opts.emit_named_tokens) {
        for (const auto& [token_value, symbol] : ctx.tokens.dimension_by_value) {
            if (std::fabs(token_value - value) < 0.0001f)
                return symbol;
        }
    }
    return format_float(value);
}

std::string asset_id_expr(const EmitContext& ctx, std::string_view asset_id) {
    if (ctx.opts.emit_asset_constants) {
        auto found = ctx.assets.id_by_asset_id.find(std::string(asset_id));
        if (found != ctx.assets.id_by_asset_id.end())
            return found->second;
    }
    return cpp_string_literal(asset_id);
}

std::optional<std::string> flex_align_value_expr(std::string_view value) {
    const auto lower = lower_copy(value);
    if (lower == "flex-end" || lower == "end") return "pulp::view::FlexAlign::end";
    if (lower == "center") return "pulp::view::FlexAlign::center";
    if (lower == "stretch") return "pulp::view::FlexAlign::stretch";
    if (lower == "baseline") return "pulp::view::FlexAlign::baseline";
    if (lower == "auto") return "pulp::view::FlexAlign::auto_";
    if (lower == "flex-start" || lower == "start") return "pulp::view::FlexAlign::start";
    return std::nullopt;
}

void emit_common_layout(std::ostringstream& out,
                        int depth,
                        const EmitContext& ctx,
                        std::string_view var,
                        const IRNode& node,
                        std::optional<LayoutDirection> parent_direction) {
    const auto& opts = ctx.opts;
    emit_line(out, depth, opts.indent_spaces, "auto& flex = " + std::string(var) + "->flex();");
    emit_line(out, depth, opts.indent_spaces,
              "flex.direction = " + flex_direction_expr(node.layout.direction) + ";");
    emit_line(out, depth, opts.indent_spaces,
              "flex.justify_content = " + flex_justify_expr(node.layout.justify) + ";");
    emit_line(out, depth, opts.indent_spaces,
              "flex.align_items = " + flex_align_expr(node.layout.align) + ";");
    if (node.layout.display && *node.layout.display == "grid") {
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_layout_mode(pulp::view::LayoutMode::grid);");
        emit_line(out, depth, opts.indent_spaces, "{");
        emit_line(out, depth + 1, opts.indent_spaces, "auto& grid = " + std::string(var) + "->grid();");
        if (auto it = node.attributes.find("pulpGridTemplateColumns"); it != node.attributes.end()) {
            emit_line(out, depth + 1, opts.indent_spaces,
                      "grid.template_columns = pulp::view::GridStyle::parse_template(" +
                          cpp_string_literal(it->second) + ");");
        }
        if (auto it = node.attributes.find("pulpGridTemplateRows"); it != node.attributes.end()) {
            emit_line(out, depth + 1, opts.indent_spaces,
                      "grid.template_rows = pulp::view::GridStyle::parse_template(" +
                          cpp_string_literal(it->second) + ");");
        }
        if (node.layout.column_gap || node.layout.gap != 0.0f)
            emit_line(out, depth + 1, opts.indent_spaces,
                      "grid.column_gap = " +
                          format_float(node.layout.column_gap.value_or(node.layout.gap)) + ";");
        if (node.layout.row_gap || node.layout.gap != 0.0f)
            emit_line(out, depth + 1, opts.indent_spaces,
                      "grid.row_gap = " + format_float(node.layout.row_gap.value_or(node.layout.gap)) + ";");
        emit_line(out, depth, opts.indent_spaces, "}");
    }
    if (node.layout.gap != 0.0f)
        emit_line(out, depth, opts.indent_spaces, "flex.gap = " + format_float(node.layout.gap) + ";");
    emit_optional_float(out, depth, opts, "flex", "row_gap", node.layout.row_gap);
    emit_optional_float(out, depth, opts, "flex", "column_gap", node.layout.column_gap);
    if (node.layout.padding_top != 0.0f)
        emit_line(out, depth, opts.indent_spaces, "flex.padding_top = " + format_float(node.layout.padding_top) + ";");
    if (node.layout.padding_right != 0.0f)
        emit_line(out, depth, opts.indent_spaces, "flex.padding_right = " + format_float(node.layout.padding_right) + ";");
    if (node.layout.padding_bottom != 0.0f)
        emit_line(out, depth, opts.indent_spaces, "flex.padding_bottom = " + format_float(node.layout.padding_bottom) + ";");
    if (node.layout.padding_left != 0.0f)
        emit_line(out, depth, opts.indent_spaces, "flex.padding_left = " + format_float(node.layout.padding_left) + ";");
    emit_optional_float(out, depth, opts, "flex", "margin_top", node.layout.margin_top);
    emit_optional_float(out, depth, opts, "flex", "margin_right", node.layout.margin_right);
    emit_optional_float(out, depth, opts, "flex", "margin_bottom", node.layout.margin_bottom);
    emit_optional_float(out, depth, opts, "flex", "margin_left", node.layout.margin_left);
    emit_optional_float(out, depth, opts, "flex", "flex_grow", node.layout.flex_grow);
    emit_optional_float(out, depth, opts, "flex", "flex_shrink", node.layout.flex_shrink);
    if (node.layout.flex_basis) {
        emit_line(out, depth, opts.indent_spaces, "{");
        emit_line(out, depth + 1, opts.indent_spaces,
                  "const auto basis = pulp::view::Dimension::parse(" +
                      cpp_string_literal(*node.layout.flex_basis) + ");");
        emit_line(out, depth + 1, opts.indent_spaces, "flex.dim_flex_basis = basis;");
        emit_line(out, depth + 1, opts.indent_spaces,
                  "if (basis.unit == pulp::view::DimensionUnit::px) flex.flex_basis = basis.value;");
        emit_line(out, depth, opts.indent_spaces, "}");
    }
    if (node.layout.order)
        emit_line(out, depth, opts.indent_spaces, "flex.order = " + std::to_string(*node.layout.order) + ";");
    if (node.layout.wrap)
        emit_line(out, depth, opts.indent_spaces, "flex.flex_wrap = pulp::view::FlexWrap::wrap;");
    if (node.layout.aspect_ratio)
        emit_line(out, depth, opts.indent_spaces, "flex.aspect_ratio = " + float_expr(ctx, *node.layout.aspect_ratio) + ";");
    if (node.layout.align_self) {
        if (auto expr = flex_align_value_expr(*node.layout.align_self))
            emit_line(out, depth, opts.indent_spaces, "flex.align_self = " + *expr + ";");
    }
    if (node.layout.align_content) {
        const auto align = lower_copy(*node.layout.align_content);
        if (align == "space-between") {
            emit_line(out, depth, opts.indent_spaces,
                      "flex.align_content_space = pulp::view::FlexStyle::AlignContentSpace::space_between;");
        } else if (align == "space-around") {
            emit_line(out, depth, opts.indent_spaces,
                      "flex.align_content_space = pulp::view::FlexStyle::AlignContentSpace::space_around;");
        } else if (align == "space-evenly") {
            emit_line(out, depth, opts.indent_spaces,
                      "flex.align_content_space = pulp::view::FlexStyle::AlignContentSpace::space_evenly;");
        } else if (auto expr = flex_align_value_expr(align)) {
            emit_line(out, depth, opts.indent_spaces, "flex.align_content = " + *expr + ";");
        }
    }

    auto emit_dimension = [&](std::string_view preferred,
                              std::string_view dim,
                              const std::optional<float>& value) {
        if (!value)
            return;
        emit_line(out, depth, opts.indent_spaces,
                  "flex." + std::string(preferred) + " = " + float_expr(ctx, *value) + ";");
        emit_line(out, depth, opts.indent_spaces,
                  "flex." + std::string(dim) + " = {" + float_expr(ctx, *value) + ", pulp::view::DimensionUnit::px};");
    };
    emit_dimension("preferred_width", "dim_width", node.style.width);
    emit_dimension("preferred_height", "dim_height", node.style.height);
    emit_dimension("min_width", "dim_min_width", node.style.min_width);
    emit_dimension("min_height", "dim_min_height", node.style.min_height);
    emit_dimension("max_width", "dim_max_width", node.style.max_width);
    emit_dimension("max_height", "dim_max_height", node.style.max_height);

    const bool parent_is_row = parent_direction && *parent_direction == LayoutDirection::row;
    const bool parent_is_column = parent_direction && *parent_direction == LayoutDirection::column;
    const bool has_explicit_align_self = node.layout.align_self.has_value();
    if (node.layout.width_mode == SizingMode::fill && !node.style.width) {
        if (!parent_direction || parent_is_row) {
            emit_line(out, depth, opts.indent_spaces, "flex.flex_grow = std::max(flex.flex_grow, 1.0f);");
        } else if (parent_is_column && !has_explicit_align_self) {
            emit_line(out, depth, opts.indent_spaces, "flex.align_self = pulp::view::FlexAlign::stretch;");
        }
    }
    if (node.layout.height_mode == SizingMode::fill && !node.style.height) {
        if (!parent_direction || parent_is_column) {
            emit_line(out, depth, opts.indent_spaces, "flex.flex_grow = std::max(flex.flex_grow, 1.0f);");
        } else if (parent_is_row && !has_explicit_align_self) {
            emit_line(out, depth, opts.indent_spaces, "flex.align_self = pulp::view::FlexAlign::stretch;");
        }
    }
    if (node.layout.width_mode == SizingMode::hug && !node.style.width)
        emit_line(out, depth, opts.indent_spaces, "flex.dim_width = {0.0f, pulp::view::DimensionUnit::auto_};");
    if (node.layout.height_mode == SizingMode::hug && !node.style.height)
        emit_line(out, depth, opts.indent_spaces, "flex.dim_height = {0.0f, pulp::view::DimensionUnit::auto_};");
}

void emit_visual_style(std::ostringstream& out,
                       int depth,
                       const EmitContext& ctx,
                       std::string_view var,
                       const IRStyle& style) {
    const auto& opts = ctx.opts;
    auto emit_color = [&](std::string_view method, const std::optional<std::string>& value) {
        if (!value)
            return;
        auto expr = color_expr(ctx, *value);
        if (!expr.empty())
            emit_line(out, depth, opts.indent_spaces,
                      std::string(var) + "->" + std::string(method) + "(" + expr + ");");
    };
    emit_color("set_background_color", style.background_color);
    if (style.background_gradient && !style.background_gradient->empty())
        emit_line(out, depth, opts.indent_spaces,
                  "pulp::view::apply_css_background_gradient(*" + std::string(var) + ", " +
                  cpp_string_literal(*style.background_gradient) + ");");
    if (style.background_repeat)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_background_repeat(" + cpp_string_literal(*style.background_repeat) + ");");
    emit_color("set_inheritable_text_color", style.color);
    emit_color("set_border_color", style.border_color);
    emit_color("set_border_top_color", style.border_top_color);
    emit_color("set_border_right_color", style.border_right_color);
    emit_color("set_border_bottom_color", style.border_bottom_color);
    emit_color("set_border_left_color", style.border_left_color);
    if (style.opacity)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_opacity(" + float_expr(ctx, *style.opacity) + ");");
    if (style.border_radius)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_border_radius(" + float_expr(ctx, *style.border_radius) + ");");
    if (style.border_width)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_border_width(" + float_expr(ctx, *style.border_width) + ");");
    if (style.border_top_width)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_border_top_width(" + float_expr(ctx, *style.border_top_width) + ");");
    if (style.border_right_width)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_border_right_width(" + float_expr(ctx, *style.border_right_width) + ");");
    if (style.border_bottom_width)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_border_bottom_width(" + float_expr(ctx, *style.border_bottom_width) + ");");
    if (style.border_left_width)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_border_left_width(" + float_expr(ctx, *style.border_left_width) + ");");
    if (style.border_top_left_radius)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_corner_radius_tl(" + float_expr(ctx, *style.border_top_left_radius) + ");");
    if (style.border_top_right_radius)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_corner_radius_tr(" + float_expr(ctx, *style.border_top_right_radius) + ");");
    if (style.border_bottom_right_radius)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_corner_radius_br(" + float_expr(ctx, *style.border_bottom_right_radius) + ");");
    if (style.border_bottom_left_radius)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_corner_radius_bl(" + float_expr(ctx, *style.border_bottom_left_radius) + ");");
    if (style.font_family)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_inheritable_font_family(" + cpp_string_literal(*style.font_family) + ");");
    if (style.font_size)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_inheritable_font_size(" + float_expr(ctx, *style.font_size) + ");");
    if (style.font_weight)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_inheritable_font_weight(" + std::to_string(*style.font_weight) + ");");
    if (style.letter_spacing)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_inheritable_letter_spacing(" + float_expr(ctx, *style.letter_spacing) + ");");
    if (style.text_align)
        emit_line(out, depth, opts.indent_spaces,
                  std::string(var) + "->set_inheritable_text_align(static_cast<int>(" + label_align_expr(*style.text_align) + "));");
    if (style.overflow) {
        std::string lower;
        for (unsigned char c : *style.overflow)
            lower += static_cast<char>(std::tolower(c));
        if (lower == "hidden")
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_overflow(pulp::view::View::Overflow::hidden);");
        else if (lower == "scroll" || lower == "auto")
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_overflow(pulp::view::View::Overflow::scroll);");
    }
    if (style.position) {
        std::string lower;
        for (unsigned char c : *style.position)
            lower += static_cast<char>(std::tolower(c));
        if (lower == "absolute")
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_position(pulp::view::View::Position::absolute);");
        else if (lower == "relative")
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_position(pulp::view::View::Position::relative);");
        else if (lower == "fixed")
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_position(pulp::view::View::Position::fixed);");
    }
    if (style.top)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_top(" + float_expr(ctx, *style.top) + ");");
    if (style.right)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_right(" + float_expr(ctx, *style.right) + ");");
    if (style.bottom)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_bottom(" + float_expr(ctx, *style.bottom) + ");");
    if (style.left)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_left(" + float_expr(ctx, *style.left) + ");");
    if (style.z_index)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_z_index(" + std::to_string(*style.z_index) + ");");
}

void emit_label_style(std::ostringstream& out,
                      int depth,
                      const EmitContext& ctx,
                      std::string_view var,
                      const IRStyle& style) {
    const auto& opts = ctx.opts;
    if (style.font_family)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_font_family(" + cpp_string_literal(*style.font_family) + ");");
    if (style.font_size)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_font_size(" + float_expr(ctx, *style.font_size) + ");");
    if (style.font_weight)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_font_weight(" + std::to_string(*style.font_weight) + ");");
    if (style.font_style && *style.font_style == "italic")
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_font_style(1);");
    if (style.letter_spacing)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_letter_spacing(" + float_expr(ctx, *style.letter_spacing) + ");");
    if (style.line_height)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_line_height(" + float_expr(ctx, *style.line_height) + ");");
    if (style.text_align)
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_text_align(" + label_align_expr(*style.text_align) + ");");
    if (style.color) {
        auto expr = color_expr(ctx, *style.color);
        if (!expr.empty())
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_text_color(" + expr + ");");
    }
    if (style.text_transform) {
        const auto value = lower_copy(*style.text_transform);
        if (value == "uppercase")
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_text_transform(pulp::view::Label::TextTransform::uppercase);");
        else if (value == "lowercase")
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_text_transform(pulp::view::Label::TextTransform::lowercase);");
        else if (value == "capitalize")
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_text_transform(pulp::view::Label::TextTransform::capitalize);");
    }
    if (style.text_decoration) {
        const auto value = lower_copy(*style.text_decoration);
        if (value.find("underline") != std::string::npos)
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_text_decoration(pulp::view::Label::TextDecoration::underline);");
        else if (value.find("line-through") != std::string::npos)
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_text_decoration(pulp::view::Label::TextDecoration::line_through);");
        else if (value.find("overline") != std::string::npos)
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_text_decoration(pulp::view::Label::TextDecoration::overline);");
    }
    if (style.white_space && *style.white_space != "nowrap")
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_multi_line(true);");
}

void emit_svg_paint(std::ostringstream& out,
                    int depth,
                    const EmitContext& ctx,
                    std::string_view var,
                    const IRNode& node,
                    bool supports_fill) {
    const auto& opts = ctx.opts;
    if (supports_fill) {
        if (auto fill = attr(node, "fill")) {
            if (*fill == "none") {
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->clear_fill();");
            } else if (auto expr = color_expr(ctx, *fill); !expr.empty()) {
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_fill_color(" + expr + ");");
            }
        }
    }
    if (auto stroke = attr(node, "stroke")) {
        if (*stroke == "none") {
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->clear_stroke();");
        } else if (auto expr = color_expr(ctx, *stroke); !expr.empty()) {
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_stroke_color(" + expr + ");");
        }
    }
    if (auto stroke_width = attr_float(node, "stroke-width"))
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_stroke_width(" + float_expr(ctx, *stroke_width) + ");");
}

std::string widget_make_expr(const IRNode& node,
                             const ResolvedNativeNode& resolved,
                             const IRAssetManifest& manifest) {
    const auto text = resolved.text.value_or(node.text_content);
    switch (resolved.kind) {
        case NativeWidgetKind::label:
            return "std::make_unique<pulp::view::Label>(" + cpp_string_literal(text) + ")";
        case NativeWidgetKind::text_button:
            return "std::make_unique<pulp::view::TextButton>(" + cpp_string_literal(text) + ")";
        case NativeWidgetKind::text_editor:
            return "std::make_unique<pulp::view::TextEditor>()";
        case NativeWidgetKind::checkbox:
            return "std::make_unique<pulp::view::Checkbox>()";
        case NativeWidgetKind::toggle_button:
            return "std::make_unique<pulp::view::ToggleButton>()";
        case NativeWidgetKind::knob:
            return "std::make_unique<pulp::view::Knob>()";
        case NativeWidgetKind::fader:
            return "std::make_unique<pulp::view::Fader>()";
        case NativeWidgetKind::meter:
            return "std::make_unique<pulp::view::Meter>()";
        case NativeWidgetKind::xy_pad:
            return "std::make_unique<pulp::view::XYPad>()";
        case NativeWidgetKind::waveform:
            return "std::make_unique<pulp::view::WaveformView>()";
        case NativeWidgetKind::spectrum:
            return "std::make_unique<pulp::view::SpectrumView>()";
        case NativeWidgetKind::image_view:
            return "std::make_unique<pulp::view::ImageView>()";
        case NativeWidgetKind::canvas:
            return "std::make_unique<pulp::view::CanvasWidget>()";
        case NativeWidgetKind::svg_path:
            return "std::make_unique<pulp::view::SvgPathWidget>()";
        case NativeWidgetKind::svg_rect:
            return "std::make_unique<pulp::view::SvgRectWidget>()";
        case NativeWidgetKind::svg_line:
            return "std::make_unique<pulp::view::SvgLineWidget>()";
        case NativeWidgetKind::view:
            return "std::make_unique<pulp::view::View>()";
    }
    (void)manifest;
    return "std::make_unique<pulp::view::View>()";
}

void emit_widget_specific(std::ostringstream& out,
                          int depth,
                          const EmitContext& ctx,
                          std::string_view var,
                          const IRNode& node,
                          const ResolvedNativeNode& resolved,
                          const IRAssetManifest& manifest) {
    const auto& opts = ctx.opts;
    const auto semantics = imported_widget_semantics(node, resolved);
    const auto& text = semantics.text;
    switch (resolved.kind) {
        case NativeWidgetKind::label:
            emit_label_style(out, depth, ctx, var, node.style);
            break;
        case NativeWidgetKind::text_editor:
            if (semantics.text_placeholder)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->placeholder = " + cpp_string_literal(*semantics.text_placeholder) + ";");
            if (semantics.text_value)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_text(" + cpp_string_literal(*semantics.text_value) + ");");
            break;
        case NativeWidgetKind::checkbox:
            if (semantics.checked)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_checked(true);");
            break;
        case NativeWidgetKind::toggle_button:
            if (!text.empty())
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_label(" + cpp_string_literal(text) + ");");
            if (semantics.toggle_on)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_on(true);");
            if (semantics.toggle_on_background_color) {
                if (auto expr = color_expr(ctx, *semantics.toggle_on_background_color); !expr.empty())
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_on_background_color(" + expr + ");");
            }
            if (semantics.toggle_off_background_color) {
                if (auto expr = color_expr(ctx, *semantics.toggle_off_background_color); !expr.empty())
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_off_background_color(" + expr + ");");
            }
            if (semantics.toggle_on_text_color) {
                if (auto expr = color_expr(ctx, *semantics.toggle_on_text_color); !expr.empty())
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_on_text_color(" + expr + ");");
            }
            if (semantics.toggle_off_text_color) {
                if (auto expr = color_expr(ctx, *semantics.toggle_off_text_color); !expr.empty())
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_off_text_color(" + expr + ");");
            }
            if (semantics.toggle_on_border_color) {
                if (auto expr = color_expr(ctx, *semantics.toggle_on_border_color); !expr.empty())
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_on_border_color(" + expr + ");");
            }
            if (semantics.toggle_off_border_color) {
                if (auto expr = color_expr(ctx, *semantics.toggle_off_border_color); !expr.empty())
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_off_border_color(" + expr + ");");
            }
            if (semantics.toggle_corner_radius)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_corner_radius(" + float_expr(ctx, *semantics.toggle_corner_radius) + ");");
            if (semantics.toggle_font_size)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_font_size(" + float_expr(ctx, *semantics.toggle_font_size) + ");");
            break;
        case NativeWidgetKind::knob: {
            if (!text.empty())
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_label(" + cpp_string_literal(text) + ");");
            const auto value = float_expr(ctx, semantics.normalized_value);
            const auto default_value = float_expr(ctx, semantics.normalized_default);
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_value(/* imported static param value */ " + value + ");");
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_default_value(" + default_value + ");");
            if (semantics.widget_schema)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_widget_schema(" + cpp_string_literal(*semantics.widget_schema) + ");");
            if (!semantics.show_internal_label)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_show_label(false);");
            break;
        }
        case NativeWidgetKind::fader: {
            if (!text.empty())
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_label(" + cpp_string_literal(text) + ");");
            emit_line(out, depth, opts.indent_spaces,
                      std::string(var) + "->set_value(/* imported static param value */ " +
                          float_expr(ctx, semantics.normalized_value) + ");");
            if (semantics.horizontal)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_orientation(pulp::view::Fader::Orientation::horizontal);");
            if (semantics.widget_schema)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_widget_schema(" + cpp_string_literal(*semantics.widget_schema) + ");");
            if (semantics.fader_thumb_shape) {
                if (*semantics.fader_thumb_shape == ImportedFaderThumbShape::rectangle)
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_thumb_shape(pulp::view::Fader::ThumbShape::rectangle);");
                else if (*semantics.fader_thumb_shape == ImportedFaderThumbShape::circle)
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_thumb_shape(pulp::view::Fader::ThumbShape::circle);");
            }
            if (semantics.fader_thumb_width || semantics.fader_thumb_height) {
                emit_line(out, depth, opts.indent_spaces,
                          std::string(var) + "->set_thumb_size(" +
                              float_expr(ctx, semantics.fader_thumb_width.value_or(0.0f)) + ", " +
                              float_expr(ctx, semantics.fader_thumb_height.value_or(0.0f)) + ");");
            }
            if (semantics.fader_thumb_corner_radius) {
                emit_line(out, depth, opts.indent_spaces,
                          std::string(var) + "->set_thumb_corner_radius(" + float_expr(ctx, *semantics.fader_thumb_corner_radius) + ");");
            }
            break;
        }
        case NativeWidgetKind::meter: {
            const auto value = float_expr(ctx, semantics.normalized_value);
            const auto peak = float_expr(ctx, semantics.peak_value);
            emit_line(out, depth, opts.indent_spaces,
                      std::string(var) + "->set_level(/* imported static meter level */ " + value + ", " + peak + ");");
            if (semantics.horizontal)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_orientation(pulp::view::Meter::Orientation::horizontal);");
            break;
        }
        case NativeWidgetKind::xy_pad:
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_x(" + float_expr(ctx, semantics.x_value) + ");");
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_y(" + float_expr(ctx, semantics.y_value) + ");");
            if (semantics.x_label) emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_x_label(" + cpp_string_literal(*semantics.x_label) + ");");
            if (semantics.y_label) emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_y_label(" + cpp_string_literal(*semantics.y_label) + ");");
            break;
        case NativeWidgetKind::waveform:
            if (semantics.waveform_shape)
                emit_line(out, depth, opts.indent_spaces,
                          std::string(var) + "->set_preview_shape(" + cpp_string_literal(*semantics.waveform_shape) + ");");
            break;
        case NativeWidgetKind::image_view:
            if (auto asset_id = first_asset_id(node)) {
                const auto uri = asset_uri(manifest, *asset_id);
                if (!uri.empty())
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_image_source(" + cpp_string_literal(uri) + ");");
            }
            if (auto sizing = imported_image_sizing_override(node)) {
                const auto flex_var = std::string(var) + "_image_flex";
                emit_line(out, depth, opts.indent_spaces,
                          "auto& " + flex_var + " = " + std::string(var) + "->flex();");
                emit_line(out, depth, opts.indent_spaces,
                          flex_var + ".preferred_width = " + float_expr(ctx, sizing->width) + ";");
                emit_line(out, depth, opts.indent_spaces,
                          flex_var + ".preferred_height = " + float_expr(ctx, sizing->height) + ";");
                emit_line(out, depth, opts.indent_spaces,
                          flex_var + ".dim_width = {" + float_expr(ctx, sizing->width) + ", pulp::view::DimensionUnit::px};");
                emit_line(out, depth, opts.indent_spaces,
                          flex_var + ".dim_height = {" + float_expr(ctx, sizing->height) + ", pulp::view::DimensionUnit::px};");
                if (sizing->left)
                    emit_line(out, depth, opts.indent_spaces,
                              std::string(var) + "->set_left(" + float_expr(ctx, *sizing->left) + ");");
                if (sizing->top)
                    emit_line(out, depth, opts.indent_spaces,
                              std::string(var) + "->set_top(" + float_expr(ctx, *sizing->top) + ");");
            }
            break;
        case NativeWidgetKind::svg_path:
            if (auto path_data = attr(node, "d"))
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_path(" + cpp_string_literal(*path_data) + ");");
            if (auto viewbox = attr(node, "viewBox")) {
                std::istringstream input(*viewbox);
                float min_x = 0.0f, min_y = 0.0f, width = 0.0f, height = 0.0f;
                if (input >> min_x >> min_y >> width >> height)
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_viewbox(" + float_expr(ctx, width) + ", " + float_expr(ctx, height) + ");");
            } else if (node.style.width && node.style.height) {
                emit_line(out, depth, opts.indent_spaces,
                          std::string(var) + "->set_viewbox(" + float_expr(ctx, *node.style.width) + ", " + float_expr(ctx, *node.style.height) + ");");
            }
            emit_svg_paint(out, depth, ctx, var, node, true);
            break;
        case NativeWidgetKind::svg_rect:
            emit_line(out, depth, opts.indent_spaces,
                      std::string(var) + "->set_rect(" +
                      float_expr(ctx, attr_float(node, "x").value_or(0.0f)) + ", " +
                      float_expr(ctx, attr_float(node, "y").value_or(0.0f)) + ", " +
                      float_expr(ctx, attr_float(node, "width").value_or(node.style.width.value_or(0.0f))) + ", " +
                      float_expr(ctx, attr_float(node, "height").value_or(node.style.height.value_or(0.0f))) + ");");
            emit_svg_paint(out, depth, ctx, var, node, true);
            break;
        case NativeWidgetKind::svg_line:
            emit_line(out, depth, opts.indent_spaces,
                      std::string(var) + "->set_line(" +
                      float_expr(ctx, attr_float(node, "x1").value_or(0.0f)) + ", " +
                      float_expr(ctx, attr_float(node, "y1").value_or(0.0f)) + ", " +
                      float_expr(ctx, attr_float(node, "x2").value_or(0.0f)) + ", " +
                      float_expr(ctx, attr_float(node, "y2").value_or(0.0f)) + ");");
            emit_svg_paint(out, depth, ctx, var, node, false);
            break;
        default:
            break;
    }
}

// IR interactive-element kind → the generated DesignFrameElement::Kind token.
// Mirrors to_frame_elements() in design_import_native_common.cpp so the codegen
// and the runtime materializer lower the same overlay set.
const char* frame_element_kind_token(InteractiveElementKind kind) {
    switch (kind) {
        case InteractiveElementKind::knob:        return "knob";
        case InteractiveElementKind::fader:       return "fader";
        case InteractiveElementKind::toggle:      return "toggle";
        case InteractiveElementKind::dropdown:    return "dropdown";
        case InteractiveElementKind::text_field:  return "text_field";
        case InteractiveElementKind::tab_group:   return "tab_group";
        case InteractiveElementKind::stepper:     return "stepper";
        case InteractiveElementKind::swap:        return "swap";
        case InteractiveElementKind::action:      return "action";
        case InteractiveElementKind::xy_pad:      return "xy_pad";
        case InteractiveElementKind::value_label: return "value_label";
        case InteractiveElementKind::custom:      return "custom";
    }
    return "knob";
}

// Emit a faithful_svg node as a DesignFrameView that renders the node's own
// Figma SVG export 1:1 (SkSVGDOM) with the typed interactive overlays composited
// on top — the same construction make_faithful_svg_frame() builds at runtime,
// but lowered to static C++. The SVG is embedded as chunked base64 (no >64K
// string literal) and decoded once at construction, matching the catalog
// generator (tools/import-design/make_catalog_component.py).
//
// Returns true when it emitted the faithful construction (assigning `var`).
// Returns false when the node is not faithful, has no svg_asset_id, or the asset
// can't be resolved at codegen time — the caller then falls back to the normal
// native-widget emit so the output always compiles and renders something.
bool emit_faithful_frame(std::ostringstream& out,
                         int depth,
                         EmitContext& ctx,
                         const std::string& var,
                         const IRNode& node) {
    if (node.render_mode != NodeRenderMode::faithful_svg || !node.svg_asset_id)
        return false;
    const IRAssetRef* asset = ctx.manifest.resolve(*node.svg_asset_id);
    const std::string svg = asset ? resolve_svg_document(*asset) : std::string{};
    if (svg.empty()) {
        if (ctx.opts.include_comments)
            emit_line(out, depth, ctx.opts.indent_spaces,
                      "// faithful_svg asset '" + *node.svg_asset_id +
                      "' unresolved at codegen time — falling back to native widgets");
        return false;
    }

    if (ctx.opts.include_comments)
        emit_line(out, depth, ctx.opts.indent_spaces,
                  "// faithful_svg: render this node's own Figma SVG 1:1 via DesignFrameView");

    // Embedded SVG: chunked base64, joined + decoded once.
    const std::string b64 = runtime::base64_encode(svg);
    emit_line(out, depth, ctx.opts.indent_spaces, "std::string " + var + "_svg;");
    emit_line(out, depth, ctx.opts.indent_spaces, "{");
    emit_line(out, depth + 1, ctx.opts.indent_spaces,
              "static const char* const kParts[] = {");
    constexpr std::size_t kChunk = 8000;
    for (std::size_t i = 0; i < b64.size(); i += kChunk) {
        const std::string piece = b64.substr(i, kChunk);
        const bool last = (i + kChunk >= b64.size());
        emit_line(out, depth + 2, ctx.opts.indent_spaces,
                  cpp_string_literal(piece) + (last ? "" : ","));
    }
    emit_line(out, depth + 1, ctx.opts.indent_spaces, "};");
    emit_line(out, depth + 1, ctx.opts.indent_spaces, "std::string b64;");
    emit_line(out, depth + 1, ctx.opts.indent_spaces,
              "for (const char* p : kParts) b64 += p;");
    emit_line(out, depth + 1, ctx.opts.indent_spaces,
              "if (auto bytes = pulp::runtime::base64_decode(b64))");
    emit_line(out, depth + 2, ctx.opts.indent_spaces,
              var + "_svg.assign(bytes->begin(), bytes->end());");
    emit_line(out, depth, ctx.opts.indent_spaces, "}");

    // Interactive overlays (knob / dropdown / text_field / tab_group / stepper).
    emit_line(out, depth, ctx.opts.indent_spaces,
              "std::vector<pulp::view::DesignFrameElement> " + var + "_els;");
    for (const auto& e : node.interactive_elements) {
        emit_line(out, depth, ctx.opts.indent_spaces, "{");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  "pulp::view::DesignFrameElement el;");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  std::string("el.kind = pulp::view::DesignFrameElement::Kind::") +
                  frame_element_kind_token(e.kind) + ";");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  "el.cx = " + format_float(e.cx) + "; el.cy = " + format_float(e.cy) +
                  "; el.hit_radius = " + format_float(e.hit_radius) + ";");
        if (!e.svg_patch_d.empty())
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.needle_d = " + cpp_string_literal(e.svg_patch_d) + ";");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  "el.value = " + format_float(e.default_value) + ";");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  "el.x = " + format_float(e.x) + "; el.y = " + format_float(e.y) +
                  "; el.w = " + format_float(e.w) + "; el.h = " + format_float(e.h) + ";");
        if (!e.placeholder.empty())
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.placeholder = " + cpp_string_literal(e.placeholder) + ";");
        if (!e.options.empty()) {
            std::string opts = "el.options = {";
            for (std::size_t i = 0; i < e.options.size(); ++i)
                opts += (i ? ", " : " ") + cpp_string_literal(e.options[i]);
            opts += " };";
            emit_line(out, depth + 1, ctx.opts.indent_spaces, opts);
        }
        if (e.selected_index != 0)
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.selected_index = " + std::to_string(e.selected_index) + ";");
        if (!e.bg_color.empty())
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.bg_color = " + cpp_string_literal(e.bg_color) + ";");
        if (e.flash)
            emit_line(out, depth + 1, ctx.opts.indent_spaces, "el.flash = true;");
        // swap / action / xy_pad / value_label fields.
        if (e.target_frame != -1)
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.target_frame = " + std::to_string(e.target_frame) + ";");
        if (!e.action.empty())
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.action = " + cpp_string_literal(e.action) + ";");
        if (!e.text.empty())
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.text = " + cpp_string_literal(e.text) + ";");
        if (e.value_left_align)
            emit_line(out, depth + 1, ctx.opts.indent_spaces, "el.value_left_align = true;");
        if (e.kind == InteractiveElementKind::xy_pad)
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.value_y = " + format_float(e.default_value_y) + ";");
        if (!e.factory_id.empty())
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.factory_id = " + cpp_string_literal(e.factory_id) + ";");
        if (!e.custom_props.empty())
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.custom_props = " + cpp_string_literal(e.custom_props) + ";");
        // Host-param binding key for a geometry-detected control.
        if (!e.param_key.empty())
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.param_key = " + cpp_string_literal(e.param_key) + ";");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  var + "_els.push_back(std::move(el));");
        emit_line(out, depth, ctx.opts.indent_spaces, "}");
    }

    emit_line(out, depth, ctx.opts.indent_spaces,
              "auto " + var + " = std::make_unique<pulp::view::DesignFrameView>(std::move(" +
              var + "_svg), std::move(" + var + "_els));");
    // If any control carries a binding key, self-wire gestures to the host-param
    // surface — parity with the runtime materialize path (make_faithful_svg_frame).
    const bool any_bound = std::any_of(
        node.interactive_elements.begin(), node.interactive_elements.end(),
        [](const IRInteractiveElement& e) { return !e.param_key.empty(); });
    if (any_bound)
        emit_line(out, depth, ctx.opts.indent_spaces,
                  var + "->route_changes_to_host_params(true);");
    return true;
}

void emit_node(std::ostringstream& out,
               EmitContext& ctx,
               const IRNode& node,
               const ResolvedNativeNode& resolved,
               std::string_view parent_var,
               int depth,
               std::optional<LayoutDirection> parent_direction,
               int& counter,
               PromotedChildHitPolicy self_hit_policy = PromotedChildHitPolicy::unchanged) {
    emit_line(out, depth, ctx.opts.indent_spaces, "{");
    ++depth;

    const std::string var = "node_" + std::to_string(counter++);
    if (ctx.opts.include_comments) {
        if (node.stable_anchor_id && !node.stable_anchor_id->empty()) {
            std::string comment = "// anchor: " + *node.stable_anchor_id;
            if (!node.name.empty()) comment += " - " + node.name;
            emit_line(out, depth, ctx.opts.indent_spaces, comment);
        } else if (!node.name.empty()) {
            emit_line(out, depth, ctx.opts.indent_spaces, "// " + node.name);
        }
    }
    // A faithful_svg node renders its own SVG via DesignFrameView; the SVG is the
    // whole subtree, so we skip the native widget body and child recursion below.
    const bool faithful = emit_faithful_frame(out, depth, ctx, var, node);
    if (!faithful)
        emit_line(out, depth, ctx.opts.indent_spaces,
                  "auto " + var + " = " + widget_make_expr(node, resolved, ctx.manifest) + ";");
    emit_line(out, depth, ctx.opts.indent_spaces,
              var + "->set_id(" + cpp_string_literal(resolved.id) + ");");
    if (node.stable_anchor_id && !node.stable_anchor_id->empty())
        emit_line(out, depth, ctx.opts.indent_spaces,
                  var + "->set_anchor_id(" + cpp_string_literal(*node.stable_anchor_id) + ");");
    const bool explicit_hit_test_disabled =
        attr(node, "pulpHitTestable") && !attr_bool(node, "pulpHitTestable");
    if (self_hit_policy == PromotedChildHitPolicy::disabled || explicit_hit_test_disabled)
        emit_line(out, depth, ctx.opts.indent_spaces, var + "->set_hit_testable(false);");
    if (self_hit_policy == PromotedChildHitPolicy::pass_through_self)
        emit_line(out, depth, ctx.opts.indent_spaces,
                  var + "->set_pointer_events(pulp::view::View::PointerEvents::box_none);");
    if (auto label = resolved.text; label && !label->empty())
        emit_line(out, depth, ctx.opts.indent_spaces,
                  var + "->set_access_label(" + cpp_string_literal(*label) + ");");

    emit_common_layout(out, depth, ctx, var, node, parent_direction);
    // Faithful frames carry their own visuals (the SVG) and interaction (the
    // overlays), so the native style/widget/child emit is skipped entirely.
    if (!faithful) {
        emit_visual_style(out, depth, ctx, var, node.style);
        emit_widget_specific(out, depth, ctx, var, node, resolved, ctx.manifest);
    }

    const auto count = faithful ? std::size_t{0}
                                : std::min(node.children.size(), resolved.children.size());
    const bool parent_owns_imported_child_hits =
        native_kind_owns_imported_child_hits(resolved.kind);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& child = node.children[i];
        const auto child_hit_policy = parent_owns_imported_child_hits
            ? promoted_widget_child_hit_policy(child, resolved.children[i])
            : PromotedChildHitPolicy::unchanged;
        auto found = ctx.extracted.find(&child);
        if (found != ctx.extracted.end()) {
            if (child_hit_policy != PromotedChildHitPolicy::unchanged) {
                const std::string child_var = "child_" + std::to_string(counter++);
                emit_line(out, depth, ctx.opts.indent_spaces, "{");
                emit_line(out, depth + 1, ctx.opts.indent_spaces,
                          "auto " + child_var + " = " + found->second + "();");
                if (child_hit_policy == PromotedChildHitPolicy::disabled) {
                    emit_line(out, depth + 1, ctx.opts.indent_spaces,
                              child_var + "->set_hit_testable(false);");
                } else if (child_hit_policy == PromotedChildHitPolicy::pass_through_self) {
                    emit_line(out, depth + 1, ctx.opts.indent_spaces,
                              child_var + "->set_pointer_events(pulp::view::View::PointerEvents::box_none);");
                }
                emit_line(out, depth + 1, ctx.opts.indent_spaces,
                          var + "->add_child(std::move(" + child_var + "));");
                emit_line(out, depth, ctx.opts.indent_spaces, "}");
            } else {
                emit_line(out, depth, ctx.opts.indent_spaces,
                          var + "->add_child(" + found->second + "());");
            }
        } else {
            emit_node(out,
                      ctx,
                      child,
                      resolved.children[i],
                      var,
                      depth,
                      node.layout.direction,
                      counter,
                      child_hit_policy);
        }
    }

    if (!parent_var.empty()) {
        emit_line(out, depth, ctx.opts.indent_spaces,
                  std::string(parent_var) + "->add_child(std::move(" + var + "));");
    } else {
        emit_line(out, depth, ctx.opts.indent_spaces, "return " + var + ";");
    }

    --depth;
    emit_line(out, depth, ctx.opts.indent_spaces, "}");
}

void emit_function(std::ostringstream& out,
                   EmitContext& ctx,
                   std::string_view function_name,
                   const IRNode& node,
                   const ResolvedNativeNode& resolved,
                   std::optional<LayoutDirection> parent_direction,
                   std::string_view comment = {}) {
    if (ctx.opts.include_comments && !comment.empty())
        out << "// " << comment << "\n";
    out << "std::unique_ptr<pulp::view::View> " << function_name << "() {\n";
    int counter = 0;
    emit_node(out, ctx, node, resolved, "", 1, parent_direction, counter);
    out << "}\n\n";
}

void emit_namespace_open(std::ostringstream& out, std::string_view ns) {
    if (!ns.empty())
        out << "namespace " << ns << " {\n\n";
}

void emit_namespace_close(std::ostringstream& out, std::string_view ns) {
    if (!ns.empty())
        out << "}  // namespace " << ns << "\n";
}

std::string trim_trailing_blank_lines(std::string text) {
    while (text.size() >= 2 &&
           text[text.size() - 1] == '\n' &&
           text[text.size() - 2] == '\n') {
        text.pop_back();
    }
    return text;
}

std::string token_basename(std::string_view symbol) {
    constexpr std::string_view kPrefix = "tokens::";
    if (symbol.rfind(kPrefix, 0) == 0)
        return std::string(symbol.substr(kPrefix.size()));
    return std::string(symbol);
}

void emit_tokens(std::ostringstream& out, const DesignIR& ir, const EmitContext& ctx) {
    const auto& opts = ctx.opts;
    if (!opts.emit_named_tokens)
        return;
    if (ir.tokens.colors.empty() && ir.tokens.dimensions.empty() && ir.tokens.strings.empty())
        return;
    out << "namespace tokens {\n";
    for (const auto& [name, value] : ir.tokens.colors) {
        auto expr = color_literal_expr(value);
        const auto found = ctx.tokens.color_by_name.find(name);
        out << "inline constexpr auto "
            << (found == ctx.tokens.color_by_name.end()
                    ? pascal_identifier(name, "Color")
                    : token_basename(found->second))
            << " = " << (expr.empty() ? cpp_string_literal(value) : expr) << ";\n";
    }
    for (const auto& [name, value] : ir.tokens.dimensions) {
        const auto found = ctx.tokens.dimension_by_name.find(name);
        out << "inline constexpr float "
            << (found == ctx.tokens.dimension_by_name.end()
                    ? pascal_identifier(name, "Dim")
                    : token_basename(found->second))
            << " = " << format_float(value) << ";\n";
    }
    for (const auto& [name, value] : ir.tokens.strings) {
        auto found = ctx.tokens.string_by_name.find(name);
        out << "inline constexpr const char* "
            << (found == ctx.tokens.string_by_name.end()
                    ? pascal_identifier(name, "String")
                    : token_basename(found->second))
            << " = " << cpp_string_literal(value) << ";\n";
    }
    out << "}  // namespace tokens\n\n";
}

void emit_asset_constants(std::ostringstream& out, const IRAssetManifest& manifest, const EmitContext& ctx) {
    const auto& opts = ctx.opts;
    if (!opts.emit_asset_constants || manifest.assets.empty())
        return;
    out << "namespace assets {\n";
    for (const auto& asset : manifest.assets) {
        if (asset.asset_id.empty())
            continue;
        out << "inline constexpr const char* "
            << ctx.assets.id_by_asset_id.at(asset.asset_id).substr(std::string_view("assets::").size())
            << " = " << cpp_string_literal(asset.asset_id) << ";\n";
    }
    out << "}  // namespace assets\n\n";
}

void emit_manifest(std::ostringstream& out, const IRAssetManifest& manifest, const EmitContext& ctx) {
    const auto& opts = ctx.opts;
    out << "pulp::view::IRAssetManifest bake_asset_manifest() {\n";
    emit_line(out, 1, opts.indent_spaces, "pulp::view::IRAssetManifest manifest;");
    emit_line(out, 1, opts.indent_spaces, "manifest.version = " + std::to_string(manifest.version) + ";");
    for (const auto& asset : manifest.assets) {
        emit_line(out, 1, opts.indent_spaces, "{");
        emit_line(out, 2, opts.indent_spaces, "pulp::view::IRAssetRef asset;");
        emit_line(out, 2, opts.indent_spaces, "asset.asset_id = " + asset_id_expr(ctx, asset.asset_id) + ";");
        emit_line(out, 2, opts.indent_spaces, "asset.original_uri = " + cpp_string_literal(asset.original_uri) + ";");
        for (const auto& alias : asset.original_uri_aliases)
            emit_line(out, 2, opts.indent_spaces, "asset.original_uri_aliases.push_back(" + cpp_string_literal(alias) + ");");
        if (asset.local_path)
            emit_line(out, 2, opts.indent_spaces, "asset.local_path = " + cpp_string_literal(*asset.local_path) + ";");
        emit_line(out, 2, opts.indent_spaces, "asset.content_hash = " + cpp_string_literal(asset.content_hash) + ";");
        emit_line(out, 2, opts.indent_spaces, "asset.mime = " + cpp_string_literal(asset.mime) + ";");
        if (asset.width)
            emit_line(out, 2, opts.indent_spaces, "asset.width = " + std::to_string(*asset.width) + ";");
        if (asset.height)
            emit_line(out, 2, opts.indent_spaces, "asset.height = " + std::to_string(*asset.height) + ";");
        if (asset.font_family)
            emit_line(out, 2, opts.indent_spaces, "asset.font_family = " + cpp_string_literal(*asset.font_family) + ";");
        if (asset.license)
            emit_line(out, 2, opts.indent_spaces, "asset.license = " + cpp_string_literal(*asset.license) + ";");
        if (asset.source_url)
            emit_line(out, 2, opts.indent_spaces, "asset.source_url = " + cpp_string_literal(*asset.source_url) + ";");
        emit_line(out, 2, opts.indent_spaces, "manifest.assets.push_back(std::move(asset));");
        emit_line(out, 1, opts.indent_spaces, "}");
    }
    emit_line(out, 1, opts.indent_spaces, "return manifest;");
    out << "}\n\n";
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

// Returns true when `node` carries any of the `pulp*` binding-contract
// attributes that gate a binding-manifest entry. This is the manifest
// eligibility gate (distinct from the helper eligibility gate below).
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

// One emitted binding-context helper route. The helper-emission pass consumes
// these; they are the value-or-empty-string projection of a metadata node that
// passed the (helper-specific) eligibility gate.
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
};

// One node's resolved binding state, parsed exactly once during the single
// tree traversal. Both the binding-manifest JSON and the binding-context
// helper C++ are rendered from the plan's routes.
//
// The two eligibility flags are DELIBERATELY DISTINCT — the manifest gate
// (`eligible_for_manifest`) and the helper gate (`eligible_for_helper`) are
// different predicates, and not every manifest entry produces a helper route.
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

// Single DFS over the (IRNode, ResolvedNativeNode) tree. Parses
// NativeBindingMetadata once per node and records both eligibility verdicts.
// Visits children in the SAME order as the old two separate traversals
// (both walked min(node.children, resolved.children) in index order), so the
// resulting manifest-entry and helper-emission orders are unchanged.
void collect_resolved_binding_plan(ResolvedBindingPlan& plan,
                                   const IRNode& node,
                                   const ResolvedNativeNode& resolved,
                                   std::string_view ir_path) {
    ResolvedBindingRoute route;
    route.ir_path = std::string(ir_path);
    route.ir_node = &node;
    route.resolved = &resolved;
    route.metadata = NativeBindingMetadata::parse(node);

    // Manifest gate: identical to the old has_binding_manifest_metadata().
    route.eligible_for_manifest = node_has_binding_manifest_metadata(node);

    // Helper gate: identical to the old collect_binding_helper_routes() gate.
    const auto& md = route.metadata;
    const bool has_single_param = md.param_key && !md.param_key->empty();
    const bool has_scalar_param_control =
        (resolved.kind == NativeWidgetKind::knob ||
         resolved.kind == NativeWidgetKind::fader ||
         resolved.kind == NativeWidgetKind::checkbox ||
         resolved.kind == NativeWidgetKind::toggle_button) &&
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

// Render one binding-manifest JSON entry. Field order / names / presence are
// identical to the old collect_binding_manifest_entries() body.
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

// Render the binding-manifest JSON from the plan, in DFS order, filtered by
// the manifest eligibility flag. Byte-identical to the old
// build_binding_manifest_json().
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

// Project the plan's helper-eligible routes into BindingHelperRoute, in DFS
// order. value_or(empty) defaulting is identical to the old
// collect_binding_helper_routes().
std::vector<BindingHelperRoute> build_binding_helper_routes(const ResolvedBindingPlan& plan) {
    std::vector<BindingHelperRoute> routes;
    for (const auto& route : plan.routes) {
        if (!route.eligible_for_helper)
            continue;
        const NativeBindingMetadata& md = route.metadata;
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

CppExportResult generate_pulp_cpp(const DesignIR& ir,
                                  const IRAssetManifest& manifest,
                                  const CppExportOptions& input_opts) {
    CppExportOptions opts = input_opts;
    if (opts.function_name.empty())
        opts.function_name = "build_imported_ui";
    if (opts.header_filename.empty())
        opts.header_filename = "imported_ui.hpp";
    if (opts.indent_spaces <= 0)
        opts.indent_spaces = 4;

    const IRAssetManifest& effective_manifest = manifest.assets.empty()
        ? ir.asset_manifest
        : manifest;
    auto resolved = resolve_design_ir_native(ir, effective_manifest);

    // Single tree traversal: parse NativeBindingMetadata once per node and
    // record both eligibility verdicts. The binding-manifest JSON and the
    // binding-context helper C++ are both rendered from this one plan.
    ResolvedBindingPlan binding_plan;
    collect_resolved_binding_plan(binding_plan, ir.root, resolved, "root");

    std::vector<BindingHelperRoute> binding_helper_routes;
    if (opts.emit_binding_context_helpers)
        binding_helper_routes = build_binding_helper_routes(binding_plan);
    const bool emit_binding_helpers = !binding_helper_routes.empty();

    EmitContext ctx{
        .opts = opts,
        .manifest = effective_manifest,
    };
    ctx.used_functions.insert(opts.function_name);
    ctx.tokens = build_token_symbols(ir, ctx);
    ctx.assets = build_asset_symbols(effective_manifest, ctx);
    collect_components(ir.root, resolved, ctx, std::nullopt, true);

    CppExportResult result;
    {
        std::ostringstream header;
        header << "#pragma once\n\n"
               << "#include <memory>\n"
               << "#include <pulp/view/design_import.hpp>\n"
               << "#include <pulp/view/view.hpp>\n\n";
        emit_namespace_open(header, opts.namespace_name);
        header << "std::unique_ptr<pulp::view::View> " << opts.function_name << "();\n"
               << "pulp::view::IRAssetManifest bake_asset_manifest();\n";
        if (emit_binding_helpers) {
            header << "void " << opts.binding_function_name
                   << "(pulp::view::View& root, pulp::view::NativeImportBindingContext& ctx);\n";
        }
        emit_namespace_close(header, opts.namespace_name);
        result.header = header.str();
    }

    std::ostringstream source;
    if (opts.include_comments)
        source << "// Generated by Pulp import-design baked C++ exporter.\n";
    source << "#include " << cpp_string_literal(opts.header_filename) << "\n\n"
           << "#include <algorithm>\n"
           << "#include <memory>\n"
           << "#include <string>\n"
           << "#include <string_view>\n"
           << "#include <utility>\n"
           << "#include <vector>\n"
           << "#include <pulp/view/buttons.hpp>\n"
           << "#include <pulp/view/canvas_widget.hpp>\n"
           << "#include <pulp/view/css_gradient.hpp>\n"
           << "#include <pulp/view/design_frame_view.hpp>\n"
           << "#include <pulp/view/svg_path_widget.hpp>\n"
           << "#include <pulp/view/text_editor.hpp>\n"
           << "#include <pulp/view/widgets.hpp>\n"
           << "#include <pulp/view/widgets/svg_line.hpp>\n"
           << "#include <pulp/view/widgets/svg_rect.hpp>\n"
           << "#include <pulp/runtime/base64.hpp>\n\n";

    emit_namespace_open(source, opts.namespace_name);
    emit_tokens(source, ir, ctx);
    emit_asset_constants(source, effective_manifest, ctx);
    emit_manifest(source, effective_manifest, ctx);

    for (const auto& component : ctx.components)
        source << "std::unique_ptr<pulp::view::View> " << component.function_name << "();\n";
    if (!ctx.components.empty())
        source << "\n";

    for (const auto& component : ctx.components)
        emit_function(source,
                      ctx,
                      component.function_name,
                      *component.node,
                      *component.resolved,
                      component.parent_direction,
                      component.rule_comment);

    emit_function(source, ctx, opts.function_name, ir.root, resolved, std::nullopt);
    if (emit_binding_helpers)
        emit_binding_context_helpers(source, opts, binding_helper_routes);
    emit_namespace_close(source, opts.namespace_name);
    result.source = trim_trailing_blank_lines(source.str());
    result.binding_manifest = build_binding_manifest_json(binding_plan);
    return result;
}

}  // namespace pulp::view
