/// @file design_swift_codegen.cpp
/// Baked SwiftUI code generator.
///
/// `generate_pulp_swift` is the fourth DesignIR lowering, alongside the DOM
/// web-compat (`generate_node`), native-bridge JS (`generate_native_node`),
/// and baked C++ (`generate_pulp_cpp`) emitters. It mirrors the C++ baker's
/// core loop — resolve native widget kinds via `resolve_design_ir_native`,
/// then walk the (IRNode, ResolvedNativeNode) tree — but produces declarative
/// SwiftUI source instead of imperative View-tree construction.
///
/// The generator lowers frame→VStack/HStack, text→Text,
/// fixed frame/padding/background modifiers, and knob/slider/toggle bound to
/// the existing PulpKnob/PulpSlider/PulpToggle controls. Tokens lower to a
/// code-first PulpTheme with the same base/`.dark` partition algorithm as
/// `export_css_variables`. Binding resolves a generated key by exact
/// `PulpParameter.name` match (there is no stable string param key today),
/// surfacing missing/duplicate rather than silently binding the wrong param.
/// Visual styling covers opacity, corner radius, uniform/per-side border
/// overlay, box-shadow, linear gradient, transform, mix-blend-mode, mixed-style
/// text, and fidelity warnings for divergences SwiftUI stacks cannot reproduce.
/// Remaining native widgets lower to PulpMeter, PulpXYPad, PulpWaveform,
/// PulpSpectrum, and SwiftUI Button views.

#include <pulp/view/design_codegen.hpp>

#include "design_import_native_common.hpp"
#include "design_binding_metadata.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
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
    // A non-finite value (inf/nan) has no valid Swift Float literal — `ss <<`
    // would emit "inf"/"nan" and the generated `.frame(width: inf)` /
    // `.scaleEffect(nan)` would not compile. This is reachable: the CSS
    // transform parser casts std::stod to float (`scale(1e40)` → +inf), and a
    // degenerate source dimension can be non-finite too. Clamp to 0 — a safe,
    // visible-no-op default — rather than emit broken Swift.
    if (!std::isfinite(value)) value = 0.0f;
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

// Parse a CSS `rgb(r,g,b)` / `rgba(r,g,b,a)` token → [r,g,b,a] in 0..255.
// r/g/b are 0..255 integers (percentages and the modern slash syntax are not
// emitted by Pulp's adapters, so they are intentionally unhandled); alpha is a
// 0..1 float scaled to 0..255. Whitespace is tolerated; a malformed token
// returns nullopt so the caller can skip it.
std::optional<std::array<unsigned, 4>> parse_rgb_color(std::string_view value) {
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
    // parts are already whitespace-free, so a valid number must consume the
    // WHOLE token: "1px" (idx 1 != 3) is malformed, not 1. (Percentages remain
    // intentionally unhandled — documented above.)
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

// Parse any CSS color token Pulp's adapters emit (hex or rgb/rgba) → 0..255.
std::optional<std::array<unsigned, 4>> parse_css_color(std::string_view value) {
    if (auto hex = parse_hex_color(value)) return hex;
    return parse_rgb_color(value);
}

// Swift `Color(.sRGB, red:…, green:…, blue:…, opacity:…)` for an RGBA quad.
std::string swift_color_from_rgba(const std::array<unsigned, 4>& c) {
    auto comp = [](unsigned v) {
        std::ostringstream ss;
        ss << (static_cast<double>(v) / 255.0);
        return ss.str();
    };
    std::ostringstream ss;
    ss << "Color(.sRGB, red: " << comp(c[0]) << ", green: " << comp(c[1])
       << ", blue: " << comp(c[2]) << ", opacity: " << comp(c[3]) << ")";
    return ss.str();
}

// Swift `Color(...)` for a hex or rgb/rgba string. Returns empty if the value
// isn't a parseable color (callers skip it).
std::string swift_color_expr(std::string_view value) {
    auto c = parse_css_color(value);
    if (!c) return {};
    return swift_color_from_rgba(*c);
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
    // Non-owning fidelity sink (== opts.fidelity_report). B2 pushes
    // SwiftUI-specific divergence findings here (flex justify/wrap, absolute
    // position, grid, skew/matrix transforms, per-side borders, multi-/inset
    // shadows) so the import CLI can surface them as `fidelity:` warnings and
    // `--strict-fidelity` can gate on the non-informational ones.
    std::vector<FidelityIssue>* fidelity = nullptr;
};

// Report a SwiftUI-lowering divergence against the fidelity sink (if any).
// `informational` findings are advisory (we emitted a faithful-enough
// approximation); non-informational ones count toward --strict-fidelity.
void push_fidelity(const SwiftEmitCtx& ctx, const IRNode& node,
                   std::string kind, std::string detail, bool informational) {
    if (!ctx.fidelity) return;
    FidelityIssue issue;
    issue.node_id = node.source_node_id.value_or(node.name);
    issue.node_name = node.name;
    issue.kind = std::move(kind);
    issue.detail = std::move(detail);
    issue.informational = informational;
    ctx.fidelity->push_back(std::move(issue));
}

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
        case NativeWidgetKind::meter:
        case NativeWidgetKind::xy_pad:
        case NativeWidgetKind::waveform:
        case NativeWidgetKind::spectrum:
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
        case NativeWidgetKind::meter:
            control = "PulpMeter(parameter: p)";
            break;
        case NativeWidgetKind::xy_pad:
            control = "PulpXYPad(parameter: p)";
            push_fidelity(ctx, node, "swiftui-xypad-single-axis",
                          "xy_pad binds its X axis to the one parameter the IR carries; "
                          "the Y axis is unbound visual state", /*informational=*/true);
            break;
        case NativeWidgetKind::waveform:
            control = "PulpWaveform(parameter: p)";
            push_fidelity(ctx, node, "swiftui-static-visualizer",
                          "waveform has no audio buffer in a baked import; rendered as a "
                          "static shape scaled by the bound parameter", /*informational=*/true);
            break;
        case NativeWidgetKind::spectrum:
            control = "PulpSpectrum(parameter: p)";
            push_fidelity(ctx, node, "swiftui-static-visualizer",
                          "spectrum has no audio buffer in a baked import; rendered as a "
                          "static shape scaled by the bound parameter", /*informational=*/true);
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

// ── B2 style helpers (gradient / transform / corner / border / shadow) ───

// Split a CSS function-argument list on TOP-LEVEL commas only (commas inside
// nested parens — rgb()/rgba() — are preserved). Each part is trimmed.
std::vector<std::string> split_top_level_commas(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char c : s) {
        if (c == '(') { depth++; cur += c; }
        else if (c == ')') { depth = std::max(0, depth - 1); cur += c; }
        else if (c == ',' && depth == 0) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    for (auto& p : out) {
        std::size_t b = p.find_first_not_of(" \t\n\r");
        std::size_t e = p.find_last_not_of(" \t\n\r");
        p = (b == std::string::npos) ? std::string() : p.substr(b, e - b + 1);
    }
    return out;
}

// Inner argument list of `name(...)`, case-insensitive on the function name.
// Returns nullopt if the value isn't that function. The match must sit on an
// identifier boundary so `name="linear-gradient"` does NOT match the substring
// inside `repeating-linear-gradient(...)`, and `name="rotate"` does not match
// `xrotate(...)`. CSS function names can contain hyphens, so the boundary char
// must be neither alphanumeric NOR a hyphen.
std::optional<std::string> fn_args(std::string_view value, std::string_view name) {
    std::string lower;
    for (char c : value) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string prefix = std::string(name) + "(";
    auto is_ident = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-';
    };
    std::size_t pos = lower.find(prefix);
    while (pos != std::string::npos) {
        if (pos == 0 || !is_ident(value[pos - 1])) {
            const std::size_t open = pos + prefix.size() - 1;
            int depth = 0;
            for (std::size_t i = open; i < value.size(); ++i) {
                if (value[i] == '(') depth++;
                else if (value[i] == ')') { if (--depth == 0)
                    return std::string(value.substr(open + 1, i - open - 1)); }
            }
            return std::nullopt;  // matched name but unbalanced parens
        }
        pos = lower.find(prefix, pos + 1);
    }
    return std::nullopt;
}

