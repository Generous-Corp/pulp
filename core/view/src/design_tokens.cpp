// design_tokens.cpp — design-token import/export extracted from
// design_import.cpp.
//
// The design-token surface converts between Pulp's Theme and the three
// external token formats Pulp interoperates with:
//
//   * W3C Design Tokens          — parse_w3c_tokens / export_w3c_tokens
//   * Figma Variables            — parse_figma_variables / export_figma_variables
//   * Stitch Design System       — parse_stitch_design_system / export_stitch_design_system
//
// Plus the Theme ⇄ IRTokens bridge (ir_tokens_to_theme / theme_to_ir_tokens).
//
// These functions are declared in pulp/view/design_import.hpp; this TU
// only relocates their definitions out of the 4.7k-line design_import.cpp
// so token-format work no longer recompiles the whole importer.

#include <pulp/view/design_import.hpp>

#include <pulp/view/css_gradient.hpp>

#include <choc/text/choc_JSON.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_set>

namespace pulp::view {
namespace {

// JSON object-field accessors — duplicated from design_import.cpp's
// "JSON parsing helpers" block so this TU stands alone (the originals
// are file-local statics; the token parsers only need string + float).
std::string get_string(const choc::value::ValueView& obj, const char* key, const char* def = "") {
    if (obj.hasObjectMember(key))
        return std::string(obj[key].toString());
    return def;
}

float get_float(const choc::value::ValueView& obj, const char* key, float def = 0.0f) {
    if (obj.hasObjectMember(key))
        return static_cast<float>(obj[key].getWithDefault<double>(def));
    return def;
}

}  // namespace

static Color parse_hex_color_str(const std::string& hex) {
    if (hex.empty() || hex[0] != '#') return {};
    try {
        auto val = std::stoul(hex.substr(1), nullptr, 16);
        if (hex.size() == 7)
            return color_from_hex(static_cast<uint32_t>(val));
        if (hex.size() == 9)
            return color_from_hex_alpha(static_cast<uint32_t>(val));
    } catch (...) {}
    return {};
}

static std::optional<float> parse_design_number(std::string value) {
    auto trim = [](std::string s) {
        auto a = s.find_first_not_of(" \t\r\n");
        auto b = s.find_last_not_of(" \t\r\n");
        return (a == std::string::npos) ? std::string{} : s.substr(a, b - a + 1);
    };

    value = trim(std::move(value));
    for (std::string_view suffix : {"px", "rem", "em", "%"}) {
        if (value.size() > suffix.size() &&
            value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0) {
            value = trim(value.substr(0, value.size() - suffix.size()));
            break;
        }
    }
    if (value.empty()) return std::nullopt;

    char* end = nullptr;
    float parsed = std::strtof(value.c_str(), &end);
    if (end == value.c_str()) return std::nullopt;
    while (*end != '\0') {
        if (!std::isspace(static_cast<unsigned char>(*end))) return std::nullopt;
        ++end;
    }
    if (!std::isfinite(parsed)) return std::nullopt;
    return parsed;
}

std::string token_css_var(const std::string& token_name) {
    static const std::string dark_suffix = ".dark";
    const bool dark = token_name.size() > dark_suffix.size() &&
                      token_name.compare(token_name.size() - dark_suffix.size(),
                                         dark_suffix.size(), dark_suffix) == 0;
    std::string base = dark ? token_name.substr(0, token_name.size() - dark_suffix.size())
                            : token_name;
    std::string out = "--";
    for (char c : base) out += (c == '.') ? '-' : c;
    return out;
}

std::string export_css_variables(const Theme& theme) {
    // Dark-mode tokens carry the ".dark" multi-mode suffix (Figma plugin +
    // DESIGN.md body parser). Partition base vs dark; base → :root, dark
    // overrides → @media (prefers-color-scheme: dark). std::map keeps output
    // deterministic (sorted by custom-property name).
    const std::string dark_suffix = ".dark";
    auto is_dark = [&](const std::string& n) {
        return n.size() > dark_suffix.size()
            && n.compare(n.size() - dark_suffix.size(), dark_suffix.size(), dark_suffix) == 0;
    };
    auto css_var = [&](const std::string& name) { return token_css_var(name); };
    auto hex = [](const Color& c) {
        char buf[10];
        if (c.a8() == 255)
            snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r8(), c.g8(), c.b8());
        else
            snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", c.r8(), c.g8(), c.b8(), c.a8());
        return std::string(buf);
    };

