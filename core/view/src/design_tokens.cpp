// design_tokens.cpp — design-token import/export, extracted from
// design_import.cpp in the 2026-05 Phase 6 (A3) refactor.
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

Theme parse_w3c_tokens(const std::string& json) {
    Theme theme;
    auto root = choc::json::parse(json);

    // W3C/DTCG Design Tokens Format:
    // - Top-level groups with $type (inherited by children)
    // - Nested tokens with $value/$type
    // - Alias references: { "$value": "{color.primary}" } resolve to other tokens
    // - $description stored but not used in Theme (available for documentation)

    // Evaluate simple math expressions: "8 * 2" → "16", "{spacing.base} * 2" after alias resolution
    auto eval_math = [](const std::string& expr) -> std::string {
        // Only handle simple "number op number" patterns
        auto trim = [](const std::string& s) -> std::string {
            auto a = s.find_first_not_of(" \t");
            auto b = s.find_last_not_of(" \t");
            return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
        };
        std::string s = trim(expr);
        // Strip trailing units (px, rem, em)
        std::string unit;
        for (auto& u : {"px", "rem", "em"}) {
            if (s.size() > 2 && s.substr(s.size() - std::strlen(u)) == u) {
                unit = u;
                s = trim(s.substr(0, s.size() - std::strlen(u)));
                break;
            }
        }
        // Find operator
        for (char op : {'*', '+', '-', '/'}) {
            auto pos = s.find(op);
            if (pos != std::string::npos && pos > 0 && pos < s.size() - 1) {
                auto a = parse_design_number(trim(s.substr(0, pos)));
                auto b = parse_design_number(trim(s.substr(pos + 1)));
                if (!a || !b) continue;
                float result = 0;
                if (op == '*') result = *a * *b;
                else if (op == '+') result = *a + *b;
                else if (op == '-') result = *a - *b;
                else if (op == '/' && *b != 0) result = *a / *b;
                // Return as clean number string
                if (result == std::floor(result))
                    return std::to_string(static_cast<int>(result));
                std::ostringstream oss;
                oss << result;
                return oss.str();
            }
        }
        return expr;  // Not a math expression, return as-is
    };

    // First pass: collect all raw token values (including unresolved aliases)
    struct RawToken { std::string type; std::string value; };
    std::unordered_map<std::string, RawToken> raw_tokens;

    std::function<void(const choc::value::ValueView&, const std::string&, const std::string&)> walk;
    walk = [&](const choc::value::ValueView& obj, const std::string& prefix,
               const std::string& inherited_type) {
        if (!obj.isObject()) return;

        // Group-level $type applies to all children without their own $type
        auto group_type = get_string(obj, "$type", "");
        if (group_type.empty()) group_type = inherited_type;

        for (uint32_t i = 0; i < obj.size(); ++i) {
            auto member = obj.getObjectMemberAt(i);
            std::string key(member.name);
            if (key.empty() || key[0] == '$') continue;

            auto& val = member.value;
            std::string full_name = prefix.empty() ? key : prefix + "." + key;

            // Leaf token: has $value
            if (val.isObject() && val.hasObjectMember("$value")) {
                auto type_str = get_string(val, "$type", "");
                if (type_str.empty()) type_str = group_type;

                auto dollar_val = val["$value"];

                // Composite tokens: typography, shadow, border have object $value
                if (dollar_val.isObject()) {
                    if (type_str == "typography") {
                        // Flatten: typography.fontFamily, typography.fontSize, etc.
                        if (dollar_val.hasObjectMember("fontFamily"))
                            raw_tokens[full_name + ".fontFamily"] = { "fontFamily", std::string(dollar_val["fontFamily"].toString()) };
                        if (dollar_val.hasObjectMember("fontSize"))
                            raw_tokens[full_name + ".fontSize"] = { "dimension", std::string(dollar_val["fontSize"].toString()) };
                        if (dollar_val.hasObjectMember("fontWeight"))
                            raw_tokens[full_name + ".fontWeight"] = { "number", std::string(dollar_val["fontWeight"].toString()) };
                        if (dollar_val.hasObjectMember("lineHeight"))
                            raw_tokens[full_name + ".lineHeight"] = { "number", std::string(dollar_val["lineHeight"].toString()) };
                        if (dollar_val.hasObjectMember("letterSpacing"))
                            raw_tokens[full_name + ".letterSpacing"] = { "dimension", std::string(dollar_val["letterSpacing"].toString()) };
                    } else if (type_str == "shadow") {
                        // Flatten shadow: offsetX, offsetY, blur, spread, color
                        if (dollar_val.hasObjectMember("color"))
                            raw_tokens[full_name + ".color"] = { "color", std::string(dollar_val["color"].toString()) };
                        if (dollar_val.hasObjectMember("offsetX"))
                            raw_tokens[full_name + ".offsetX"] = { "dimension", std::string(dollar_val["offsetX"].toString()) };
                        if (dollar_val.hasObjectMember("offsetY"))
                            raw_tokens[full_name + ".offsetY"] = { "dimension", std::string(dollar_val["offsetY"].toString()) };
                        if (dollar_val.hasObjectMember("blur"))
                            raw_tokens[full_name + ".blur"] = { "dimension", std::string(dollar_val["blur"].toString()) };
                        if (dollar_val.hasObjectMember("spread"))
                            raw_tokens[full_name + ".spread"] = { "dimension", std::string(dollar_val["spread"].toString()) };
                    } else if (type_str == "border") {
                        if (dollar_val.hasObjectMember("color"))
                            raw_tokens[full_name + ".color"] = { "color", std::string(dollar_val["color"].toString()) };
                        if (dollar_val.hasObjectMember("width"))
                            raw_tokens[full_name + ".width"] = { "dimension", std::string(dollar_val["width"].toString()) };
                        if (dollar_val.hasObjectMember("style"))
                            raw_tokens[full_name + ".style"] = { "string", std::string(dollar_val["style"].toString()) };
                    } else {
                        // Unknown composite — store as string
                        raw_tokens[full_name] = { type_str, choc::json::toString(dollar_val) };
                    }
                } else {
                    // Simple scalar $value
                    auto value_str = std::string(dollar_val.toString());
                    raw_tokens[full_name] = { type_str, value_str };
                }
            } else if (val.isObject()) {
                // Nested group — recurse with inherited type
                walk(val, full_name, group_type);
            }
        }
    };

    walk(root, "", "");

    // Second pass: resolve aliases (values like "{color.primary}" or "{spacing.base} * 2")
    // Handles both whole-value aliases and partial aliases embedded in expressions
    // Supports up to 10 levels of chained aliases to prevent infinite loops
    auto resolve_value = [&](const std::string& value) -> std::string {
        std::string resolved = value;
        std::unordered_set<std::string> visited;  // Cycle detection
        for (int depth = 0; depth < 10; ++depth) {
            // Find any {reference.path} pattern in the string
            auto open = resolved.find('{');
            auto close = resolved.find('}');
            if (open == std::string::npos || close == std::string::npos || close <= open)
                break;

            auto ref = resolved.substr(open + 1, close - open - 1);
            if (visited.count(ref))
                break;  // Circular alias — stop resolving
            visited.insert(ref);

            auto it = raw_tokens.find(ref);
            if (it != raw_tokens.end()) {
                // Replace {ref} with resolved value
                resolved = resolved.substr(0, open) + it->second.value + resolved.substr(close + 1);
                continue;  // May contain more aliases
            }
            break;  // Reference not found
        }
        return resolved;
    };

    // Third pass: resolve aliases, evaluate math, store into Theme
    for (auto& [name, token] : raw_tokens) {
        auto value_str = eval_math(resolve_value(token.value));
        auto& type_str = token.type;

        if (type_str == "color") {
            if (!value_str.empty() && value_str[0] == '#') {
                theme.colors[name] = parse_hex_color_str(value_str);
            }
        } else if (type_str == "dimension") {
            if (auto v = parse_design_number(value_str)) theme.dimensions[name] = *v;
        } else if (type_str == "fontFamily" || type_str == "string") {
            theme.strings[name] = value_str;
        } else if (type_str == "number") {
            if (auto v = parse_design_number(value_str)) theme.dimensions[name] = *v;
        } else {
            // Infer type from resolved value
            if (!value_str.empty() && value_str[0] == '#') {
                theme.colors[name] = parse_hex_color_str(value_str);
            } else {
                if (auto v = parse_design_number(value_str)) {
                    theme.dimensions[name] = *v;
                } else {
                    theme.strings[name] = value_str;
                }
            }
        }
    }

    return theme;
}

