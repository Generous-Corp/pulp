// w3c_tokens.cpp — W3C / DTCG Design Tokens <-> Pulp Theme conversion.
//
// Split out of design_tokens.cpp so the two functions reached at runtime by the
// default WidgetBridge theme API (importDesignTokens / exportDesignTokens) stay
// always-compiled, independent of the PULP_ENABLE_DESIGN_IMPORT-gated authoring
// cluster. Depends only on theme.hpp + choc::json. The small JSON/number helpers
// are intentionally COPIES of the file-local statics in design_tokens.cpp (the
// gated remainder still needs its own copies); duplicate internal-linkage helpers
// across TUs are ODR-safe.

#include <pulp/view/w3c_tokens.hpp>

#include <choc/text/choc_JSON.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulp::view {
namespace {

std::string get_string(const choc::value::ValueView& obj, const char* key, const char* def = "") {
    if (obj.hasObjectMember(key))
        return std::string(obj[key].toString());
    return def;
}

Color parse_hex_color_str(const std::string& hex) {
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

std::optional<float> parse_design_number(std::string value) {
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

}  // namespace

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
    auto resolve_value = [&](const std::string& value) -> std::string {
        std::string resolved = value;
        std::unordered_set<std::string> visited;  // Cycle detection
        for (int depth = 0; depth < 10; ++depth) {
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
    // Group every token by its top-level prefix (text before the first dot).
    // Colors, dimensions, and strings that share a prefix MUST land in one
    // group object: e.g. every derived theme has both `control.*` colors and
    // `control.*` dimensions, and emitting "control" twice produced a duplicate
    // JSON member that parse_w3c_tokens (choc::json) rejects with
    // "object already contains a member". One ordered map, emitted once, fixes
    // the round-trip for full themes.
    auto split = [](const std::string& name, const char* fallback)
        -> std::pair<std::string, std::string> {
        auto dot = name.find('.');
        if (dot == std::string::npos) return {fallback, name};
        return {name.substr(0, dot), name.substr(dot + 1)};
    };

    std::map<std::string, std::vector<std::string>> groups;

    for (auto& [name, color] : theme.colors) {
        auto [group, key] = split(name, "color");
        char buf[10];
        if (color.a8() == 255)
            snprintf(buf, sizeof(buf), "#%02x%02x%02x", color.r8(), color.g8(), color.b8());
        else
            snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", color.r8(), color.g8(), color.b8(), color.a8());
        std::ostringstream e;
        e << "    \"" << key << "\": { \"$value\": \"" << buf << "\", \"$type\": \"color\" }";
        groups[group].push_back(e.str());
    }

    for (auto& [name, value] : theme.dimensions) {
        auto [group, key] = split(name, "dimension");
        std::ostringstream e;
        e << "    \"" << key << "\": { \"$value\": \"" << value << "\", \"$type\": \"dimension\" }";
        groups[group].push_back(e.str());
    }

    for (auto& [name, value] : theme.strings) {
        auto [group, key] = split(name, "string");
        std::ostringstream e;
        e << "    \"" << key << "\": { \"$value\": \"" << value << "\", \"$type\": \"string\" }";
        groups[group].push_back(e.str());
    }

    std::ostringstream ss;
    ss << "{\n";
    bool first_group = true;
    for (auto& [group, entries] : groups) {
        if (!first_group) ss << ",\n";
        first_group = false;
        ss << "  \"" << group << "\": {\n";
        for (size_t i = 0; i < entries.size(); ++i) {
            ss << entries[i];
            if (i + 1 < entries.size()) ss << ",";
            ss << "\n";
        }
        ss << "  }";
    }
    ss << "\n}\n";
    return ss.str();
}

}  // namespace pulp::view