    std::map<std::string, std::string> base, dark;
    for (auto& [name, color] : theme.colors)
        (is_dark(name) ? dark : base)[css_var(name)] = hex(color);
    for (auto& [name, value] : theme.dimensions) {
        std::ostringstream v;
        v << value << "px";
        (is_dark(name) ? dark : base)[css_var(name)] = v.str();
    }
    for (auto& [name, value] : theme.strings)
        (is_dark(name) ? dark : base)[css_var(name)] = value;

    std::ostringstream ss;
    ss << "/* Generated by pulp import-design --format css-variables */\n";
    ss << ":root {\n";
    for (auto& [k, v] : base) ss << "  " << k << ": " << v << ";\n";
    ss << "}\n";
    if (!dark.empty()) {
        ss << "\n@media (prefers-color-scheme: dark) {\n  :root {\n";
        for (auto& [k, v] : dark) ss << "    " << k << ": " << v << ";\n";
        ss << "  }\n}\n";
    }
    return ss.str();
}

namespace {

/// Parse one design colour token, or nothing.
///
/// A captured design states its colours in CSS, so a token is as likely to be
/// `rgba(60,50,30,0.18)` as `#C4622A`. parse_hex_color_str understands only
/// hex and answers a default-constructed (transparent black) Color for the
/// rest, which paints as BLACK — and the knob path never saw it because it
/// resolves the same tokens through parse_css_color. One design, two parsers,
/// two answers.
///
/// parse_css_color is that one parser, but it returns opaque WHITE for a token
/// it does not recognize, which is a colour from nowhere in the same way a
/// fallback palette is. So recognize the forms it handles first and answer
/// nothing for anything else: an unparseable token leaves its widget key unset
/// and reported, rather than painting white or black.
std::optional<Color> parse_design_color(const std::string& token) {
    if (token.empty()) return std::nullopt;
    if (token == "transparent") return parse_css_color(token);
    if (token[0] == '#') {
        // parse_css_color leaves any component it cannot read at its default,
        // so a truncated hex would come back part-white. Only the three
        // complete forms are a colour.
        if (token.size() != 4 && token.size() != 7 && token.size() != 9)
            return std::nullopt;
        if (token.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos)
            return std::nullopt;
        return parse_css_color(token);
    }
    for (std::string_view fn : {"rgb(", "rgba(", "hsl(", "hsla("}) {
        if (token.compare(0, fn.size(), fn) == 0 &&
            token.find(')') != std::string::npos)
            return parse_css_color(token);
    }
    // Named colours, oklab(), colour-mix() and anything else this parser does
    // not model. Refusing is the honest answer; guessing is not.
    return std::nullopt;
}

/// One widget theme key and the design tokens that may supply it, most
/// specific first. Sources are tried in order; the first that the design
/// actually states wins.
struct WidgetTokenRule {
    const char* widget;
    std::vector<const char*> sources;
};

const std::vector<WidgetTokenRule>& widget_token_rules() {
    // The design → widget colour map, in one place, as data.
    //
    // Tokens are copied into the theme by NAME, so a design that says `accent`
    // has no `control.fill` — and Fader::paint resolves `control.fill`, misses,
    // and paints its built-in blue. An imported design's palette reached its
    // panels and text but never its primitives: a blue fader and a green meter
    // on a cream-and-rust faceplate.
    //
    // Each rule is a widget key and the design tokens that MEAN the same thing,
    // most specific first. Every entry is a direct copy — no hue shift, no
    // blend, no built-in default. theme_presets.cpp derives a built-in theme's
    // meter zones by rotating the accent's hue; doing that here would invent a
    // colour the design never chose. A widget key whose sources are all absent
    // therefore stays UNSET and is reported: the widget's own default is an
    // honest "the design did not say", while a synthesized colour is a second
    // palette that silently drifts from the first.
    //
    // Three source vocabularies are accepted so a design need not know Pulp's:
    // the Pulp design-system semantic names (`accent`, `line-strong`,
    // `signal-low` — shared by every pack), the shadcn-style names the Figma /
    // Stitch / W3C importers emit (`primary`, `muted`, `foreground`), and the
    // Material spelling (`outline`, `on-surface`).
    static const std::vector<WidgetTokenRule> rules = {
        // Accents. `danger` / `warning` / `success` / `info` are stated
        // outright by the design; none of them is derived.
        {"accent.primary",   {"primary", "accent"}},
        {"accent.secondary", {"secondary", "accent-soft"}},
        {"accent.error",     {"destructive", "danger"}},
        {"accent.warning",   {"warning"}},
        {"accent.success",   {"success"}},
        {"accent.info",      {"info"}},

        // Controls, knobs and sliders share one shape: an empty track, a
        // filled value portion, and a mark that moves. The knob path already
        // resolved that shape against this design (browser_capture_ir.cpp
        // lowers `css/accent` / `css/line-strong` / `css/text-strong` onto a
        // control's design_accent / design_track / design_indicator
        // attributes), so the fader and the meter answer to the same three
        // tokens the knob does. Two paths reading one source is the point —
        // giving the fader its own answer is how they drift apart.
        {"control.fill",   {"primary", "accent"}},
        {"control.track",  {"line-strong", "muted", "outline"}},
        {"control.thumb",  {"text-strong", "foreground", "on-surface"}},
        {"control.border", {"border", "control-line", "outline"}},

        {"knob.arc",    {"primary", "accent"}},
        {"knob.arc.bg", {"line-strong", "muted", "outline"}},
        {"knob.thumb",  {"text-strong", "foreground", "on-surface"}},

        {"slider.track", {"line-strong", "muted", "outline"}},
        {"slider.fill",  {"primary", "accent"}},
        {"slider.thumb", {"text-strong", "foreground", "on-surface"}},

        // A meter's three zones are the design's own signal ramp: the level a
        // signal sits at normally, the level that wants attention, and the
        // level that is too hot. `green` / `yellow` / `red` name Pulp's
        // built-in hues, not required hues — a design whose ramp runs through
        // one colour gets a meter in that one colour, which is what it asked
        // for.
        {"meter.green",  {"signal-low"}},
        {"meter.yellow", {"signal-mid"}},
        {"meter.red",    {"signal-high"}},

        {"waveform.line", {"signal-wave", "primary", "accent"}},

        {"focus.ring", {"accent-ring", "primary", "accent"}},
    };
    return rules;
}

}  // namespace

