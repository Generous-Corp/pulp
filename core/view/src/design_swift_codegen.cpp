/// @file design_swift_codegen.cpp
/// Baked SwiftUI code generator (Workstream B1 of
/// planning/2026-06-02-design-token-export-and-swiftui-path.md).
///
/// `generate_pulp_swift` is the fourth DesignIR lowering, alongside the DOM
/// web-compat (`generate_node`), native-bridge JS (`generate_native_node`),
/// and baked C++ (`generate_pulp_cpp`) emitters. It mirrors the C++ baker's
/// core loop — resolve native widget kinds via `resolve_design_ir_native`,
/// then walk the (IRNode, ResolvedNativeNode) tree — but produces declarative
/// SwiftUI source instead of imperative View-tree construction.
///
/// B1 is an MVP skeleton: frame→VStack/HStack, text→Text, fixed
/// frame/padding/background modifiers, and knob/slider/toggle bound to the
/// existing PulpKnob/PulpSlider/PulpToggle controls. Tokens lower to a
/// code-first PulpTheme with the same base/`.dark` partition algorithm as
/// `export_css_variables`. Binding resolves a generated key by exact
/// `PulpParameter.name` match (there is no stable string param key today),
/// surfacing missing/duplicate rather than silently binding the wrong param.
/// Full style, text-runs, flex-fidelity warnings, the remaining widgets, the
/// binding-manifest parity, grid/assets, and the host scaffold are B2–B5.

#include <pulp/view/design_codegen.hpp>

#include "design_import_native_common.hpp"
#include "design_binding_metadata.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::view {

namespace {

// ── Small string / literal helpers ──────────────────────────────────────

std::string indent(int depth, int spaces) {
    return std::string(static_cast<std::size_t>(std::max(0, depth) * std::max(1, spaces)), ' ');
}

void emit_line(std::ostringstream& out, int depth, int spaces, std::string_view text) {
    out << indent(depth, spaces) << text << "\n";
}

std::string swift_string_escape(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\0': out += "\\0"; break;
            default: {
                // Any other C0 control byte (and DEL) is illegal bare inside a
                // Swift string literal — emit a unicode-scalar escape \u{xx}.
                // UTF-8 multibyte (>= 0x80) and printable ASCII pass verbatim.
                const unsigned char uc = static_cast<unsigned char>(c);
                if (uc < 0x20 || uc == 0x7f) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u{%x}", uc);
                    out += buf;
                } else {
                    out += c;
                }
            }
        }
    }
    return out;
}

std::string swift_string_literal(std::string_view input) {
    return "\"" + swift_string_escape(input) + "\"";
}

// Make arbitrary text (e.g. an imported node name) safe to drop into a single
// `// ...` line comment: collapse CR/LF and other control chars to spaces so a
// hostile name like "Panel\nnotSwift(" can't terminate the comment and inject
// source. Used for every node-derived comment in the generated Swift.
std::string swift_comment_safe(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input)
        out += (static_cast<unsigned char>(c) < 0x20) ? ' ' : c;
    return out;
}

std::string json_string_escape(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string format_float(float value) {
    // Trim trailing zeros so emitted Swift reads naturally (4 not 4.000000).
    std::ostringstream ss;
    ss << value;
    return ss.str();
}

// Map an arbitrary token path / node name to a lowerCamelCase Swift
// identifier. "color.bg" → "colorBg", "accent-primary" → "accentPrimary".
// A leading digit is prefixed with "t" so the result is a valid identifier.
// Swift reserved words that, used as a bare declaration name, fail to compile.
// A generated identifier matching one is wrapped in backticks (a valid Swift
// escaped identifier). Not exhaustive of every contextual keyword, but covers
// the declaration-keyword set a token name can realistically collide with.
bool is_swift_keyword(const std::string& s) {
    static const std::set<std::string> kw = {
        "associatedtype","class","deinit","enum","extension","fileprivate","func",
        "import","init","inout","internal","let","open","operator","private",
        "precedencegroup","protocol","public","rethrows","static","struct","subscript",
        "typealias","var","break","case","continue","default","defer","do","else",
        "fallthrough","for","guard","if","in","repeat","return","switch","where",
        "while","as","catch","false","is","nil","super","self","Self","throw",
        "throws","true","try","Any","Protocol","Type","async","await","actor",
    };
    return kw.count(s) > 0;
}

// Raw lowerCamelCase identifier from an arbitrary token name. No keyword
// escaping or collision handling — callers that emit a declaration name use
// swift_identifier (escapes keywords) and, where collisions are possible (the
// theme), a per-scope dedup map keyed on this raw form.
std::string swift_camel(std::string_view input, std::string_view fallback = "token") {
    std::string out;
    bool upper_next = false;
    for (char c : input) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            if (upper_next) {
                out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                upper_next = false;
            } else {
                out += c;
            }
        } else {
            if (!out.empty()) upper_next = true;  // separator → camel-case next
        }
    }
    if (out.empty()) out = std::string(fallback);
    if (std::isdigit(static_cast<unsigned char>(out.front()))) out = "t" + out;
    return out;
}