// Map a CSS linear-gradient direction (angle or `to <side>`) to a SwiftUI
// (startPoint, endPoint) UnitPoint pair. CSS 0deg = upward; SwiftUI y grows
// downward, so we snap the angle to the nearest 45° and pick the matching
// pair. Default (no/￼unparseable direction) is CSS's `to bottom`.
std::pair<std::string, std::string> gradient_unit_points(const std::string& dir) {
    std::string d;
    for (char c : dir) if (!std::isspace((unsigned char)c)) d += (char)std::tolower((unsigned char)c);
    auto pts = [](const char* a, const char* b) { return std::make_pair(std::string(a), std::string(b)); };
    if (d == "toright")       return pts(".leading", ".trailing");
    if (d == "toleft")        return pts(".trailing", ".leading");
    if (d == "totop")         return pts(".bottom", ".top");
    if (d == "tobottom")      return pts(".top", ".bottom");
    if (d == "totopright"   || d == "torighttop")    return pts(".bottomLeading", ".topTrailing");
    if (d == "totopleft"    || d == "tolefttop")     return pts(".bottomTrailing", ".topLeading");
    if (d == "tobottomright"|| d == "torightbottom") return pts(".topLeading", ".bottomTrailing");
    if (d == "tobottomleft" || d == "toleftbottom")  return pts(".topTrailing", ".bottomLeading");
    // Angle form, e.g. "45deg".
    const std::size_t deg = d.find("deg");
    if (deg != std::string::npos) {
        try {
            double a = std::stod(d.substr(0, deg));
            a = std::fmod(std::fmod(a, 360.0) + 360.0, 360.0);
            const int oct = static_cast<int>(std::lround(a / 45.0)) % 8;  // 0=up
            static const std::pair<const char*, const char*> ring[8] = {
                {".bottom", ".top"},            // 0deg → up
                {".bottomLeading", ".topTrailing"},
                {".leading", ".trailing"},      // 90deg → right
                {".topLeading", ".bottomTrailing"},
                {".top", ".bottom"},            // 180deg → down
                {".topTrailing", ".bottomLeading"},
                {".trailing", ".leading"},      // 270deg → left
                {".bottomTrailing", ".topLeading"},
            };
            return pts(ring[oct].first, ring[oct].second);
        } catch (...) {}
    }
    return pts(".top", ".bottom");
}

// Build a SwiftUI `LinearGradient(...)` for a CSS `linear-gradient(...)` value.
// Returns the expression plus whether explicit non-uniform stop positions were
// dropped (SwiftUI's colors: initializer spaces stops evenly). nullopt if the
// value isn't a parseable linear-gradient with >= 2 colors.
struct GradientResult { std::string expr; bool dropped_positions = false; };
std::optional<GradientResult> swift_linear_gradient_expr(std::string_view value) {
    auto inner = fn_args(value, "linear-gradient");
    if (!inner) return std::nullopt;
    std::vector<std::string> parts = split_top_level_commas(*inner);
    if (parts.empty()) return std::nullopt;
    std::string start = ".top", end = ".bottom";
    std::size_t first_stop = 0;
    const std::string& head = parts.front();
    std::string head_l;
    for (char c : head) if (!std::isspace((unsigned char)c)) head_l += (char)std::tolower((unsigned char)c);
    if (head_l.rfind("to", 0) == 0 || head_l.find("deg") != std::string::npos) {
        std::tie(start, end) = gradient_unit_points(head);
        first_stop = 1;
    }
    std::vector<std::string> colors;
    bool dropped = false;
    for (std::size_t i = first_stop; i < parts.size(); ++i) {
        // A stop is "<color> [<position>]". The colour itself may contain spaces
        // when it is a function — rgb(0, 0, 0) / rgba(0, 0, 0, 0.5) — so split on
        // the function's matching paren, not the first space.
        const std::string& tok = parts[i];
        std::size_t paren = tok.find('(');
        std::size_t space = tok.find_first_of(" \t");
        std::string col;
        if (paren != std::string::npos && (space == std::string::npos || paren < space)) {
            int d = 0; std::size_t close = std::string::npos;
            for (std::size_t k = paren; k < tok.size(); ++k) {
                if (tok[k] == '(') d++;
                else if (tok[k] == ')' && --d == 0) { close = k; break; }
            }
            col = (close == std::string::npos) ? tok : tok.substr(0, close + 1);
            // Anything non-blank after the colour function is an explicit stop.
            if (close != std::string::npos &&
                tok.find_first_not_of(" \t", close + 1) != std::string::npos)
                dropped = true;
        } else {
            col = (space == std::string::npos) ? tok : tok.substr(0, space);
            if (space != std::string::npos) dropped = true;
        }
        std::string expr = swift_color_expr(col);
        if (!expr.empty()) colors.push_back(expr);
    }
    if (colors.size() < 2) return std::nullopt;
    std::ostringstream ss;
    ss << "LinearGradient(colors: [";
    for (std::size_t i = 0; i < colors.size(); ++i) ss << (i ? ", " : "") << colors[i];
    ss << "], startPoint: " << start << ", endPoint: " << end << ")";
    return GradientResult{ss.str(), dropped};
}