Theme ir_tokens_to_theme(const IRTokens& tokens,
                         std::vector<std::string>* unresolved_widget_tokens) {
    Theme theme;
    for (auto& [name, value] : tokens.colors) {
        // A token this parser cannot read is left OUT rather than stored as
        // the default black it used to become. A theme entry is a colour the
        // design chose; a parse failure is not one, and storing it as black
        // hands every widget resolving that key a colour from nowhere.
        if (const auto colour = parse_design_color(value))
            theme.colors[name] = *colour;
    }

    // A design captured from CSS names its tokens `css/<custom-property>`; the
    // Figma / Stitch / W3C importers use the bare name. Trying both lets one
    // rule table serve every producer.
    const auto find_source = [&theme](const char* name) -> std::optional<Color> {
        auto it = theme.colors.find(name);
        if (it != theme.colors.end()) return it->second;
        it = theme.colors.find("css/" + std::string(name));
        if (it != theme.colors.end()) return it->second;
        return std::nullopt;
    };

    for (const auto& rule : widget_token_rules()) {
        // Fills only what is ABSENT: a design that names knob.arc itself keeps
        // it, so the more specific instruction never loses to the general one.
        if (theme.colors.count(rule.widget) != 0) continue;
        bool resolved = false;
        for (const char* source : rule.sources) {
            // Copy the value out before inserting — inserting into
            // theme.colors can rehash and invalidate a reference into it.
            if (const auto colour = find_source(source)) {
                theme.colors[rule.widget] = *colour;
                resolved = true;
                break;
            }
        }
        if (!resolved && unresolved_widget_tokens != nullptr)
            unresolved_widget_tokens->emplace_back(rule.widget);
    }

    theme.dimensions = tokens.dimensions;
    theme.strings = tokens.strings;
    return theme;
}