std::string swift_identifier(std::string_view input, std::string_view fallback = "token") {
    std::string out = swift_camel(input, fallback);
    if (is_swift_keyword(out)) out = "`" + out + "`";  // escaped identifier
    return out;
}

// PascalCase type name for a generated `struct`/`enum` ("my-ui" → "MyUi",
// "class" → "Class", "Type" → `` `Type` ``). Capitalize FIRST, then
// keyword-escape: most capitalized names aren't keywords, but reserved type
// names (Any/Type/Protocol/Self) still need backticks.
std::string swift_type_name(std::string_view input, std::string_view fallback) {
    std::string camel = swift_camel(input, fallback);
    if (camel.empty()) camel = std::string(fallback);
    camel.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(camel.front())));
    if (is_swift_keyword(camel)) camel = "`" + camel + "`";
    return camel;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse "#rgb", "#rgba", "#rrggbb", "#rrggbbaa" → [r,g,b,a] in 0..255.
std::optional<std::array<unsigned, 4>> parse_hex_color(std::string_view value) {
    if (value.empty() || value.front() != '#') return std::nullopt;
    auto nibble = [](int v) -> unsigned { return static_cast<unsigned>((v << 4) | v); };
    if (value.size() == 4 || value.size() == 5) {
        const int r = hex_digit(value[1]);
        const int g = hex_digit(value[2]);
        const int b = hex_digit(value[3]);
        const int a = value.size() == 5 ? hex_digit(value[4]) : 15;
        if (r < 0 || g < 0 || b < 0 || a < 0) return std::nullopt;
        return std::array<unsigned, 4>{nibble(r), nibble(g), nibble(b), nibble(a)};
    }
    if (value.size() == 7 || value.size() == 9) {
        auto pair = [&](std::size_t off) -> std::optional<unsigned> {
            const int hi = hex_digit(value[off]);
            const int lo = hex_digit(value[off + 1]);
            if (hi < 0 || lo < 0) return std::nullopt;
            return static_cast<unsigned>((hi << 4) | lo);
        };
        auto r = pair(1), g = pair(3), b = pair(5);
        auto a = value.size() == 9 ? pair(7) : std::optional<unsigned>(255);
        if (!r || !g || !b || !a) return std::nullopt;
        return std::array<unsigned, 4>{*r, *g, *b, *a};
    }
    return std::nullopt;
}

// Swift `Color(.sRGB, red:…, green:…, blue:…, opacity:…)` for a hex string.
// Returns empty if the value isn't a parseable hex color (callers skip it).
std::string swift_color_expr(std::string_view value) {
    auto c = parse_hex_color(value);
    if (!c) return {};
    auto comp = [](unsigned v) {
        std::ostringstream ss;
        ss << (static_cast<double>(v) / 255.0);
        return ss.str();
    };
    std::ostringstream ss;
    ss << "Color(.sRGB, red: " << comp((*c)[0]) << ", green: " << comp((*c)[1])
       << ", blue: " << comp((*c)[2]) << ", opacity: " << comp((*c)[3]) << ")";
    return ss.str();
}

// ── Token partition (base vs `.dark`) → code-first PulpTheme ─────────────
// Same algorithm as export_css_variables (design_tokens.cpp): a token whose
// name ends in ".dark" is a dark-mode override; strip the suffix for the base
// identifier. std::map keeps emission deterministic.