// Append CSS `transform` → SwiftUI modifier lines. rotate/scale/translate map
// cleanly; skew/matrix/3D have no SwiftUI 2-D equivalent and are reported as a
// non-informational fidelity divergence (the visual will be wrong, not just
// approximate). Returns the modifier lines to emit, in source order.
std::vector<std::string> swift_transform_modifiers(const SwiftEmitCtx& ctx,
                                                    const IRNode& node) {
    std::vector<std::string> mods;
    const std::string& css = *node.style.transform;
    if (auto a = fn_args(css, "rotate")) {
        std::string v = *a; std::size_t deg = v.find("deg");
        std::string num = (deg == std::string::npos) ? v : v.substr(0, deg);
        try { mods.push_back(".rotationEffect(.degrees(" + format_float((float)std::stod(num)) + "))"); }
        catch (...) {}
    }
    if (auto a = fn_args(css, "scale")) {
        auto comps = split_top_level_commas(*a);
        try {
            if (comps.size() >= 2)
                mods.push_back(".scaleEffect(x: " + format_float((float)std::stod(comps[0]))
                               + ", y: " + format_float((float)std::stod(comps[1])) + ")");
            else if (comps.size() == 1)
                mods.push_back(".scaleEffect(" + format_float((float)std::stod(comps[0])) + ")");
        } catch (...) {}
    }
    auto px = [](std::string t) {
        std::size_t p = t.find("px"); return (p == std::string::npos) ? t : t.substr(0, p);
    };
    if (auto a = fn_args(css, "translate")) {
        auto comps = split_top_level_commas(*a);
        try {
            float x = comps.size() >= 1 ? (float)std::stod(px(comps[0])) : 0.0f;
            float y = comps.size() >= 2 ? (float)std::stod(px(comps[1])) : 0.0f;
            mods.push_back(".offset(x: " + format_float(x) + ", y: " + format_float(y) + ")");
        } catch (...) {}
    }
    if (auto a = fn_args(css, "translatex"))
        try { mods.push_back(".offset(x: " + format_float((float)std::stod(px(*a))) + ")"); } catch (...) {}
    if (auto a = fn_args(css, "translatey"))
        try { mods.push_back(".offset(y: " + format_float((float)std::stod(px(*a))) + ")"); } catch (...) {}
    std::string lower;
    for (char c : css) lower += (char)std::tolower((unsigned char)c);
    if (lower.find("skew") != std::string::npos || lower.find("matrix") != std::string::npos ||
        lower.find("rotate3d") != std::string::npos || lower.find("perspective") != std::string::npos)
        push_fidelity(ctx, node, "swiftui-transform",
                      "transform `" + css + "` uses skew/matrix/3D, which SwiftUI's "
                      "2-D modifiers cannot represent; dropped", /*informational=*/false);
    return mods;
}

// Append the style modifiers to the view expression just emitted. Modifiers are
// emitted as continuation lines indented one level deeper than the view keyword.
// Order matters for SwiftUI: sizing → padding → fill/gradient → corner clip →
// border overlay → shadow → opacity → transform.
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

    // Fill: a gradient wins over a flat color (CSS layers background-image over
    // background-color). A non-parseable gradient falls back to the flat color.
    bool filled = false;
    if (st.background_gradient) {
        if (auto g = swift_linear_gradient_expr(*st.background_gradient)) {
            emit_line(out, depth + 1, s, ".background(" + g->expr + ")");
            filled = true;
            if (g->dropped_positions)
                push_fidelity(ctx, node, "swiftui-gradient-stops",
                              "linear-gradient explicit colour-stop positions dropped; "
                              "SwiftUI spaces stops evenly", /*informational=*/true);
        } else {
            push_fidelity(ctx, node, "swiftui-gradient",
                          "background gradient `" + *st.background_gradient +
                          "` is not a 2-colour linear-gradient; using flat fill",
                          /*informational=*/true);
        }
    }
    if (!filled && st.background_color) {
        std::string color = swift_color_expr(*st.background_color);
        if (!color.empty())
            emit_line(out, depth + 1, s, ".background(" + color + ")");
    }

    // Corner radius. SwiftUI `.cornerRadius` is uniform; non-uniform per-corner
    // radii degrade to the largest corner + an informational note.
    const std::optional<float> corners[4] = {
        st.border_top_left_radius, st.border_top_right_radius,
        st.border_bottom_right_radius, st.border_bottom_left_radius};
    bool any_corner = false; float max_corner = 0.0f; bool uneven = false;
    float first_corner = 0.0f; bool first_set = false;
    for (auto c : corners) if (c) {
        any_corner = true; max_corner = std::max(max_corner, *c);
        if (!first_set) { first_corner = *c; first_set = true; }
        else if (*c != first_corner) uneven = true;
    }
    float radius = 0.0f;
    if (st.border_radius) { radius = *st.border_radius; }
    else if (any_corner) {
        radius = max_corner;
        if (uneven)
            push_fidelity(ctx, node, "swiftui-corner-radius",
                          "per-corner radii are not uniform; SwiftUI .cornerRadius is "
                          "uniform, using the largest corner", /*informational=*/true);
    }
    if (radius > 0.0f)
        emit_line(out, depth + 1, s, ".cornerRadius(" + format_float(radius) + ")");

    // Border → an overlay stroke following the corner radius. SwiftUI's stroke
    // is uniform; a border whose WIDTH or COLOUR differs per side genuinely
    // loses a side's appearance, so it is a hard divergence (not advisory): we
    // approximate with the heaviest side's width + the first declared colour.
    // Compute the EFFECTIVE per-side width/colour: a side falls back to the
    // `border-width`/`border-color` shorthand when it has no override. Comparing
    // effective values (not just the side overrides among themselves) catches a
    // single side overriding the shorthand — e.g. `border-color:#fff` +
    // `border-top-color:#f00`.
    auto eff_w = [&](const std::optional<float>& side) -> std::optional<float> {
        return side ? side : st.border_width;
    };
    auto eff_c = [&](const std::optional<std::string>& side) -> std::optional<std::string> {
        return side ? side : st.border_color;
    };
    const std::optional<float> ew[4] = {eff_w(st.border_top_width), eff_w(st.border_right_width),
                                        eff_w(st.border_bottom_width), eff_w(st.border_left_width)};
    const std::optional<std::string> ec[4] = {
        eff_c(st.border_top_color), eff_c(st.border_right_color),
        eff_c(st.border_bottom_color), eff_c(st.border_left_color)};

    bool per_side = false; float border_w = 0.0f;
    std::optional<float> w_first;
    for (const auto& w : ew) if (w) {
        border_w = std::max(border_w, *w);
        if (!w_first) w_first = *w;
        else if (*w != *w_first) per_side = true;
    }
    if (st.border_width) border_w = std::max(border_w, *st.border_width);

    std::string border_color_tok;
    std::optional<std::string> c_first;
    for (const auto& c : ec) if (c) {
        if (border_color_tok.empty()) border_color_tok = *c;  // first effective colour
        if (!c_first) c_first = *c;
        else if (*c != *c_first) per_side = true;
    }
    if (border_color_tok.empty() && st.border_color) border_color_tok = *st.border_color;
    if (border_w > 0.0f && !border_color_tok.empty()) {
        std::string color = swift_color_expr(border_color_tok);
        if (!color.empty()) {
            std::string shape = radius > 0.0f
                ? "RoundedRectangle(cornerRadius: " + format_float(radius) + ")"
                : "Rectangle()";
            emit_line(out, depth + 1, s,
                      ".overlay(" + shape + ".stroke(" + color +
                      ", lineWidth: " + format_float(border_w) + "))");
            if (per_side)
                push_fidelity(ctx, node, "swiftui-per-side-border",
                              "per-side border width/colour differs; SwiftUI overlay stroke "
                              "is uniform, using the heaviest side + first colour",
                              /*informational=*/false);
        }
    }

    // Shadow → the first box-shadow layer. SwiftUI's radius is roughly half the
    // CSS blur. Inset and additional layers have no SwiftUI .shadow equivalent.
    if (!st.box_shadow.empty()) {
        const auto& sh = st.box_shadow.front();
        if (sh.inset) {
            push_fidelity(ctx, node, "swiftui-inset-shadow",
                          "inset box-shadow has no SwiftUI .shadow equivalent; dropped",
                          /*informational=*/false);
        } else {
            std::string color = sh.color.empty() ? "Color.black.opacity(0.33)"
                                                  : swift_color_expr(sh.color);
            if (color.empty()) color = "Color.black.opacity(0.33)";
            std::ostringstream sm;
            sm << ".shadow(color: " << color << ", radius: " << format_float(sh.blur / 2.0f)
               << ", x: " << format_float(sh.offset_x) << ", y: " << format_float(sh.offset_y) << ")";
            emit_line(out, depth + 1, s, sm.str());
        }
        // Dropping a layer can erase a visible glow/inner-ring, so a multi-layer
        // shadow is a hard divergence, not advisory.
        if (st.box_shadow.size() > 1)
            push_fidelity(ctx, node, "swiftui-multi-shadow",
                          "only the first of " + std::to_string(st.box_shadow.size()) +
                          " box-shadow layers is emitted (SwiftUI .shadow is single-layer)",
                          /*informational=*/false);
    }

    // Opacity.
    if (st.opacity && *st.opacity < 1.0f)
        emit_line(out, depth + 1, s, ".opacity(" + format_float(*st.opacity) + ")");

    // Transform (rotate/scale/translate; skew/matrix/3D flagged).
    if (st.transform)
        for (const auto& m : swift_transform_modifiers(ctx, node))
            emit_line(out, depth + 1, s, m);

    // mix-blend-mode → SwiftUI .blendMode for the modes that map; the rest are
    // flagged. CSS `multiply/screen/overlay/...` share spelling with SwiftUI's
    // BlendMode cases, but `normal` is the default (no modifier).
    if (st.mix_blend_mode && *st.mix_blend_mode != "normal") {
        static const std::set<std::string> supported = {
            "multiply","screen","overlay","darken","lighten","colorDodge","colorBurn",
            "softLight","hardLight","difference","exclusion","hue","saturation","color",
            "luminosity","plusDarker","plusLighter"};
        // CSS spells these hyphenated; map to SwiftUI's camelCase enum spelling.
        std::string m = *st.mix_blend_mode;
        std::string camel;
        bool up = false;
        for (char c : m) { if (c == '-') { up = true; } else { camel += up ? (char)std::toupper((unsigned char)c) : c; up = false; } }
        if (supported.count(camel))
            emit_line(out, depth + 1, s, ".blendMode(." + camel + ")");
        else
            push_fidelity(ctx, node, "swiftui-blend-mode",
                          "mix-blend-mode `" + m + "` has no SwiftUI BlendMode; dropped",
                          /*informational=*/false);
    }
}