std::optional<ImportDiagnostic> unmapped_widget_token_diagnostic(
    const std::vector<std::string>& unresolved) {
    if (unresolved.empty()) return std::nullopt;
    // One diagnostic naming all of them, not one each: a design with no token
    // set at all leaves every widget key unset, and twenty warnings for one
    // cause is how a reader learns to skip the diagnostics.
    std::string keys;
    for (const auto& key : unresolved) keys += (keys.empty() ? "" : ", ") + key;
    return ImportDiagnostic{
        ImportDiagnosticSeverity::warning,
        "design-token-unmapped",
        "$",
        "the design states no colour for " + std::to_string(unresolved.size()) +
            " widget token(s), so widgets resolving them paint their built-in "
            "default: " + keys,
        ImportDiagnosticKind::fallback_used,
        std::nullopt,
        std::nullopt};
}

IRTokens theme_to_ir_tokens(const Theme& theme) {
    IRTokens tokens;
    for (auto& [name, color] : theme.colors) {
        char buf[10];
        if (color.a8() == 255)
            snprintf(buf, sizeof(buf), "#%02x%02x%02x", color.r8(), color.g8(), color.b8());
        else
            snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", color.r8(), color.g8(), color.b8(), color.a8());
        tokens.colors[name] = buf;
    }
    tokens.dimensions = theme.dimensions;
    tokens.strings = theme.strings;
    return tokens;
}

// ── Figma Variables sync ────────────────────────────────────────────────

Theme parse_figma_variables(const std::string& json) {
    Theme theme;
    auto root = choc::json::parse(json);

    // Figma Variables JSON structure (from MCP get_variable_defs):
    // { "variables": [ { "name": "color/primary", "resolvedValue": "#89B4FA",
    //                     "type": "COLOR" }, ... ],
    //   "collections": [ { "name": "Tokens", "modes": [...] } ] }
    // OR flat array of variables

    auto parse_vars = [&](const choc::value::ValueView& vars) {
        for (uint32_t i = 0; i < vars.size(); ++i) {
            auto v = vars[static_cast<int>(i)];
            auto name = get_string(v, "name", "");
            auto type = get_string(v, "type", "");
            if (name.empty()) continue;

            // Figma uses slash-separated paths: "color/primary" → "color.primary"
            std::string dotted = name;
            for (auto& c : dotted) if (c == '/') c = '.';

            auto resolved = get_string(v, "resolvedValue", "");
            if (resolved.empty() && v.hasObjectMember("value"))
                resolved = get_string(v, "value", "");

            if (type == "COLOR" || type == "color") {
                if (!resolved.empty() && resolved[0] == '#')
                    theme.colors[dotted] = parse_hex_color_str(resolved);
            } else if (type == "FLOAT" || type == "float" || type == "number") {
                if (auto v = parse_design_number(resolved)) theme.dimensions[dotted] = *v;
            } else if (type == "STRING" || type == "string") {
                theme.strings[dotted] = resolved;
            } else {
                // Infer from value
                if (!resolved.empty() && resolved[0] == '#')
                    theme.colors[dotted] = parse_hex_color_str(resolved);
                else {
                    if (auto v = parse_design_number(resolved)) theme.dimensions[dotted] = *v;
                    else theme.strings[dotted] = resolved;
                }
            }
        }
    };

    if (root.isObject() && root.hasObjectMember("variables") && root["variables"].isArray())
        parse_vars(root["variables"]);
    else if (root.isArray())
        parse_vars(root);

    return theme;
}

std::string export_figma_variables(const Theme& theme) {
    std::ostringstream ss;
    ss << "{\n  \"variables\": [\n";

    bool first = true;
    auto emit = [&](const std::string& name, const std::string& type, const std::string& value) {
        if (!first) ss << ",\n";
        first = false;
        // Convert dot-separated to slash-separated for Figma
        std::string figma_name = name;
        for (auto& c : figma_name) if (c == '.') c = '/';
        ss << "    { \"name\": \"" << figma_name << "\", \"type\": \"" << type
           << "\", \"value\": \"" << value << "\" }";
    };

    for (auto& [name, color] : theme.colors) {
        char buf[10];
        if (color.a8() == 255)
            snprintf(buf, sizeof(buf), "#%02x%02x%02x", color.r8(), color.g8(), color.b8());
        else
            snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", color.r8(), color.g8(), color.b8(), color.a8());
        emit(name, "COLOR", buf);
    }
    for (auto& [name, value] : theme.dimensions) {
        std::ostringstream vs;
        if (value == std::floor(value)) vs << static_cast<int>(value);
        else vs << value;
        emit(name, "FLOAT", vs.str());
    }
    for (auto& [name, value] : theme.strings)
        emit(name, "STRING", value);

    ss << "\n  ]\n}\n";
    return ss.str();
}