constexpr std::string_view kDarkSuffix = ".dark";

bool is_dark_token(std::string_view name) {
    return name.size() > kDarkSuffix.size() &&
           name.compare(name.size() - kDarkSuffix.size(), kDarkSuffix.size(), kDarkSuffix) == 0;
}

std::string base_token_name(std::string_view name) {
    return is_dark_token(name)
               ? std::string(name.substr(0, name.size() - kDarkSuffix.size()))
               : std::string(name);
}

std::string emit_theme(const DesignIR& ir, const SwiftExportOptions& opts) {
    // Partition each token family into base + dark, keyed by base name.
    std::map<std::string, std::string> color_base, color_dark;   // hex
    std::map<std::string, float> dim_base, dim_dark;
    std::map<std::string, std::string> str_base, str_dark;

    for (auto& [name, hex] : ir.tokens.colors)
        (is_dark_token(name) ? color_dark : color_base)[base_token_name(name)] = hex;
    for (auto& [name, val] : ir.tokens.dimensions)
        (is_dark_token(name) ? dim_dark : dim_base)[base_token_name(name)] = val;
    for (auto& [name, val] : ir.tokens.strings)
        (is_dark_token(name) ? str_dark : str_base)[base_token_name(name)] = val;

    // Sanitize the theme type name too (a caller may pass an arbitrary
    // SwiftExportOptions.theme_type_name like "my-theme" or a keyword).
    const std::string type = swift_type_name(opts.theme_type_name, "PulpTheme");
    std::ostringstream out;
    if (opts.include_comments)
        out << "// Generated by pulp import-design --emit swiftui (PulpTheme)\n";
    out << "import SwiftUI\n";
    out << "#if canImport(UIKit)\n";
    out << "import UIKit\n";
    out << "#elseif canImport(AppKit)\n";
    out << "import AppKit\n";
    out << "#endif\n\n";

    bool needs_dynamic = false;
    for (auto& [name, _] : color_base)
        if (color_dark.count(name)) { needs_dynamic = true; break; }

    out << "public enum " << type << " {\n";

    // Dynamic light/dark Color helper, nested as a private static func so two
    // generated themes compiled into one Swift target don't clash on a
    // top-level symbol (each theme is a distinct enum). Only emitted when a
    // dark color override exists. Referenced unqualified by the static lets
    // below (resolves to this enum's own static).
    if (needs_dynamic) {
        out << "    /// A Color that resolves light/dark per the current appearance.\n";
        out << "    private static func dynamicColor(light: Color, dark: Color) -> Color {\n";
        out << "#if canImport(UIKit)\n";
        out << "        return Color(UIColor { traits in\n";
        out << "            traits.userInterfaceStyle == .dark ? UIColor(dark) : UIColor(light)\n";
        out << "        })\n";
        out << "#elseif canImport(AppKit)\n";
        out << "        return Color(NSColor(name: nil) { appearance in\n";
        out << "            let isDark = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua\n";
        out << "            return isDark ? NSColor(dark) : NSColor(light)\n";
        out << "        })\n";
        out << "#else\n";
        out << "        return light\n";
        out << "#endif\n";
        out << "    }\n\n";
    }

    // All static members share the PulpTheme enum scope, so identifiers must be
    // unique across colors/dims/strings AND not be Swift keywords. unique_id
    // returns the *raw* deduped camelCase form (so "foo.bar" and "foo-bar"
    // don't both emit "fooBar"); `Dark` companions are reserved alongside it.
    // Keyword-escaping is applied by esc() at the emission site — AFTER any
    // `Dark` suffix is appended — so a `default` dimension+dark pair emits
    // `default`/`defaultDark`, never the invalid ``default`Dark`.
    std::set<std::string> taken;
    auto unique_id = [&](const std::string& name, const char* fb) -> std::string {
        std::string base = swift_camel(name, fb);
        std::string cand = base;
        for (int n = 2; taken.count(cand); ++n) cand = base + std::to_string(n);
        taken.insert(cand);
        taken.insert(cand + "Dark");  // reserve the dark companion id too
        return cand;
    };
    auto esc = [](const std::string& id) {
        return is_swift_keyword(id) ? "`" + id + "`" : id;
    };

    // Colors.
    if (!color_base.empty() || !color_dark.empty()) {
        if (opts.include_comments) out << "    // Colors\n";
        for (auto& [name, hex] : color_base) {
            std::string light = swift_color_expr(hex);
            if (light.empty()) continue;  // skip non-hex (e.g. named) colors in B1
            std::string id = esc(unique_id(name, "color"));
            auto dk = color_dark.find(name);
            if (dk != color_dark.end()) {
                std::string dark = swift_color_expr(dk->second);
                if (!dark.empty()) {
                    out << "    public static let " << id << ": Color = dynamicColor(\n";
                    out << "        light: " << light << ",\n";
                    out << "        dark: " << dark << ")\n";
                    continue;
                }
            }
            out << "    public static let " << id << ": Color = " << light << "\n";
        }
        // Dark-only colors (no base): emit using the dark value as the sole
        // value with a note — there is no light counterpart to pair it with.
        for (auto& [name, hex] : color_dark) {
            if (color_base.count(name)) continue;
            std::string expr = swift_color_expr(hex);
            if (expr.empty()) continue;
            std::string id = esc(unique_id(name, "color"));
            if (opts.include_comments)
                out << "    // dark-only token (no light base)\n";
            out << "    public static let " << id << ": Color = " << expr << "\n";
        }
    }

    // Dimensions.
    if (!dim_base.empty() || !dim_dark.empty()) {
        if (opts.include_comments) out << "    // Dimensions\n";
        for (auto& [name, val] : dim_base) {
            std::string id = unique_id(name, "dimension");
            out << "    public static let " << esc(id) << ": CGFloat = " << format_float(val) << "\n";
            auto dk = dim_dark.find(name);
            if (dk != dim_dark.end())
                out << "    public static let " << esc(id + "Dark") << ": CGFloat = "
                    << format_float(dk->second) << "\n";
        }
        for (auto& [name, val] : dim_dark) {
            if (dim_base.count(name)) continue;
            out << "    public static let " << esc(unique_id(name, "dimension") + "Dark")
                << ": CGFloat = " << format_float(val) << "\n";
        }
    }

    // Strings.
    if (!str_base.empty() || !str_dark.empty()) {
        if (opts.include_comments) out << "    // Strings\n";
        for (auto& [name, val] : str_base) {
            std::string id = unique_id(name, "string");
            out << "    public static let " << esc(id) << ": String = " << swift_string_literal(val) << "\n";
            auto dk = str_dark.find(name);
            if (dk != str_dark.end())
                out << "    public static let " << esc(id + "Dark") << ": String = "
                    << swift_string_literal(dk->second) << "\n";
        }
        for (auto& [name, val] : str_dark) {
            if (str_base.count(name)) continue;
            out << "    public static let " << esc(unique_id(name, "string") + "Dark")
                << ": String = " << swift_string_literal(val) << "\n";
        }
    }

    out << "}\n";
    return out.str();
}