// ── B2 text: mixed-style runs → concatenated Text ─────────────────────────

// CSS numeric font-weight → SwiftUI Font.Weight case (nearest bucket).
std::string swift_font_weight(int w) {
    if (w <= 150) return ".ultraLight";
    if (w <= 250) return ".thin";
    if (w <= 350) return ".light";
    if (w <= 450) return ".regular";
    if (w <= 550) return ".medium";
    if (w <= 650) return ".semibold";
    if (w <= 750) return ".bold";
    if (w <= 850) return ".heavy";
    return ".black";
}

// Snap a byte index up to the next UTF-8 codepoint boundary (run offsets are
// byte offsets into text_content; never slice mid-codepoint).
std::size_t snap_utf8(const std::string& s, std::size_t i) {
    if (i > s.size()) return s.size();
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}

// One Text(...) segment with its (possibly inherited) styling, as a Swift
// expression. SwiftUI's Text-returning modifier overloads keep the whole
// `Text(..).font(..) + Text(..)` chain typed as Text.
std::string text_segment_expr(std::string_view slice,
                              std::optional<float> size,
                              std::optional<int> weight,
                              bool italic,
                              const std::string& color_tok,
                              const std::string& decoration) {
    std::string e = "Text(" + swift_string_literal(slice) + ")";
    if (size)   e += ".font(.system(size: " + format_float(*size) + "))";
    if (weight) e += ".fontWeight(" + swift_font_weight(*weight) + ")";
    if (italic) e += ".italic()";
    if (!color_tok.empty()) {
        std::string c = swift_color_expr(color_tok);
        if (!c.empty()) e += ".foregroundColor(" + c + ")";
    }
    if (decoration == "underline")        e += ".underline()";
    else if (decoration == "line-through") e += ".strikethrough()";
    return e;
}

