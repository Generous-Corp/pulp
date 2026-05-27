#include <pulp/view/buttons.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/svg_path_widget.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/widgets/svg_line.hpp>
#include <pulp/view/widgets/svg_rect.hpp>

#include "design_import_native_common.hpp"

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

std::string lower_copy(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value)
        out += static_cast<char>(std::tolower(c));
    return out;
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

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::optional<std::array<unsigned, 4>> parse_hex_color(std::string_view value) {
    if (value.empty() || value.front() != '#')
        return std::nullopt;
    auto nibble = [](int v) -> unsigned { return static_cast<unsigned>((v << 4) | v); };
    if (value.size() == 4 || value.size() == 5) {
        const int r = hex_digit(value[1]);
        const int g = hex_digit(value[2]);
        const int b = hex_digit(value[3]);
        const int a = value.size() == 5 ? hex_digit(value[4]) : 15;
        if (r < 0 || g < 0 || b < 0 || a < 0)
            return std::nullopt;
        return std::array<unsigned, 4>{nibble(r), nibble(g), nibble(b), nibble(a)};
    }
    if (value.size() == 7 || value.size() == 9) {
        auto pair = [&](std::size_t offset) -> std::optional<unsigned> {
            const int hi = hex_digit(value[offset]);
            const int lo = hex_digit(value[offset + 1]);
            if (hi < 0 || lo < 0)
                return std::nullopt;
            return static_cast<unsigned>((hi << 4) | lo);
        };
        auto r = pair(1);
        auto g = pair(3);
        auto b = pair(5);
        auto a = value.size() == 9 ? pair(7) : std::optional<unsigned>(255);
        if (!r || !g || !b || !a)
            return std::nullopt;
        return std::array<unsigned, 4>{*r, *g, *b, *a};
    }
    return std::nullopt;
}