// ── View emission ───────────────────────────────────────────────────────

struct SwiftEmitCtx {
    const SwiftExportOptions& opts;
    const IRAssetManifest& manifest;
};

// The string the generated control resolves against PulpParameter.name. B1's
// convention is an exact match on the runtime parameter's *display name*, so we
// use the widget's display label — the audio label (e.g. "Gain") or, lacking
// one, the node name. We deliberately do NOT use pulpParamKey here: that is the
// design tool's *canonical* key (e.g. "filter.cutoff_hz"), a different concept
// from ParamInfo's human-readable `name`, and would systematically miss. The
// canonical key is preserved as metadata in the binding manifest for B4, which
// will add a real key→parameter resolver. Until then a name mismatch surfaces
// as a visible missing/duplicate placeholder, never a silent mis-bind.
std::string binding_resolve_name(const IRNode& node) {
    if (!node.audio_label.empty()) return node.audio_label;
    return node.name;
}

bool is_bound_widget(NativeWidgetKind kind) {
    switch (kind) {
        case NativeWidgetKind::knob:
        case NativeWidgetKind::fader:
        case NativeWidgetKind::toggle_button:
        case NativeWidgetKind::checkbox:
            return true;
        default:
            return false;
    }
}

// Emit a bound control as an inline `switch` over the resolver result so a
// missing or duplicate parameter is visible rather than silently mis-bound.
void emit_bound_control(std::ostringstream& out, const SwiftEmitCtx& ctx,
                        NativeWidgetKind kind, const IRNode& node,
                        const std::string& key, int depth) {
    const int s = ctx.opts.indent_spaces;
    std::string control;
    switch (kind) {
        case NativeWidgetKind::knob: {
            float size = 60.0f;
            if (node.style.width) size = *node.style.width;
            else if (node.style.height) size = *node.style.height;
            control = "PulpKnob(parameter: p, size: " + format_float(size) + ")";
            break;
        }
        case NativeWidgetKind::fader:
            control = "PulpSlider(parameter: p)";
            break;
        case NativeWidgetKind::toggle_button:
        case NativeWidgetKind::checkbox:
            control = "PulpToggle(parameter: p)";
            break;
        default:
            control = "EmptyView()";
            break;
    }
    const std::string lit = swift_string_literal(key);
    emit_line(out, depth, s, "switch resolver.resolveParameter(named: " + lit + ") {");
    emit_line(out, depth, s, "case .resolved(let p): " + control);
    emit_line(out, depth, s, "case .missing:");
    emit_line(out, depth + 1, s, "Text(\"⚠︎ missing parameter: \\(" + lit + ")\")");
    emit_line(out, depth + 2, s, ".font(.caption).foregroundColor(.red)");
    emit_line(out, depth, s, "case .duplicate:");
    emit_line(out, depth + 1, s, "Text(\"⚠︎ duplicate parameter: \\(" + lit + ")\")");
    emit_line(out, depth + 2, s, ".font(.caption).foregroundColor(.orange)");
    emit_line(out, depth, s, "}");
}