// Emit a text node. With per-range `text_runs` it becomes a concatenation of
// styled Text segments; without them it is the simple single Text + dominant
// style (B1 behaviour). Run byte offsets index into node.text_content.
void emit_text_node(std::ostringstream& out, const SwiftEmitCtx& ctx,
                    const IRNode& node, const ResolvedNativeNode& resolved, int depth) {
    const int s = ctx.opts.indent_spaces;
    const auto& base = node.style;

    if (node.text_runs.empty()) {
        std::string text = resolved.text ? *resolved.text : node.text_content;
        emit_line(out, depth, s, "Text(" + swift_string_literal(text) + ")");
        if (base.font_size)
            emit_line(out, depth + 1, s,
                      ".font(.system(size: " + format_float(*base.font_size) + "))");
        if (base.font_weight)
            emit_line(out, depth + 1, s, ".fontWeight(" + swift_font_weight(*base.font_weight) + ")");
        if (base.font_style && *base.font_style == "italic")
            emit_line(out, depth + 1, s, ".italic()");
        if (base.color) {
            std::string color = swift_color_expr(*base.color);
            if (!color.empty())
                emit_line(out, depth + 1, s, ".foregroundColor(" + color + ")");
        }
        return;
    }

    // Mixed-style: build segments over the full text, filling gaps with the
    // node's dominant style and overriding inside each run.
    const std::string& full = node.text_content;
    std::vector<IRTextRun> runs = node.text_runs;
    std::sort(runs.begin(), runs.end(),
              [](const IRTextRun& a, const IRTextRun& b) { return a.start < b.start; });
    const std::string base_deco = base.text_decoration.value_or("");
    const std::string base_color = base.color.value_or("");
    const bool base_italic = base.font_style && *base.font_style == "italic";

    std::vector<std::string> segs;
    auto base_seg = [&](std::size_t a, std::size_t b) {
        if (b <= a) return;
        segs.push_back(text_segment_expr(std::string_view(full).substr(a, b - a),
                                         base.font_size, base.font_weight, base_italic,
                                         base_color, base_deco));
    };
    std::size_t cursor = 0;
    for (const auto& r : runs) {
        if (r.end <= r.start) continue;
        std::size_t a = snap_utf8(full, static_cast<std::size_t>(std::max(0, r.start)));
        std::size_t b = snap_utf8(full, static_cast<std::size_t>(std::max(0, r.end)));
        a = std::max(a, cursor);
        b = std::min(b, full.size());
        if (b <= a) continue;
        base_seg(cursor, a);  // gap before the run inherits the dominant style
        segs.push_back(text_segment_expr(
            std::string_view(full).substr(a, b - a),
            r.font_size ? r.font_size : base.font_size,
            r.font_weight ? r.font_weight : base.font_weight,
            r.font_style ? (*r.font_style == "italic") : base_italic,
            r.color ? *r.color : base_color,
            r.text_decoration ? *r.text_decoration : base_deco));
        cursor = b;
    }
    base_seg(cursor, full.size());
    if (segs.empty()) segs.push_back("Text(" + swift_string_literal(full) + ")");

    if (segs.size() == 1) {
        emit_line(out, depth, s, segs.front());
        return;
    }
    // Emit the `+`-joined chain across lines: first segment, then `+ seg`.
    emit_line(out, depth, s, segs.front());
    for (std::size_t i = 1; i < segs.size(); ++i)
        emit_line(out, depth + 1, s, "+ " + segs[i]);
}

// ── B2 flex → stack mapping ───────────────────────────────────────────────

// Cross-axis alignment → SwiftUI stack alignment token. `*stretch` is set when
// the source asked to stretch children to fill the cross axis (SwiftUI stacks
// size to content; this is an approximation worth flagging).
std::string stack_alignment_token(bool row, LayoutAlign align, bool* stretch) {
    *stretch = false;
    switch (align) {
        case LayoutAlign::flex_start: return row ? ".top" : ".leading";
        case LayoutAlign::center:     return ".center";
        case LayoutAlign::flex_end:   return row ? ".bottom" : ".trailing";
        case LayoutAlign::stretch:    *stretch = true; return ".center";
        case LayoutAlign::space_between:
        case LayoutAlign::space_around: return ".center";
    }
    return ".center";
}

// Count grid columns from a CSS `grid-template-columns` track list. Splits into
// top-level tracks (paren-aware, so `minmax(…)` is one track) and sums them; a
// `repeat(N, <pattern>)` track contributes N × the pattern's track count
// (recursively), so `repeat(2, 1fr) 2fr` is 3, not 2. A non-integer repeat
// count (`auto-fill`/`auto-fit`, viewport-dependent and unknowable at codegen)
// falls back to the pattern's own count. Clamped to ≥1. Exact track SIZING is
// not modelled — B5 maps only the column COUNT onto equal flexible GridItems.
int grid_column_count(const std::string& tracks, int recursion_depth = 0) {
    // CSS forbids nested repeat(), but the IR can carry arbitrary text from a
    // non-CSS source; cap recursion so a pathological repeat(repeat(repeat(…)))
    // can't overflow the stack. Past the cap a repeat's pattern counts as one.
    constexpr int kMaxDepth = 8;
    if (recursion_depth >= kMaxDepth) return 1;

    std::vector<std::string> toks;
    std::string cur;
    int depth = 0;
    for (char c : tracks) {
        if (c == '(') { depth++; cur += c; }
        else if (c == ')') { if (depth > 0) depth--; cur += c; }
        else if ((c == ' ' || c == '\t' || c == '\n') && depth == 0) {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
        } else cur += c;
    }
    if (!cur.empty()) toks.push_back(cur);

    int total = 0;
    for (const auto& t : toks) {
        std::string lt;
        for (char c : t) lt += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lt.rfind("repeat(", 0) == 0) {
            const std::size_t comma = t.find(',');
            const std::size_t close = t.rfind(')');
            int n = 1;
            std::string pattern;
            if (comma != std::string::npos) {
                try { n = std::stoi(t.substr(7, comma - 7)); if (n < 1) n = 1; }
                catch (...) { n = 1; }  // auto-fill / auto-fit → unknowable count
                if (close != std::string::npos && close > comma)
                    pattern = t.substr(comma + 1, close - comma - 1);
            }
            total += n * grid_column_count(pattern, recursion_depth + 1);
        } else {
            total += 1;
        }
        if (total > 4096) break;  // bound a pathological repeat(100000, …)
    }
    // Clamp to a sane column count: no real grid exceeds this, and it bounds the
    // emitted GridItem array regardless of a hostile track list.
    return std::clamp(total, 1, 1024);
}

// The asset id an image node references. Mirrors design_cpp_codegen's
// first_asset_id: the explicit src/background/href keys first, then any
// `*AssetId` attribute (deterministic by sorted key).
std::optional<std::string> swift_first_asset_id(const IRNode& node) {
    for (std::string_view key : {"srcAssetId", "backgroundImageAssetId", "hrefAssetId"}) {
        auto it = node.attributes.find(std::string(key));
        if (it != node.attributes.end() && !it->second.empty()) return it->second;
    }
    std::vector<std::pair<std::string, std::string>> cands;
    for (const auto& [k, v] : node.attributes)
        if (k.size() >= 7 && k.compare(k.size() - 7, 7, "AssetId") == 0 && !v.empty())
            cands.emplace_back(k, v);
    std::sort(cands.begin(), cands.end());
    if (!cands.empty()) return cands.front().second;
    return std::nullopt;
}

void emit_node(std::ostringstream& out, const SwiftEmitCtx& ctx,
               const IRNode& node, const ResolvedNativeNode& resolved, int depth);