// ── Stitch Design System sync ──────────────────────────────────────────

Theme parse_stitch_design_system(const std::string& json) {
    Theme theme;
    auto root = choc::json::parse(json);

    // Stitch Design System JSON (from MCP list_design_systems):
    // { "name": "My Theme",
    //   "colors": { "primary": "#89B4FA", "background": "#1E1E2E", ... },
    //   "fonts": { "heading": "Inter", "body": "Roboto" },
    //   "roundness": "medium",
    //   "spacing": 8 }

    if (root.hasObjectMember("colors")) {
        auto colors = root["colors"];
        for (uint32_t i = 0; i < colors.size(); ++i) {
            auto m = colors.getObjectMemberAt(i);
            auto hex = std::string(m.value.toString());
            if (!hex.empty() && hex[0] == '#')
                theme.colors[std::string("color.") + std::string(m.name)] = parse_hex_color_str(hex);
        }
    }

    if (root.hasObjectMember("fonts")) {
        auto fonts = root["fonts"];
        for (uint32_t i = 0; i < fonts.size(); ++i) {
            auto m = fonts.getObjectMemberAt(i);
            theme.strings[std::string("font.") + std::string(m.name)] = std::string(m.value.toString());
        }
    }

    if (root.hasObjectMember("roundness")) {
        auto r = get_string(root, "roundness", "medium");
        float radius = 8.0f;
        if (r == "none") radius = 0;
        else if (r == "small") radius = 4;
        else if (r == "medium") radius = 8;
        else if (r == "large") radius = 16;
        else if (r == "full") radius = 999;
        else { try { radius = std::stof(r); } catch (...) {} }
        theme.dimensions["roundness"] = radius;
    }

    if (root.hasObjectMember("spacing")) {
        theme.dimensions["spacing.base"] = get_float(root, "spacing", 8);
    }

    return theme;
}

std::string export_stitch_design_system(const Theme& theme) {
    std::ostringstream ss;
    ss << "{\n";

    // Colors
    ss << "  \"colors\": {\n";
    bool first = true;
    for (auto& [name, color] : theme.colors) {
        if (!first) ss << ",\n";
        first = false;
        // Strip "color." prefix for Stitch
        auto key = name;
        if (key.substr(0, 6) == "color.") key = key.substr(6);
        char buf[10];
        if (color.a8() == 255)
            snprintf(buf, sizeof(buf), "#%02x%02x%02x", color.r8(), color.g8(), color.b8());
        else
            snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", color.r8(), color.g8(), color.b8(), color.a8());
        ss << "    \"" << key << "\": \"" << buf << "\"";
    }
    ss << "\n  },\n";

    // Fonts
    ss << "  \"fonts\": {\n";
    first = true;
    for (auto& [name, value] : theme.strings) {
        if (name.find("font.") != 0) continue;
        if (!first) ss << ",\n";
        first = false;
        auto key = name.substr(5);
        ss << "    \"" << key << "\": \"" << value << "\"";
    }
    ss << "\n  },\n";

    // Roundness
    float roundness = 8;
    if (theme.dimensions.count("roundness"))
        roundness = theme.dimensions.at("roundness");
    std::string r_name = "medium";
    if (roundness <= 0) r_name = "none";
    else if (roundness <= 4) r_name = "small";
    else if (roundness <= 8) r_name = "medium";
    else if (roundness <= 16) r_name = "large";
    else r_name = "full";
    ss << "  \"roundness\": \"" << r_name << "\",\n";

    // Spacing
    float spacing = 8;
    if (theme.dimensions.count("spacing.base"))
        spacing = theme.dimensions.at("spacing.base");
    ss << "  \"spacing\": " << static_cast<int>(spacing) << "\n";

    ss << "}\n";
    return ss.str();
}


} // namespace pulp::view