void emit_node(std::ostringstream& out, const SwiftEmitCtx& ctx,
               const IRNode& node, const ResolvedNativeNode& resolved, int depth);

// Emit child indices [lo, hi) into the current ViewBuilder, keeping every
// container's direct child count <= 10 (SwiftUI's ViewBuilder arity limit) by
// recursively wrapping in Group blocks. For N <= 10, emit directly. Otherwise
// fan out into <= 10 Groups (chunk size = ceil(N/10)), each recursively
// batched — so arbitrarily large child counts (>100, >1000) stay valid Swift,
// not just the one-level case.
void emit_child_range(std::ostringstream& out, const SwiftEmitCtx& ctx,
                      const IRNode& node, const ResolvedNativeNode& resolved,
                      std::size_t lo, std::size_t hi, int depth) {
    const int s = ctx.opts.indent_spaces;
    const std::size_t n = hi - lo;
    if (n <= 10) {
        for (std::size_t i = lo; i < hi; ++i)
            emit_node(out, ctx, node.children[i], resolved.children[i], depth);
        return;
    }
    const std::size_t chunk = (n + 9) / 10;  // ceil(n/10) → at most 10 groups
    for (std::size_t start = lo; start < hi; start += chunk) {
        emit_line(out, depth, s, "Group {");
        emit_child_range(out, ctx, node, resolved, start,
                         std::min(start + chunk, hi), depth + 1);
        emit_line(out, depth, s, "}");
    }
}

void emit_children(std::ostringstream& out, const SwiftEmitCtx& ctx,
                   const IRNode& node, const ResolvedNativeNode& resolved, int depth) {
    const std::size_t count = std::min(node.children.size(), resolved.children.size());
    if (count > 10 && ctx.opts.include_comments)
        emit_line(out, depth, ctx.opts.indent_spaces,
                  "// >10 children: recursively batched into Group blocks (ViewBuilder arity)");
    emit_child_range(out, ctx, node, resolved, 0, count, depth);
}