// Emit a container's children with Spacer interposition to approximate a
// main-axis `justify` of space-between / space-around / flex-end. Used only
// when the resulting subview count stays within the ViewBuilder arity limit;
// callers fall back to plain (batched) children + a fidelity note otherwise.
void emit_children_distributed(std::ostringstream& out, const SwiftEmitCtx& ctx,
                               const IRNode& node, const ResolvedNativeNode& resolved,
                               int depth, LayoutAlign justify) {
    const int s = ctx.opts.indent_spaces;
    const std::size_t count = std::min(node.children.size(), resolved.children.size());
    const bool around = justify == LayoutAlign::space_around;
    const bool between = justify == LayoutAlign::space_between;
    const bool end = justify == LayoutAlign::flex_end;
    if (around || end) emit_line(out, depth, s, "Spacer()");
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0 && (around || between)) emit_line(out, depth, s, "Spacer()");
        emit_node(out, ctx, node.children[i], resolved.children[i], depth);
    }
    if (around) emit_line(out, depth, s, "Spacer()");
    // space-between with a single child is flex-start: a trailing Spacer pushes
    // it to the main-axis start (there is no interior gap to place).
    else if (between && count == 1) emit_line(out, depth, s, "Spacer()");
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
        emit_text_node(out, ctx, node, resolved, depth);
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

    // Momentary text button → SwiftUI Button. The IR carries no action target
    // (clicks were React handlers / host commands, not a parameter), so the
    // generated button has an empty action; flag that it is inert.
    if (kind == NativeWidgetKind::text_button) {
        const std::size_t btn_children =
            std::min(node.children.size(), resolved.children.size());
        emit_line(out, depth, s, "Button(action: {}) {");
        if (btn_children > 0) {
            // A button with label/icon children renders them in the label
            // closure (e.g. an icon + text); dropping them would lose content.
            emit_children(out, ctx, node, resolved, depth + 1);
        } else {
            std::string label = resolved.text ? *resolved.text
                              : (!node.text_content.empty() ? node.text_content : node.name);
            emit_line(out, depth + 1, s, "Text(" + swift_string_literal(label) + ")");
        }
        emit_line(out, depth, s, "}");
        emit_modifiers(out, ctx, node, depth);
        push_fidelity(ctx, node, "swiftui-inert-button",
                      "text button has no action target in the IR; generated with an empty "
                      "action", /*informational=*/true);
        return;
    }

    // Image leaf with a resolved asset → Image / AsyncImage (B5). A remote
    // http(s) source streams via AsyncImage; everything else references the
    // asset by id in the app's asset catalog (xcassets generation deferred per
    // the B5 plan). A bare image with no resolvable asset falls through to the
    // Color.clear placeholder below. (svg/canvas vectors stay deferred.)
    if (kind == NativeWidgetKind::image_view &&
        std::min(node.children.size(), resolved.children.size()) == 0) {
        if (auto asset_id = swift_first_asset_id(node)) {
            const auto* asset = ctx.manifest.resolve(*asset_id);
            const std::string uri = asset ? asset->original_uri : std::string();
            const bool remote = uri.rfind("http://", 0) == 0 || uri.rfind("https://", 0) == 0;
            // Inline / non-bundle schemes can't be referenced by a catalog name
            // and aren't a remote URL, so Image("id") would silently render
            // nothing — a hard divergence the developer must resolve manually.
            const bool inline_uri = uri.rfind("data:", 0) == 0 || uri.rfind("blob:", 0) == 0 ||
                                    uri.rfind("memory:", 0) == 0 || uri.rfind("resource:", 0) == 0;
            if (remote) {
                emit_line(out, depth, s,
                          "AsyncImage(url: URL(string: " + swift_string_literal(uri) + ")) { image in");
                emit_line(out, depth + 1, s, "image.resizable().scaledToFit()");
                emit_line(out, depth, s, "} placeholder: {");
                emit_line(out, depth + 1, s, "Color.gray.opacity(0.1)");
                emit_line(out, depth, s, "}");
            } else {
                emit_line(out, depth, s, "Image(" + swift_string_literal(*asset_id) + ")");
                emit_line(out, depth + 1, s, ".resizable()");
                emit_line(out, depth + 1, s, ".scaledToFit()");
                if (inline_uri)
                    push_fidelity(ctx, node, "swiftui-inline-asset",
                                  "image asset '" + *asset_id + "' is an inline/data URI with no "
                                  "catalog name; Image(\"" + *asset_id + "\") will not resolve "
                                  "until the bytes are extracted into the asset catalog",
                                  /*informational=*/false);
                else
                    push_fidelity(ctx, node, "swiftui-bundled-asset",
                                  "image references asset '" + *asset_id + "'; add it to the app's "
                                  "asset catalog under that name (xcassets generation is deferred)",
                                  /*informational=*/true);
            }
            emit_modifiers(out, ctx, node, depth);
            return;
        }
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

        const auto& ly = node.layout;
        const bool row = ly.direction == LayoutDirection::row;

        // CSS grid → SwiftUI LazyVGrid (B5). Map the column COUNT onto equal
        // flexible GridItems; exact fr/px/minmax track sizing and explicit
        // row/column placement are approximated (informational, not a hard
        // divergence — the grid renders). iOS16+/macOS13+ floor (LazyVGrid is
        // actually 14+/11+, well within it).
        const bool is_grid = (ly.display && *ly.display == "grid") ||
                             ly.grid_template_columns || ly.grid_template_rows;
        if (is_grid) {
            const int cols = ly.grid_template_columns
                                 ? grid_column_count(*ly.grid_template_columns) : 1;
            const float col_gap = ly.column_gap.value_or(ly.gap);
            const float row_gap = ly.row_gap.value_or(ly.gap);
            std::string cols_expr;
            for (int c = 0; c < cols; ++c) {
                cols_expr += (c ? ", " : "") + std::string("GridItem(.flexible()");
                if (col_gap > 0.0f) cols_expr += ", spacing: " + format_float(col_gap);
                cols_expr += ")";
            }
            emit_line(out, depth, s, "LazyVGrid(columns: [" + cols_expr +
                                     "], spacing: " + format_float(row_gap) + ") {");
            if (child_count == 0) emit_line(out, depth + 1, s, "EmptyView()");
            else emit_children(out, ctx, node, resolved, depth + 1);
            emit_line(out, depth, s, "}");
            emit_modifiers(out, ctx, node, depth);
            if (node.style.position && *node.style.position == "absolute") {
                emit_line(out, depth + 1, s,
                          ".offset(x: " + format_float(node.style.left.value_or(0.0f)) +
                          ", y: " + format_float(node.style.top.value_or(0.0f)) + ")");
                push_fidelity(ctx, node, "swiftui-absolute-position",
                              "position:absolute approximated with .offset from natural position",
                              /*informational=*/false);
            }
            push_fidelity(ctx, node, "swiftui-grid-tracks",
                          "grid mapped to " + std::to_string(cols) + " equal flexible column(s); "
                          "exact fr/px/minmax track sizing approximated",
                          /*informational=*/true);
            // Explicit per-item placement (grid-column/grid-row) is genuinely
            // lost — LazyVGrid auto-flows children in source order — so it is a
            // HARD divergence, not the advisory track-sizing note above.
            bool has_placement = false;
            for (std::size_t i = 0; i < child_count; ++i) {
                const auto& cl = node.children[i].layout;
                if ((cl.grid_column && !cl.grid_column->empty()) ||
                    (cl.grid_row && !cl.grid_row->empty())) { has_placement = true; break; }
            }
            if (has_placement)
                push_fidelity(ctx, node, "swiftui-grid-placement",
                              "explicit grid item placement (grid-column/grid-row) is dropped; "
                              "LazyVGrid auto-flows children in source order",
                              /*informational=*/false);
            return;
        }
        if (ly.wrap)
            push_fidelity(ctx, node, "swiftui-flex-wrap",
                          "flex-wrap has no SwiftUI HStack/VStack equivalent; children "
                          "stay on one axis", /*informational=*/false);

        bool stretch = false;
        const std::string align_tok = stack_alignment_token(row, ly.align, &stretch);
        if (stretch)
            push_fidelity(ctx, node, "swiftui-align-stretch",
                          "align-items:stretch can't fill the cross axis of a content-sized "
                          "SwiftUI stack; using center", /*informational=*/true);

        const char* stack = row ? "HStack" : "VStack";
        std::string open = std::string(stack) + "(";
        if (align_tok != ".center") open += "alignment: " + align_tok + ", ";
        open += "spacing: " + format_float(ly.gap) + ") {";
        emit_line(out, depth, s, open);

        // Main-axis distribution. SwiftUI stacks pack content; space-between/
        // space-around/flex-end are approximated with Spacers when the subview
        // count stays within the ViewBuilder arity limit, else flagged only.
        const LayoutAlign j = ly.justify;
        const bool wants_spacers = j == LayoutAlign::space_between ||
                                   j == LayoutAlign::space_around ||
                                   j == LayoutAlign::flex_end;
        std::size_t subviews = child_count;
        // space-between with N>=2 → N-1 interior Spacers; with exactly 1 child it
        // is flex-start, approximated with a single trailing Spacer.
        if (j == LayoutAlign::space_between)
            subviews += (child_count >= 2 ? child_count - 1 : (child_count == 1 ? 1 : 0));
        else if (j == LayoutAlign::space_around) subviews += child_count + 1;
        else if (j == LayoutAlign::flex_end) subviews += 1;

        if (child_count == 0) {
            emit_line(out, depth + 1, s, "EmptyView()");
        } else if (wants_spacers && subviews <= 10) {
            emit_children_distributed(out, ctx, node, resolved, depth + 1, j);
            push_fidelity(ctx, node, "swiftui-flex-justify",
                          std::string("justify-content approximated with Spacers; exact ") +
                          "distribution depends on the parent giving the stack free space",
                          /*informational=*/true);
        } else {
            if (wants_spacers)
                push_fidelity(ctx, node, "swiftui-flex-justify",
                              "justify-content distribution dropped: too many children for "
                              "Spacer interposition within the ViewBuilder arity limit",
                              /*informational=*/false);
            emit_children(out, ctx, node, resolved, depth + 1);
        }
        emit_line(out, depth, s, "}");
        emit_modifiers(out, ctx, node, depth);

        // Absolute positioning: CSS anchors the top-left of the box at (left,
        // top); SwiftUI has no flow-relative absolute layout. `.offset` shifts
        // from the natural position — a close-but-not-equivalent approximation.
        if (node.style.position && *node.style.position == "absolute") {
            const float x = node.style.left.value_or(0.0f);
            const float y = node.style.top.value_or(0.0f);
            emit_line(out, depth + 1, s,
                      ".offset(x: " + format_float(x) + ", y: " + format_float(y) + ")");
            push_fidelity(ctx, node, "swiftui-absolute-position",
                          "position:absolute approximated with .offset from natural position; "
                          "SwiftUI has no flow-relative absolute layout", /*informational=*/false);
        }
        return;
    }

    // Unsupported leaf widget: keep the footprint with a clear rectangle.
    if (ctx.opts.include_comments)
        emit_line(out, depth, s, "// " + std::string(native_widget_kind_name(kind))
                                      + " has no SwiftUI exporter; rendered as Color.clear");
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
    SwiftEmitCtx ctx{opts, ir.asset_manifest, opts.fidelity_report};
    emit_node(out, ctx, ir.root, resolved, 2);
    out << "    }\n";
    out << "}\n";
    return out.str();
}

