#include "design_cpp_codegen_internal.hpp"

#include "design_ir_helpers.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulp::view::cpp_codegen {

static std::string indent(int depth, int spaces) {
    return std::string(static_cast<std::size_t>(std::max(0, depth * spaces)), ' ');
}

static std::string cpp_string_escape(std::string_view input) {
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

std::string format_float(float value) {
    std::ostringstream out;
    out << std::setprecision(7) << value;
    auto text = out.str();
    if (text.find_first_of(".eE") == std::string::npos)
        text += ".0";
    text += "f";
    return text;
}

static std::string sanitize_identifier(std::string_view input, std::string_view fallback = "node") {
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

static std::string pascal_identifier(std::string_view input, std::string_view fallback = "Token") {
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

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::optional<std::array<unsigned, 4>> parse_hex_color(std::string_view value) {
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

// Parse `rgb()` / `rgba()` to an 0..255 quad. Mirrors the Swift emitter's
// parser (design_swift_codegen.cpp) deliberately: the two lanes must agree on
// a color or the same design renders differently depending on which backend
// materialized it. Percentages and the modern space-separated `rgb(R G B / A)`
// form stay unhandled here, exactly as they are there — an unrecognized value
// returns nullopt and the caller falls back, rather than guessing.
//
// The duplication is real and known: parse_hex_color / this / the materializer's
// parse_any_css_color / the Swift pair are four spellings of one concept, which is
// why they drifted in the first place. Consolidating them into a shared lowering
// helper is tracked separately; matching behavior now beats leaving the C++ lane
// silently dropping colors until that lands.
static std::optional<std::array<unsigned, 4>> parse_rgb_color(std::string_view value) {
    std::string s;
    s.reserve(value.size());
    for (char c : value)
        if (!std::isspace(static_cast<unsigned char>(c))) s += static_cast<char>(std::tolower(c));
    const bool has_alpha = s.rfind("rgba(", 0) == 0;
    const bool plain = s.rfind("rgb(", 0) == 0;
    if (!has_alpha && !plain) return std::nullopt;
    const std::size_t open = s.find('(');
    const std::size_t close = s.find(')', open);
    if (close == std::string::npos) return std::nullopt;
    std::vector<std::string> parts;
    std::string cur;
    for (std::size_t i = open + 1; i < close; ++i) {
        if (s[i] == ',') { parts.push_back(cur); cur.clear(); }
        else cur += s[i];
    }
    parts.push_back(cur);
    if (parts.size() < 3) return std::nullopt;
    auto to_u8 = [](const std::string& t, bool* ok) -> unsigned {
        try { std::size_t idx = 0; double d = std::stod(t, &idx);
              if (idx != t.size()) { *ok = false; return 0; }
              *ok = true;
              return static_cast<unsigned>(std::clamp<long>(std::lround(d), 0, 255)); }
        catch (...) { *ok = false; return 0; }
    };
    bool ok = true;
    unsigned r = to_u8(parts[0], &ok); if (!ok) return std::nullopt;
    unsigned g = to_u8(parts[1], &ok); if (!ok) return std::nullopt;
    unsigned b = to_u8(parts[2], &ok); if (!ok) return std::nullopt;
    unsigned a = 255;
    if (parts.size() >= 4) {
        try { std::size_t idx = 0; double af = std::stod(parts[3], &idx);
              if (idx != parts[3].size()) return std::nullopt;
              a = static_cast<unsigned>(std::clamp(af, 0.0, 1.0) * 255.0 + 0.5); }
        catch (...) { return std::nullopt; }
    }
    return std::array<unsigned, 4>{r, g, b, a};
}

static std::string color_literal_expr(std::string_view value) {
    // Hex first (the common case), then rgb()/rgba(). Before the rgb() arm this
    // returned an empty expression for every rgba() value, so the baked-C++ lane
    // silently dropped colors the live-JS materializer renders — the same design,
    // two different pictures, depending on which lane you took.
    auto color = parse_hex_color(value);
    if (!color) color = parse_rgb_color(value);
    if (color) {
        std::ostringstream out;
        out << "pulp::view::Color::rgba8("
            << (*color)[0] << ", " << (*color)[1] << ", "
            << (*color)[2] << ", " << (*color)[3] << ")";
        return out.str();
    }
    return {};
}

static std::optional<float> parse_float(std::string_view value) {
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

std::string asset_uri_expression(const IRAssetManifest& manifest, std::string_view asset_id) {
    const auto* asset = manifest.resolve(asset_id);
    if (asset && asset->local_path &&
        std::filesystem::path(*asset->local_path).is_relative()) {
        return "std::string(\"file://\") + (asset_base_directory / " +
               cpp_string_literal(*asset->local_path) + ").string()";
    }
    const auto uri = asset_uri(manifest, asset_id);
    return uri.empty() ? std::string{} : cpp_string_literal(uri);
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

static bool is_container_node(const IRNode& node) {
    return !node.children.empty() || node.type == "frame";
}

static bool is_structural_component_name(std::string_view name) {
    static const std::set<std::string_view> kNames = {
        "Header", "Sidebar", "Footer", "Toolbar", "Nav", "Main",
        "Section", "Aside", "Panel", "Content",
    };
    return kNames.count(name) != 0;
}

static bool is_pascal_case(std::string_view name) {
    if (name.empty() || !std::isupper(static_cast<unsigned char>(name.front())))
        return false;
    for (unsigned char c : name) {
        if (!std::isalnum(c))
            return false;
    }
    return true;
}


static std::string unique_function_name(EmitContext& ctx, std::string base) {
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
                        bool root) {
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
                         std::string_view expr) {
    if (value)
        emit_line(out, depth, opts.indent_spaces,
                  std::string(target) + "." + std::string(field) + " = " +
                      (expr.empty() ? format_float(*value) : std::string(expr)) + ";");
}

static std::string unique_symbol(std::set<std::string>& used, std::string base) {
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

static std::string token_basename(std::string_view symbol) {
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

}  // namespace pulp::view::cpp_codegen