std::string export_w3c_tokens(const Theme& theme) {
    std::ostringstream ss;
    ss << "{\n";

    // Group tokens by prefix (before the dot)
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> color_groups;
    for (auto& [name, color] : theme.colors) {
        auto dot = name.find('.');
        std::string group = dot != std::string::npos ? name.substr(0, dot) : "color";
        std::string key = dot != std::string::npos ? name.substr(dot + 1) : name;

        char buf[10];
        if (color.a8() == 255)
            snprintf(buf, sizeof(buf), "#%02x%02x%02x", color.r8(), color.g8(), color.b8());
        else
            snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", color.r8(), color.g8(), color.b8(), color.a8());

        color_groups[group].emplace_back(key, buf);
    }

    std::map<std::string, std::vector<std::pair<std::string, float>>> dim_groups;
    for (auto& [name, value] : theme.dimensions) {
        auto dot = name.find('.');
        std::string group = dot != std::string::npos ? name.substr(0, dot) : "dimension";
        std::string key = dot != std::string::npos ? name.substr(dot + 1) : name;
        dim_groups[group].emplace_back(key, value);
    }

    bool first_group = true;

    // Colors
    for (auto& [group, entries] : color_groups) {
        if (!first_group) ss << ",\n";
        first_group = false;
        ss << "  \"" << group << "\": {\n";
        bool first = true;
        for (auto& [key, hex] : entries) {
            if (!first) ss << ",\n";
            first = false;
            ss << "    \"" << key << "\": { \"$value\": \"" << hex << "\", \"$type\": \"color\" }";
        }
        ss << "\n  }";
    }

    // Dimensions
    for (auto& [group, entries] : dim_groups) {
        if (!first_group) ss << ",\n";
        first_group = false;
        ss << "  \"" << group << "\": {\n";
        bool first = true;
        for (auto& [key, val] : entries) {
            if (!first) ss << ",\n";
            first = false;
            ss << "    \"" << key << "\": { \"$value\": \"" << val << "\", \"$type\": \"dimension\" }";
        }
        ss << "\n  }";
    }

    // Strings
    if (!theme.strings.empty()) {
        if (!first_group) ss << ",\n";
        ss << "  \"string\": {\n";
        bool first = true;
        for (auto& [name, value] : theme.strings) {
            if (!first) ss << ",\n";
            first = false;
            auto dot = name.find('.');
            std::string key = dot != std::string::npos ? name.substr(dot + 1) : name;
            ss << "    \"" << key << "\": { \"$value\": \"" << value << "\", \"$type\": \"string\" }";
        }
        ss << "\n  }";
    }

    ss << "\n}\n";
    return ss.str();
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
    auto css_var = [&](const std::string& name) {
        // Strip the .dark suffix, then map "." → "-" for the custom-property id.
        std::string base = is_dark(name) ? name.substr(0, name.size() - dark_suffix.size()) : name;
        std::string out = "--";
        for (char c : base) out += (c == '.') ? '-' : c;
        return out;
    };
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
    ss << "/* Generated by pulp import-design --token-format css */\n";
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

Theme ir_tokens_to_theme(const IRTokens& tokens) {
    Theme theme;
    for (auto& [name, hex] : tokens.colors)
        theme.colors[name] = parse_hex_color_str(hex);
    theme.dimensions = tokens.dimensions;
    theme.strings = tokens.strings;
    return theme;
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