// ── SwiftUI binding manifest (B4 — parity with the C++ manifest) ─────────
// The SwiftUI manifest carries the same per-entry binding-contract fields the
// C++ path emits (render_binding_manifest_entry in design_cpp_codegen.cpp), so
// a SwiftUI host gets the same data, PLUS the SwiftUI-specific resolution block
// (B1's exact-`PulpParameter.name` strategy — Pulp's state model has no stable
// string key, so bound controls resolve by display name). An entry is emitted
// for any node the C++ manifest would include (carries `pulp*` binding
// metadata) OR any resolvable bound widget — the union gives the host both the
// full contract and the name-resolution pre-flight. The field set is asserted
// to match the C++ manifest by a cross-check test (test_design_swift_codegen).

// The same manifest-eligibility predicate the C++ path uses (the `pulp*`
// contract keys). Kept in sync with node_has_binding_manifest_metadata in
// design_cpp_codegen.cpp; the cross-check parity test fails if the two drift.
bool swift_node_has_binding_metadata(const IRNode& node) {
    for (std::string_view key : {
             "pulpRouteId", "pulpRouteType", "pulpSourceFamily", "pulpSourcePath",
             "pulpParamKey", "pulpBindingModule", "pulpBindingParam", "pulpChoiceValue",
             "pulpChoiceLabel", "pulpParamKeyX", "pulpParamKeyY", "pulpBindingModuleX",
             "pulpBindingParamX", "pulpBindingModuleY", "pulpBindingParamY", "pulpMeterSource",
             "pulpMeterChannel", "pulpMeterValueKey", "pulpWaveformShape", "pulpValueKey",
             "pulpInitialValue", "pulpPlaceholder", "pulpFocusContract", "pulpPayloadContract",
             "pulpHostActionLabel", "pulpTypeLabel", "pulpDescription", "pulpEventContract",
             "pulpGestureContract", "pulpHostAction", "pulpStyleTokens",
             "pulpDefaultValueSource", "pulpFallbackReason",
         }) {
        auto it = node.attributes.find(std::string(key));
        if (it != node.attributes.end() && !it->second.empty()) return true;
    }
    return false;
}

void swift_append_json_field(std::ostringstream& out, bool& first,
                             std::string_view key, const std::optional<std::string>& value) {
    if (!value || value->empty()) return;
    out << (first ? "" : ", ") << "\"" << key << "\": \"" << json_string_escape(*value) << "\"";
    first = false;
}