// Append the B1 fixed-style modifiers (.frame / .padding / .background) to the
// view expression just emitted. Modifiers are emitted as continuation lines
// indented one level deeper than the view keyword.
void emit_modifiers(std::ostringstream& out, const SwiftEmitCtx& ctx,
                    const IRNode& node, int depth) {
    const int s = ctx.opts.indent_spaces;
    const auto& st = node.style;
    if (st.width || st.height) {
        std::string frame = ".frame(";
        bool first = true;
        if (st.width)  { frame += "width: " + format_float(*st.width); first = false; }
        if (st.height) { frame += (first ? "" : ", ") + std::string("height: ") + format_float(*st.height); }
        frame += ")";
        emit_line(out, depth + 1, s, frame);
    }
    const auto& ly = node.layout;
    if (ly.padding_top || ly.padding_right || ly.padding_bottom || ly.padding_left) {
        std::ostringstream pad;
        pad << ".padding(EdgeInsets(top: " << format_float(ly.padding_top)
            << ", leading: " << format_float(ly.padding_left)
            << ", bottom: " << format_float(ly.padding_bottom)
            << ", trailing: " << format_float(ly.padding_right) << "))";
        emit_line(out, depth + 1, s, pad.str());
    }
    if (st.background_color) {
        std::string color = swift_color_expr(*st.background_color);
        if (!color.empty())
            emit_line(out, depth + 1, s, ".background(" + color + ")");
    }
}

// Emit a single SwiftUI view expression for one (node, resolved) pair.
void emit_node(std::ostringstream& out, const SwiftEmitCtx& ctx,
               const IRNode& node, const ResolvedNativeNode& resolved, int depth) {
    const int s = ctx.opts.indent_spaces;
    if (ctx.opts.include_comments && !node.name.empty())
        emit_line(out, depth, s, "// " + swift_comment_safe(node.name));

    const NativeWidgetKind kind = resolved.kind;

    // Text.
    if (kind == NativeWidgetKind::label || node.type == "text") {
        std::string text = resolved.text ? *resolved.text : node.text_content;
        emit_line(out, depth, s, "Text(" + swift_string_literal(text) + ")");
        if (node.style.font_size)
            emit_line(out, depth + 1, s,
                      ".font(.system(size: " + format_float(*node.style.font_size) + "))");
        if (node.style.color) {
            std::string color = swift_color_expr(*node.style.color);
            if (!color.empty())
                emit_line(out, depth + 1, s, ".foregroundColor(" + color + ")");
        }
        return;
    }

    // Bound audio controls (knob/slider/toggle).
    if (is_bound_widget(kind)) {
        const std::string key = binding_resolve_name(node);
        if (key.empty()) {
            if (ctx.opts.include_comments)
                emit_line(out, depth, s, "// unbound " + std::string(native_widget_kind_name(kind))
                                              + " (no param key)");
            emit_line(out, depth, s,
                      "Text(\"⚠︎ unbound " + std::string(native_widget_kind_name(kind)) + "\")");
            emit_line(out, depth + 1, s, ".font(.caption).foregroundColor(.orange)");
            return;
        }
        emit_bound_control(out, ctx, kind, node, key, depth);
        return;
    }

    // Containers (view) and — for B1 — any not-yet-supported widget that has
    // children is lowered as a stack so its subtree still renders. Leaf
    // unsupported widgets degrade to a sized clear rectangle with a comment;
    // the remaining widgets (meter/xy_pad/waveform/spectrum/image/svg/canvas/
    // text_button/text_editor) are B3.
    const std::size_t child_count = std::min(node.children.size(), resolved.children.size());
    const bool is_container = (kind == NativeWidgetKind::view) || child_count > 0;
    if (is_container) {
        if (kind != NativeWidgetKind::view && ctx.opts.include_comments)
            emit_line(out, depth, s, "// " + std::string(native_widget_kind_name(kind))
                                          + " lowered as a container in B1");
        const char* stack = (node.layout.direction == LayoutDirection::row) ? "HStack" : "VStack";
        std::string open = std::string(stack) + "(spacing: " + format_float(node.layout.gap) + ") {";
        emit_line(out, depth, s, open);
        if (child_count == 0)
            emit_line(out, depth + 1, s, "EmptyView()");
        else
            emit_children(out, ctx, node, resolved, depth + 1);
        emit_line(out, depth, s, "}");
        emit_modifiers(out, ctx, node, depth);
        return;
    }

    // Unsupported leaf widget: keep the footprint with a clear rectangle.
    if (ctx.opts.include_comments)
        emit_line(out, depth, s, "// " + std::string(native_widget_kind_name(kind))
                                      + " not supported in B1 (see B3)");
    emit_line(out, depth, s, "Color.clear");
    emit_modifiers(out, ctx, node, depth);
}