std::string color_literal_expr(std::string_view value) {
    if (auto color = parse_hex_color(value)) {
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

std::optional<std::string> attr(const IRNode& node, std::string_view key) {
    auto it = node.attributes.find(std::string(key));
    if (it == node.attributes.end())
        return std::nullopt;
    return it->second;
}

std::optional<float> attr_float(const IRNode& node, std::string_view key) {
    auto value = attr(node, key);
    if (!value)
        return std::nullopt;
    return parse_float(*value);
}

bool attr_bool(const IRNode& node, std::string_view key) {
    auto value = attr(node, key);
    if (!value)
        return false;
    std::string lower;
    lower.reserve(value->size());
    for (unsigned char c : *value)
        lower += static_cast<char>(std::tolower(c));
    return lower == "true" || lower == "1" || lower == "yes" || lower == "on";
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
    if (auto value = attr_float(node, "value"))
        return std::clamp(*value, 0.0f, 1.0f);
    return normalized_audio_default(node);
}

std::optional<std::string> first_asset_id(const IRNode& node) {
    for (std::string_view key : {"srcAssetId", "backgroundImageAssetId", "hrefAssetId"}) {
        auto value = attr(node, key);
        if (value && !value->empty())
            return value;
    }
    std::vector<std::pair<std::string, std::string>> candidates;
    for (const auto& [key, value] : node.attributes) {
        if (key.size() >= 7 && key.rfind("AssetId") == key.size() - 7 && !value.empty())
            candidates.emplace_back(key, value);
    }
    std::sort(candidates.begin(), candidates.end());
    if (!candidates.empty())
        return candidates.front().second;
    return std::nullopt;
}

std::string asset_uri(const IRAssetManifest& manifest, std::string_view asset_id) {
    const auto* asset = manifest.resolve(asset_id);
    if (asset == nullptr)
        return {};
    if (asset->local_path && !asset->local_path->empty())
        return "file://" + *asset->local_path;
    if (!asset->original_uri.empty() &&
        (asset->original_uri.rfind("data:", 0) == 0 ||
         asset->original_uri.rfind("resource:", 0) == 0 ||
         asset->original_uri.rfind("memory:", 0) == 0)) {
        return asset->original_uri;
    }
    return {};
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
        if (parse_hex_color(value))
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
    if (node.layout.display && *node.layout.display == "grid")
        emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_layout_mode(pulp::view::LayoutMode::grid);");
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

bool is_horizontal(const IRNode& node) {
    if (auto orientation = attr(node, "orientation"); orientation && lower_copy(*orientation) == "horizontal")
        return true;
    if (auto type = attr(node, "type"); type && lower_copy(*type) == "range")
        return true;
    return node.style.width && node.style.height && *node.style.width >= *node.style.height;
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
    const auto text = resolved.text.value_or(node.text_content);
    switch (resolved.kind) {
        case NativeWidgetKind::label:
            emit_label_style(out, depth, ctx, var, node.style);
            break;
        case NativeWidgetKind::text_editor:
            if (!text.empty())
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_text(" + cpp_string_literal(text) + ");");
            break;
        case NativeWidgetKind::checkbox:
            if (attr_bool(node, "checked"))
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_checked(true);");
            break;
        case NativeWidgetKind::toggle_button:
            if (!text.empty())
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_label(" + cpp_string_literal(text) + ");");
            if (attr_bool(node, "checked") || attr_bool(node, "value"))
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_on(true);");
            break;
        case NativeWidgetKind::knob: {
            if (!text.empty())
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_label(" + cpp_string_literal(text) + ");");
            const auto value = float_expr(ctx, normalized_audio_value(node));
            const auto default_value = float_expr(ctx, normalized_audio_default(node));
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_value(/* TODO: bind to param */ " + value + ");");
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_default_value(" + default_value + ");");
            if (auto schema = attr(node, "pulpWidgetSchema"); schema && !schema->empty())
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_widget_schema(" + cpp_string_literal(*schema) + ");");
            if (auto show_label = attr(node, "pulpShowInternalLabel"); show_label && lower_copy(*show_label) == "false")
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_show_label(false);");
            break;
        }
        case NativeWidgetKind::fader: {
            if (!text.empty())
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_label(" + cpp_string_literal(text) + ");");
            emit_line(out, depth, opts.indent_spaces,
                      std::string(var) + "->set_value(/* TODO: bind to param */ " +
                          float_expr(ctx, normalized_audio_value(node)) + ");");
            if (is_horizontal(node))
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_orientation(pulp::view::Fader::Orientation::horizontal);");
            if (auto schema = attr(node, "pulpWidgetSchema"); schema && !schema->empty())
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_widget_schema(" + cpp_string_literal(*schema) + ");");
            if (auto shape = attr(node, "pulpThumbShape")) {
                const auto lower = lower_copy(*shape);
                if (lower == "rectangle" || lower == "rect" || lower == "rounded_rect")
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_thumb_shape(pulp::view::Fader::ThumbShape::rectangle);");
            }
            const auto thumb_width = attr_float(node, "pulpThumbWidth");
            const auto thumb_height = attr_float(node, "pulpThumbHeight");
            if (thumb_width || thumb_height) {
                emit_line(out, depth, opts.indent_spaces,
                          std::string(var) + "->set_thumb_size(" +
                              float_expr(ctx, thumb_width.value_or(0.0f)) + ", " +
                              float_expr(ctx, thumb_height.value_or(0.0f)) + ");");
            }
            if (auto radius = attr_float(node, "pulpThumbCornerRadius")) {
                emit_line(out, depth, opts.indent_spaces,
                          std::string(var) + "->set_thumb_corner_radius(" + float_expr(ctx, *radius) + ");");
            }
            break;
        }
        case NativeWidgetKind::meter: {
            const auto value = float_expr(ctx, normalized_audio_value(node));
            const auto peak = float_expr(ctx, attr_float(node, "peak").value_or(normalized_audio_value(node)));
            emit_line(out, depth, opts.indent_spaces,
                      std::string(var) + "->set_level(/* TODO: bind to meter */ " + value + ", " + peak + ");");
            if (auto orientation = attr(node, "orientation"); orientation && *orientation == "horizontal")
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_orientation(pulp::view::Meter::Orientation::horizontal);");
            break;
        }
        case NativeWidgetKind::xy_pad:
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_x(" + float_expr(ctx, attr_float(node, "x").value_or(0.5f)) + ");");
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_y(" + float_expr(ctx, attr_float(node, "y").value_or(0.5f)) + ");");
            if (auto label = attr(node, "xLabel")) emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_x_label(" + cpp_string_literal(*label) + ");");
            if (auto label = attr(node, "yLabel")) emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_y_label(" + cpp_string_literal(*label) + ");");
            break;
        case NativeWidgetKind::image_view:
            if (auto asset_id = first_asset_id(node)) {
                const auto uri = asset_uri(manifest, *asset_id);
                if (!uri.empty())
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_image_source(" + cpp_string_literal(uri) + ");");
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

void emit_node(std::ostringstream& out,
               EmitContext& ctx,
               const IRNode& node,
               const ResolvedNativeNode& resolved,
               std::string_view parent_var,
               int depth,
               std::optional<LayoutDirection> parent_direction,
               int& counter) {
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
    emit_line(out, depth, ctx.opts.indent_spaces,
              "auto " + var + " = " + widget_make_expr(node, resolved, ctx.manifest) + ";");
    emit_line(out, depth, ctx.opts.indent_spaces,
              var + "->set_id(" + cpp_string_literal(resolved.id) + ");");
    if (node.stable_anchor_id && !node.stable_anchor_id->empty())
        emit_line(out, depth, ctx.opts.indent_spaces,
                  var + "->set_anchor_id(" + cpp_string_literal(*node.stable_anchor_id) + ");");
    if (auto label = resolved.text; label && !label->empty())
        emit_line(out, depth, ctx.opts.indent_spaces,
                  var + "->set_access_label(" + cpp_string_literal(*label) + ");");

    emit_common_layout(out, depth, ctx, var, node, parent_direction);
    emit_visual_style(out, depth, ctx, var, node.style);
    emit_widget_specific(out, depth, ctx, var, node, resolved, ctx.manifest);

    const auto count = std::min(node.children.size(), resolved.children.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto& child = node.children[i];
        auto found = ctx.extracted.find(&child);
        if (found != ctx.extracted.end()) {
            emit_line(out, depth, ctx.opts.indent_spaces,
                      var + "->add_child(" + found->second + "());");
        } else {
            emit_node(out, ctx, child, resolved.children[i], var, depth, node.layout.direction, counter);
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

bool has_binding_manifest_metadata(const IRNode& node) {
    for (std::string_view key : {
             "pulpRouteId",
             "pulpRouteType",
             "pulpSourceFamily",
             "pulpSourcePath",
             "pulpParamKey",
             "pulpBindingModule",
             "pulpBindingParam",
             "pulpParamKeyX",
             "pulpParamKeyY",
             "pulpBindingModuleX",
             "pulpBindingParamX",
             "pulpBindingModuleY",
             "pulpBindingParamY",
             "pulpMeterSource",
             "pulpMeterChannel",
             "pulpMeterValueKey",
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

void collect_binding_manifest_entries(std::ostringstream& out,
                                      const IRNode& node,
                                      const ResolvedNativeNode& resolved,
                                      std::string_view ir_path,
                                      bool& first_entry) {
    if (has_binding_manifest_metadata(node)) {
        if (!first_entry)
            out << ",";
        out << "\n    {";
        bool first_field = true;
        if (auto route_id = attr(node, "pulpRouteId"); route_id && !route_id->empty()) {
            append_json_field(out, first_field, "id", *route_id);
        } else if (node.stable_anchor_id && !node.stable_anchor_id->empty()) {
            append_json_field(out, first_field, "id", *node.stable_anchor_id);
        } else if (!node.name.empty()) {
            append_json_field(out, first_field, "id", node.name);
        }
        append_json_field(out, first_field, "ir_path", ir_path);
        if (node.stable_anchor_id && !node.stable_anchor_id->empty())
            append_json_field(out, first_field, "anchor_id", *node.stable_anchor_id);
        append_json_field(out, first_field, "native_primitive", native_widget_kind_name(resolved.kind));
        append_json_field_if_present(out, first_field, "route_type", attr(node, "pulpRouteType"));
        append_json_field_if_present(out, first_field, "source_family", attr(node, "pulpSourceFamily"));
        append_json_field_if_present(out, first_field, "source_path", attr(node, "pulpSourcePath"));
        append_json_field_if_present(out, first_field, "param_key", attr(node, "pulpParamKey"));
        append_json_field_if_present(out, first_field, "binding_module", attr(node, "pulpBindingModule"));
        append_json_field_if_present(out, first_field, "binding_param", attr(node, "pulpBindingParam"));
        append_json_field_if_present(out, first_field, "x_param_key", attr(node, "pulpParamKeyX"));
        append_json_field_if_present(out, first_field, "y_param_key", attr(node, "pulpParamKeyY"));
        append_json_field_if_present(out, first_field, "x_binding_module", attr(node, "pulpBindingModuleX"));
        append_json_field_if_present(out, first_field, "x_binding_param", attr(node, "pulpBindingParamX"));
        append_json_field_if_present(out, first_field, "y_binding_module", attr(node, "pulpBindingModuleY"));
        append_json_field_if_present(out, first_field, "y_binding_param", attr(node, "pulpBindingParamY"));
        append_json_field_if_present(out, first_field, "meter_source", attr(node, "pulpMeterSource"));
        append_json_field_if_present(out, first_field, "meter_channel", attr(node, "pulpMeterChannel"));
        append_json_field_if_present(out, first_field, "meter_value_key", attr(node, "pulpMeterValueKey"));
        append_json_field_if_present(out, first_field, "thumb_shape", attr(node, "pulpThumbShape"));
        append_json_field_if_present(out, first_field, "thumb_width", attr(node, "pulpThumbWidth"));
        append_json_field_if_present(out, first_field, "thumb_height", attr(node, "pulpThumbHeight"));
        append_json_field_if_present(out, first_field, "thumb_corner_radius", attr(node, "pulpThumbCornerRadius"));
        append_json_field_if_present(out, first_field, "event_contract", attr(node, "pulpEventContract"));
        append_json_field_if_present(out, first_field, "gesture_contract", attr(node, "pulpGestureContract"));
        append_json_field_if_present(out, first_field, "host_action", attr(node, "pulpHostAction"));
        append_json_field_if_present(out, first_field, "style_tokens", attr(node, "pulpStyleTokens"));
        append_json_field_if_present(out, first_field, "default_value_source", attr(node, "pulpDefaultValueSource"));
        append_json_field_if_present(out, first_field, "fallback_reason", attr(node, "pulpFallbackReason"));
        out << "\n    }";
        first_entry = false;
    }

    const auto count = std::min(node.children.size(), resolved.children.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto child_path = std::string(ir_path) + "/" + std::to_string(i);
        collect_binding_manifest_entries(out, node.children[i], resolved.children[i], child_path, first_entry);
    }
}

std::string build_binding_manifest_json(const DesignIR& ir, const ResolvedNativeNode& resolved) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"pulp-native-cpp-binding-manifest-v1\",\n"
        << "  \"entries\": [";
    bool first_entry = true;
    collect_binding_manifest_entries(out, ir.root, resolved, "root", first_entry);
    if (!first_entry)
        out << "\n  ";
    out << "]\n"
        << "}\n";
    return out.str();
}

struct BindingHelperRoute {
    NativeWidgetKind kind = NativeWidgetKind::view;
    std::string anchor_id;
    std::string route_id;
    std::string param_key;
    std::string binding_module;
    std::string binding_param;
    std::string x_param_key;
    std::string y_param_key;
    std::string x_binding_module;
    std::string x_binding_param;
    std::string y_binding_module;
    std::string y_binding_param;
    std::string meter_source;
    std::string meter_channel;
    std::string meter_value_key;
    std::string event_contract;
    std::string gesture_contract;
};

void collect_binding_helper_routes(std::vector<BindingHelperRoute>& routes,
                                   const IRNode& node,
                                   const ResolvedNativeNode& resolved) {
    auto route_id = attr(node, "pulpRouteId");
    auto param_key = attr(node, "pulpParamKey");
    auto x_param_key = attr(node, "pulpParamKeyX");
    auto y_param_key = attr(node, "pulpParamKeyY");
    auto meter_source = attr(node, "pulpMeterSource");
    auto meter_channel = attr(node, "pulpMeterChannel");
    const bool has_single_param = param_key && !param_key->empty();
    const bool has_xy_params = resolved.kind == NativeWidgetKind::xy_pad &&
        x_param_key && !x_param_key->empty() &&
        y_param_key && !y_param_key->empty();
    const bool has_meter_input = resolved.kind == NativeWidgetKind::meter &&
        meter_source && !meter_source->empty() &&
        meter_channel && !meter_channel->empty();
    if (route_id && !route_id->empty() &&
        (has_single_param || has_xy_params || has_meter_input) &&
        node.stable_anchor_id && !node.stable_anchor_id->empty()) {
        routes.push_back(BindingHelperRoute{
            .kind = resolved.kind,
            .anchor_id = *node.stable_anchor_id,
            .route_id = *route_id,
            .param_key = param_key.value_or(std::string{}),
            .binding_module = attr(node, "pulpBindingModule").value_or(std::string{}),
            .binding_param = attr(node, "pulpBindingParam").value_or(std::string{}),
            .x_param_key = x_param_key.value_or(std::string{}),
            .y_param_key = y_param_key.value_or(std::string{}),
            .x_binding_module = attr(node, "pulpBindingModuleX").value_or(std::string{}),
            .x_binding_param = attr(node, "pulpBindingParamX").value_or(std::string{}),
            .y_binding_module = attr(node, "pulpBindingModuleY").value_or(std::string{}),
            .y_binding_param = attr(node, "pulpBindingParamY").value_or(std::string{}),
            .meter_source = meter_source.value_or(std::string{}),
            .meter_channel = meter_channel.value_or(std::string{}),
            .meter_value_key = attr(node, "pulpMeterValueKey").value_or(std::string{}),
            .event_contract = attr(node, "pulpEventContract").value_or(std::string{}),
            .gesture_contract = attr(node, "pulpGestureContract").value_or(std::string{}),
        });
    }

    const auto count = std::min(node.children.size(), resolved.children.size());
    for (std::size_t i = 0; i < count; ++i)
        collect_binding_helper_routes(routes, node.children[i], resolved.children[i]);
}

void emit_binding_context_helpers(std::ostringstream& out,
                                  const CppExportOptions& opts,
                                  const std::vector<BindingHelperRoute>& routes) {
    out << "namespace {\n"
        << "pulp::view::View* find_imported_view_by_anchor(pulp::view::View& root, std::string_view anchor) {\n"
        << "    if (root.anchor_id() == anchor) return &root;\n"
        << "    for (std::size_t i = 0; i < root.child_count(); ++i) {\n"
        << "        if (auto* found = find_imported_view_by_anchor(*root.child_at(i), anchor)) return found;\n"
        << "    }\n"
        << "    return nullptr;\n"
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

    auto emit_meter_descriptor = [&](const BindingHelperRoute& route, int depth) {
        emit_line(out, depth, opts.indent_spaces, "pulp::view::NativeImportMeterBindingDescriptor{");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.route_id) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.meter_source) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.meter_channel) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.meter_value_key) + ",");
        emit_line(out, depth + 1, opts.indent_spaces, cpp_string_literal(route.event_contract));
        emit_line(out, depth, opts.indent_spaces, "});");
    };

    for (const auto& route : routes) {
        if (route.kind != NativeWidgetKind::knob &&
            route.kind != NativeWidgetKind::fader &&
            route.kind != NativeWidgetKind::meter &&
            route.kind != NativeWidgetKind::toggle_button &&
            route.kind != NativeWidgetKind::xy_pad)
            continue;
        emit_line(out, 1, opts.indent_spaces,
                  "if (auto* view = find_imported_view_by_anchor(root, " +
                      cpp_string_literal(route.anchor_id) + ")) {");
        if (route.kind == NativeWidgetKind::knob) {
            emit_line(out, 2, opts.indent_spaces,
                      "if (auto* knob = dynamic_cast<pulp::view::Knob*>(view)) {");
            emit_line(out, 3, opts.indent_spaces, "ctx.bind_knob(*knob,");
            emit_descriptor(route, 3);
        } else {
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
            if (route.kind == NativeWidgetKind::toggle_button) {
                emit_line(out, 2, opts.indent_spaces,
                          "if (auto* button = dynamic_cast<pulp::view::ToggleButton*>(view)) {");
                emit_line(out, 3, opts.indent_spaces, "ctx.bind_toggle_button(*button,");
                emit_descriptor(route, 3);
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
    std::vector<BindingHelperRoute> binding_helper_routes;
    if (opts.emit_binding_context_helpers)
        collect_binding_helper_routes(binding_helper_routes, ir.root, resolved);
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
           << "#include <string_view>\n"
           << "#include <utility>\n"
           << "#include <pulp/view/buttons.hpp>\n"
           << "#include <pulp/view/canvas_widget.hpp>\n"
           << "#include <pulp/view/svg_path_widget.hpp>\n"
           << "#include <pulp/view/text_editor.hpp>\n"
           << "#include <pulp/view/widgets.hpp>\n"
           << "#include <pulp/view/widgets/svg_line.hpp>\n"
           << "#include <pulp/view/widgets/svg_rect.hpp>\n\n";

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
    result.source = source.str();
    result.binding_manifest = build_binding_manifest_json(ir, resolved);
    return result;
}

}  // namespace pulp::view