struct SwiftBindingEntry {
    std::string ir_path;
    const IRNode* node = nullptr;
    NativeWidgetKind kind = NativeWidgetKind::view;
    bool bound = false;  // a name-resolvable control (gets the resolve_name block)
};

void collect_swift_bindings(const IRNode& node, const ResolvedNativeNode& resolved,
                            std::string_view ir_path, std::vector<SwiftBindingEntry>& out) {
    const bool bound = is_bound_widget(resolved.kind) && !binding_resolve_name(node).empty();
    if (bound || swift_node_has_binding_metadata(node))
        out.push_back({std::string(ir_path), &node, resolved.kind, bound});
    const std::size_t count = std::min(node.children.size(), resolved.children.size());
    for (std::size_t i = 0; i < count; ++i)
        collect_swift_bindings(node.children[i], resolved.children[i],
                               std::string(ir_path) + "/" + std::to_string(i), out);
}

std::string emit_binding_manifest(const IRNode& root, const ResolvedNativeNode& resolved) {
    std::vector<SwiftBindingEntry> entries;
    collect_swift_bindings(root, resolved, "root", entries);

    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"pulp-native-swiftui-binding-manifest-v1\",\n";
    out << "  \"resolution\": { \"strategy\": \"pulp_parameter_name_exact\", "
           "\"source_field\": \"resolve_name\" },\n";
    // The convention the generated controls honour (mirrors PulpViews.swift):
    // gesture-grouped writes (beginGesture/endGesture), normalized 0…1 range
    // mapped to [minValue, maxValue], and host-automation pickup via poll().
    out << "  \"conventions\": { \"gesture_grouping\": true, "
           "\"value_range\": \"normalized_0_1\", \"automation\": \"poll\" },\n";
    out << "  \"entries\": [";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        const IRNode& node = *e.node;
        const auto md = NativeBindingMetadata::parse(node);
        out << (i == 0 ? "\n" : ",\n") << "    {";
        bool first = true;
        // id: route_id ?: anchor ?: name (matches the C++ entry's id precedence).
        if (md.route_id && !md.route_id->empty())
            swift_append_json_field(out, first, "id", md.route_id);
        else if (node.stable_anchor_id && !node.stable_anchor_id->empty())
            swift_append_json_field(out, first, "id", node.stable_anchor_id);
        else if (!node.name.empty())
            swift_append_json_field(out, first, "id", std::optional<std::string>(node.name));
        swift_append_json_field(out, first, "ir_path", std::optional<std::string>(e.ir_path));
        if (node.stable_anchor_id && !node.stable_anchor_id->empty())
            swift_append_json_field(out, first, "anchor_id", node.stable_anchor_id);
        swift_append_json_field(out, first, "native_primitive",
                                std::optional<std::string>(native_widget_kind_name(e.kind)));
        // Full binding-contract fields — same names AND order as the C++
        // manifest, emitted contiguously right after native_primitive so the
        // shared field sequence is identical (the SwiftUI-only resolution
        // fields are appended AFTER this run, below).
        swift_append_json_field(out, first, "route_type", md.route_type);
        swift_append_json_field(out, first, "source_family", md.source_family);
        swift_append_json_field(out, first, "source_path", md.source_path);
        swift_append_json_field(out, first, "param_key", md.param_key);
        swift_append_json_field(out, first, "binding_module", md.binding_module);
        swift_append_json_field(out, first, "binding_param", md.binding_param);
        swift_append_json_field(out, first, "choice_value", md.choice_value);
        swift_append_json_field(out, first, "choice_label", md.choice_label);
        swift_append_json_field(out, first, "x_param_key", md.x_param_key);
        swift_append_json_field(out, first, "y_param_key", md.y_param_key);
        swift_append_json_field(out, first, "x_binding_module", md.x_binding_module);
        swift_append_json_field(out, first, "x_binding_param", md.x_binding_param);
        swift_append_json_field(out, first, "y_binding_module", md.y_binding_module);
        swift_append_json_field(out, first, "y_binding_param", md.y_binding_param);
        swift_append_json_field(out, first, "meter_source", md.meter_source);
        swift_append_json_field(out, first, "meter_channel", md.meter_channel);
        swift_append_json_field(out, first, "meter_value_key", md.meter_value_key);
        swift_append_json_field(out, first, "waveform_shape", md.waveform_shape);
        swift_append_json_field(out, first, "value_key", md.value_key);
        swift_append_json_field(out, first, "initial_value", md.initial_value);
        swift_append_json_field(out, first, "placeholder", md.placeholder);
        swift_append_json_field(out, first, "focus_contract", md.focus_contract);
        swift_append_json_field(out, first, "payload_contract", md.payload_contract);
        swift_append_json_field(out, first, "host_action_label", md.host_action_label);
        swift_append_json_field(out, first, "component_type_label", md.type_label);
        swift_append_json_field(out, first, "description", md.description);
        swift_append_json_field(out, first, "thumb_shape", md.thumb_shape);
        swift_append_json_field(out, first, "thumb_width", md.thumb_width);
        swift_append_json_field(out, first, "thumb_height", md.thumb_height);
        swift_append_json_field(out, first, "thumb_corner_radius", md.thumb_corner_radius);
        swift_append_json_field(out, first, "on_background_color", md.on_background_color);
        swift_append_json_field(out, first, "off_background_color", md.off_background_color);
        swift_append_json_field(out, first, "on_text_color", md.on_text_color);
        swift_append_json_field(out, first, "off_text_color", md.off_text_color);
        swift_append_json_field(out, first, "on_border_color", md.on_border_color);
        swift_append_json_field(out, first, "off_border_color", md.off_border_color);
        swift_append_json_field(out, first, "corner_radius", md.corner_radius);
        swift_append_json_field(out, first, "font_size", md.font_size);
        swift_append_json_field(out, first, "event_contract", md.event_contract);
        swift_append_json_field(out, first, "gesture_contract", md.gesture_contract);
        swift_append_json_field(out, first, "host_action", md.host_action);
        swift_append_json_field(out, first, "style_tokens", md.style_tokens);
        swift_append_json_field(out, first, "default_value_source", md.default_value_source);
        swift_append_json_field(out, first, "fallback_reason", md.fallback_reason);
        // SwiftUI-only resolution (name-exact) for resolvable controls — appended
        // after the shared C++ field run so the parity fields stay contiguous.
        if (e.bound) {
            swift_append_json_field(out, first, "resolve_name",
                                    std::optional<std::string>(binding_resolve_name(node)));
            swift_append_json_field(out, first, "resolution_strategy",
                                    std::optional<std::string>("pulp_parameter_name_exact"));
        }
        out << "}";
    }
    out << (entries.empty() ? "" : "\n  ") << "]\n";
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