std::string emit_view(const DesignIR& ir, const ResolvedNativeNode& resolved,
                      const SwiftExportOptions& opts) {
    const std::string view_name =
        swift_type_name(opts.root_view_name, "ImportedPulpView");
    std::ostringstream out;
    if (opts.include_comments)
        out << "// Generated by pulp import-design --emit swiftui\n";
    out << "import SwiftUI\n";
    out << "import PulpSwift\n\n";
    out << "public struct " << view_name
        << "<Resolver: PulpParameterResolving & ObservableObject>: View {\n";
    out << "    @ObservedObject private var resolver: Resolver\n\n";
    out << "    public init(resolver: Resolver) {\n";
    out << "        self.resolver = resolver\n";
    out << "    }\n\n";
    out << "    public var body: some View {\n";
    SwiftEmitCtx ctx{opts, ir.asset_manifest};
    emit_node(out, ctx, ir.root, resolved, 2);
    out << "    }\n";
    out << "}\n";
    return out.str();
}

// ── Minimal SwiftUI binding manifest (B1) ───────────────────────────────
// B4 brings this to parity with the C++ binding_manifest; B1 records just the
// name-keyed bound widgets so a host can pre-flight the resolver.

struct BindingEntry {
    std::string primitive;       // native widget kind
    std::string resolve_name;    // matched against PulpParameter.name (B1)
    std::string canonical_key;   // pulpParamKey, if any — metadata for B4
};

void collect_bindings(const IRNode& node, const ResolvedNativeNode& resolved,
                      std::vector<BindingEntry>& out) {
    if (is_bound_widget(resolved.kind)) {
        const std::string name = binding_resolve_name(node);
        if (!name.empty()) {
            const auto meta = NativeBindingMetadata::parse(node);
            out.push_back({native_widget_kind_name(resolved.kind), name,
                           meta.param_key.value_or("")});
        }
    }
    const std::size_t count = std::min(node.children.size(), resolved.children.size());
    for (std::size_t i = 0; i < count; ++i)
        collect_bindings(node.children[i], resolved.children[i], out);
}

std::string emit_binding_manifest(const IRNode& root, const ResolvedNativeNode& resolved) {
    std::vector<BindingEntry> bindings;
    collect_bindings(root, resolved, bindings);
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"pulp-native-swiftui-binding-manifest-v1\",\n";
    out << "  \"resolution\": { \"strategy\": \"pulp_parameter_name_exact\", "
           "\"source_field\": \"resolve_name\" },\n";
    out << "  \"entries\": [";
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        out << (i == 0 ? "\n" : ",\n");
        out << "    { \"native_primitive\": \"" << json_string_escape(bindings[i].primitive)
            << "\", \"resolve_name\": \"" << json_string_escape(bindings[i].resolve_name)
            << "\", \"canonical_key\": \"" << json_string_escape(bindings[i].canonical_key)
            << "\", \"resolution_strategy\": \"pulp_parameter_name_exact\" }";
    }
    out << (bindings.empty() ? "" : "\n  ") << "]\n";
    out << "}\n";
    return out.str();
}

} // namespace

SwiftExportResult generate_pulp_swift(const DesignIR& ir,
                                      const IRAssetManifest& manifest,
                                      const SwiftExportOptions& opts) {
    // Use the passed manifest when it carries assets, else the IR's own.
    const IRAssetManifest& effective =
        manifest.assets.empty() ? ir.asset_manifest : manifest;
    const ResolvedNativeNode resolved = resolve_design_ir_native(ir, effective);

    SwiftExportResult result;
    result.view_source = emit_view(ir, resolved, opts);
    if (opts.emit_theme) result.theme_source = emit_theme(ir, opts);
    if (opts.emit_binding_manifest)
        result.binding_manifest = emit_binding_manifest(ir.root, resolved);
    return result;
}

} // namespace pulp::view
