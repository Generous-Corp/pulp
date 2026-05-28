#include <pulp/view/design_import.hpp>
#include <pulp/view/anchor_strategy.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pulp::view {
namespace {

std::string trim_ascii_ws(std::string_view sv) {
    size_t i = 0;
    size_t j = sv.size();
    while (i < j && std::isspace(static_cast<unsigned char>(sv[i]))) ++i;
    while (j > i && std::isspace(static_cast<unsigned char>(sv[j - 1]))) --j;
    return std::string(sv.substr(i, j - i));
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

ImportDiagnostic make_v0_import_diagnostic(ImportDiagnosticSeverity severity,
                                           ImportDiagnosticKind kind,
                                           std::string code,
                                           std::string path,
                                           std::string message) {
    ImportDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.kind = kind;
    diagnostic.code = std::move(code);
    diagnostic.path = std::move(path);
    diagnostic.message = std::move(message);
    return diagnostic;
}

struct V0SourceContracts {
    std::unordered_map<std::string, std::string> setter_to_state;
    std::unordered_map<std::string, std::string> handler_to_state;
    std::unordered_map<std::string, std::string> state_initial_values;
};

std::optional<float> parse_jsx_css_number(std::string_view raw) {
    auto value = trim_ascii_ws(raw);
    if (value.empty()) return std::nullopt;
    if (value.size() >= 2 && value.front() == '{' && value.back() == '}')
        value = trim_ascii_ws(std::string_view(value).substr(1, value.size() - 2));
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }
    if (value.size() > 2 && value.substr(value.size() - 2) == "px")
        value.resize(value.size() - 2);
    char* end = nullptr;
    const auto out = std::strtof(value.c_str(), &end);
    if (end == value.c_str()) return std::nullopt;
    while (*end != '\0') {
        if (!std::isspace(static_cast<unsigned char>(*end))) return std::nullopt;
        ++end;
    }
    if (!std::isfinite(out)) return std::nullopt;
    return out;
}

std::string strip_jsx_value_quotes(std::string value) {
    value = trim_ascii_ws(value);
    if (value.size() >= 2 && value.front() == '{' && value.back() == '}')
        value = trim_ascii_ws(std::string_view(value).substr(1, value.size() - 2));
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        std::string out;
        out.reserve(value.size() - 2);
        for (size_t i = 1; i + 1 < value.size(); ++i) {
            if (value[i] == '\\' && i + 2 < value.size()) {
                out += value[++i];
            } else {
                out += value[i];
            }
        }
        return out;
    }
    return value;
}

bool is_identifier_like(std::string_view value) {
    if (value.empty()) return false;
    const auto first = static_cast<unsigned char>(value.front());
    if (!(std::isalpha(first) || value.front() == '_' || value.front() == '$')) return false;
    for (char c : value.substr(1)) {
        const auto ch = static_cast<unsigned char>(c);
        if (!(std::isalnum(ch) || c == '_' || c == '$')) return false;
    }
    return true;
}

std::string normalize_state_initial_value(std::string value) {
    value = strip_jsx_value_quotes(std::move(value));
    if (value.size() > 120) return {};
    return value;
}

std::optional<std::string> state_from_setter_call_expression(
    std::string_view expression,
    const V0SourceContracts& contracts) {
    for (const auto& [setter, state_key] : contracts.setter_to_state) {
        const auto needle = setter + "(";
        if (expression.find(needle) != std::string_view::npos)
            return state_key;
    }
    return std::nullopt;
}

V0SourceContracts extract_v0_source_contracts(const std::string& tsx) {
    V0SourceContracts contracts;
    const std::regex state_re(
        R"(const\s*\[\s*([A-Za-z_$][A-Za-z0-9_$]*)\s*,\s*([A-Za-z_$][A-Za-z0-9_$]*)\s*\]\s*=\s*useState\s*\(([^)]*)\))");
    for (auto it = std::sregex_iterator(tsx.begin(), tsx.end(), state_re);
         it != std::sregex_iterator(); ++it) {
        const auto state_key = (*it)[1].str();
        const auto setter = (*it)[2].str();
        const auto initial = normalize_state_initial_value((*it)[3].str());
        contracts.setter_to_state[setter] = state_key;
        if (!initial.empty())
            contracts.state_initial_values[state_key] = initial;
    }
    const std::regex arrow_handler_re(
        R"(const\s+([A-Za-z_$][A-Za-z0-9_$]*)\s*=\s*(?:\([^)]*\)|[A-Za-z_$][A-Za-z0-9_$]*)\s*=>\s*([^;]+);)");
    for (auto it = std::sregex_iterator(tsx.begin(), tsx.end(), arrow_handler_re);
         it != std::sregex_iterator(); ++it) {
        const auto handler = (*it)[1].str();
        const auto body = (*it)[2].str();
        if (auto state_key = state_from_setter_call_expression(body, contracts))
            contracts.handler_to_state[handler] = *state_key;
    }
    const std::regex function_handler_re(
        R"(function\s+([A-Za-z_$][A-Za-z0-9_$]*)\s*\([^)]*\)\s*\{([^}]*)\})");
    for (auto it = std::sregex_iterator(tsx.begin(), tsx.end(), function_handler_re);
         it != std::sregex_iterator(); ++it) {
        const auto handler = (*it)[1].str();
        const auto body = (*it)[2].str();
        if (auto state_key = state_from_setter_call_expression(body, contracts))
            contracts.handler_to_state[handler] = *state_key;
    }
    return contracts;
}

std::optional<std::string> state_from_value_expression(std::string_view expression,
                                                       const V0SourceContracts& contracts) {
    auto value = trim_ascii_ws(expression);
    if (is_identifier_like(value) && contracts.state_initial_values.count(value) != 0)
        return value;
    if (auto bracket = value.find('['); bracket != std::string::npos) {
        const auto base = trim_ascii_ws(std::string_view(value).substr(0, bracket));
        if (is_identifier_like(base) && contracts.state_initial_values.count(base) != 0)
            return value;
    }
    return std::nullopt;
}

std::optional<std::string> state_from_event_expression(std::string_view expression,
                                                       const V0SourceContracts& contracts) {
    auto value = trim_ascii_ws(expression);
    if (is_identifier_like(value)) {
        if (auto it = contracts.handler_to_state.find(value); it != contracts.handler_to_state.end())
            return it->second;
    }
    for (const auto& [handler, state_key] : contracts.handler_to_state) {
        const auto needle = handler + "(";
        if (value.find(needle) != std::string_view::npos)
            return state_key;
    }
    return state_from_setter_call_expression(value, contracts);
}

std::optional<std::string> attr_value(const IRNode& node, std::string_view key) {
    if (auto it = node.attributes.find(std::string(key)); it != node.attributes.end())
        return it->second;
    return std::nullopt;
}

void set_contract_attr(IRNode& node, std::string key, std::string value) {
    if (value.empty()) return;
    if (auto it = node.attributes.find(key); it == node.attributes.end() || it->second.empty())
        node.attributes[std::move(key)] = std::move(value);
}

LayoutAlign parse_jsx_layout_align(const std::string& value) {
    if (value == "center") return LayoutAlign::center;
    if (value == "flex-end" || value == "end") return LayoutAlign::flex_end;
    if (value == "stretch") return LayoutAlign::stretch;
    if (value == "space-between") return LayoutAlign::space_between;
    if (value == "space-around") return LayoutAlign::space_around;
    return LayoutAlign::flex_start;
}

std::vector<std::string> split_jsx_top_level(std::string_view text, char delimiter) {
    std::vector<std::string> out;
    size_t start = 0;
    int paren_depth = 0;
    int brace_depth = 0;
    int bracket_depth = 0;
    char quote = '\0';
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote != '\0') {
            if (c == '\\') ++i;
            else if (c == quote) quote = '\0';
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            quote = c;
            continue;
        }
        if (c == '(') ++paren_depth;
        else if (c == ')' && paren_depth > 0) --paren_depth;
        else if (c == '{') ++brace_depth;
        else if (c == '}' && brace_depth > 0) --brace_depth;
        else if (c == '[') ++bracket_depth;
        else if (c == ']' && bracket_depth > 0) --bracket_depth;
        else if (c == delimiter && paren_depth == 0 && brace_depth == 0 && bracket_depth == 0) {
            out.push_back(trim_ascii_ws(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    out.push_back(trim_ascii_ws(text.substr(start)));
    return out;
}

std::optional<std::string> extract_jsx_style_body(const std::string& value) {
    auto raw = trim_ascii_ws(value);
    if (raw.size() >= 4 && raw.rfind("{{", 0) == 0 &&
        raw.substr(raw.size() - 2) == "}}") {
        return raw.substr(2, raw.size() - 4);
    }
    if (raw.size() >= 2 && raw.front() == '{' && raw.back() == '}')
        return raw.substr(1, raw.size() - 2);
    return std::nullopt;
}

void apply_jsx_style_property(IRNode& node,
                              const std::string& key,
                              const std::string& raw_value) {
    const auto value = strip_jsx_value_quotes(raw_value);
    const auto number = parse_jsx_css_number(value);

    auto set_number = [&](std::optional<float>& field) {
        if (number) field = *number;
    };
    auto set_string = [&](std::optional<std::string>& field) {
        if (!value.empty()) field = value;
    };

    if (key == "background" || key == "backgroundColor") {
        if (value.find("gradient(") != std::string::npos)
            node.style.background_gradient = value;
        else
            node.style.background_color = value;
    } else if (key == "backgroundImage") {
        node.style.background_image = value;
    } else if (key == "backgroundRepeat") {
        node.style.background_repeat = value;
    } else if (key == "color") {
        node.style.color = value;
    } else if (key == "opacity") {
        set_number(node.style.opacity);
    } else if (key == "borderRadius") {
        set_number(node.style.border_radius);
    } else if (key == "border") {
        set_string(node.style.border);
    } else if (key == "borderColor") {
        set_string(node.style.border_color);
    } else if (key == "borderWidth") {
        set_number(node.style.border_width);
    } else if (key == "borderStyle") {
        set_string(node.style.border_style);
    } else if (key == "boxShadow") {
        set_string(node.style.box_shadow);
    } else if (key == "filter") {
        set_string(node.style.filter);
    } else if (key == "backdropFilter") {
        set_string(node.style.backdrop_filter);
    } else if (key == "fontFamily") {
        set_string(node.style.font_family);
    } else if (key == "fontSize") {
        set_number(node.style.font_size);
    } else if (key == "fontWeight") {
        if (number) node.style.font_weight = static_cast<int>(*number);
    } else if (key == "fontStyle") {
        set_string(node.style.font_style);
    } else if (key == "textAlign") {
        set_string(node.style.text_align);
    } else if (key == "letterSpacing") {
        set_number(node.style.letter_spacing);
    } else if (key == "lineHeight") {
        set_number(node.style.line_height);
    } else if (key == "textTransform") {
        set_string(node.style.text_transform);
    } else if (key == "textDecoration") {
        set_string(node.style.text_decoration);
    } else if (key == "whiteSpace") {
        set_string(node.style.white_space);
    } else if (key == "textOverflow") {
        set_string(node.style.text_overflow);
    } else if (key == "overflow") {
        set_string(node.style.overflow);
    } else if (key == "overflowX") {
        node.layout.overflow_x = value;
    } else if (key == "overflowY") {
        node.layout.overflow_y = value;
    } else if (key == "cursor") {
        set_string(node.style.cursor);
    } else if (key == "position") {
        set_string(node.style.position);
    } else if (key == "top") {
        set_number(node.style.top);
    } else if (key == "left") {
        set_number(node.style.left);
    } else if (key == "right") {
        set_number(node.style.right);
    } else if (key == "bottom") {
        set_number(node.style.bottom);
    } else if (key == "zIndex") {
        if (number) node.style.z_index = static_cast<int>(*number);
    } else if (key == "transform") {
        set_string(node.style.transform);
    } else if (key == "width") {
        set_number(node.style.width);
    } else if (key == "height") {
        set_number(node.style.height);
    } else if (key == "minWidth") {
        set_number(node.style.min_width);
    } else if (key == "minHeight") {
        set_number(node.style.min_height);
    } else if (key == "maxWidth") {
        set_number(node.style.max_width);
    } else if (key == "maxHeight") {
        set_number(node.style.max_height);
    } else if (key == "display") {
        node.layout.display = value;
    } else if (key == "flexDirection") {
        node.layout.direction = (value == "row" || value == "row-reverse")
            ? LayoutDirection::row
            : LayoutDirection::column;
    } else if (key == "gap") {
        if (number) node.layout.gap = *number;
    } else if (key == "rowGap") {
        if (number) node.layout.row_gap = *number;
    } else if (key == "columnGap") {
        if (number) node.layout.column_gap = *number;
    } else if (key == "padding") {
        if (number) {
            node.layout.padding_top = *number;
            node.layout.padding_right = *number;
            node.layout.padding_bottom = *number;
            node.layout.padding_left = *number;
        }
    } else if (key == "paddingTop") {
        if (number) node.layout.padding_top = *number;
    } else if (key == "paddingRight") {
        if (number) node.layout.padding_right = *number;
    } else if (key == "paddingBottom") {
        if (number) node.layout.padding_bottom = *number;
    } else if (key == "paddingLeft") {
        if (number) node.layout.padding_left = *number;
    } else if (key == "marginTop") {
        if (number) node.layout.margin_top = *number;
    } else if (key == "marginRight") {
        if (number) node.layout.margin_right = *number;
    } else if (key == "marginBottom") {
        if (number) node.layout.margin_bottom = *number;
    } else if (key == "marginLeft") {
        if (number) node.layout.margin_left = *number;
    } else if (key == "justifyContent") {
        node.layout.justify = parse_jsx_layout_align(value);
    } else if (key == "alignItems") {
        node.layout.align = parse_jsx_layout_align(value);
    } else if (key == "alignSelf") {
        node.layout.align_self = value;
    } else if (key == "alignContent") {
        node.layout.align_content = value;
    } else if (key == "flexGrow") {
        if (number) node.layout.flex_grow = *number;
    } else if (key == "flexShrink") {
        if (number) node.layout.flex_shrink = *number;
    } else if (key == "flexBasis") {
        node.layout.flex_basis = value;
    } else if (key == "flexWrap") {
        node.layout.wrap = value != "nowrap";
    } else if (key == "order") {
        if (number) node.layout.order = static_cast<int>(*number);
    } else if (key == "aspectRatio") {
        if (number) node.layout.aspect_ratio = *number;
    } else if (!key.empty() && !value.empty()) {
        node.attributes["style." + key] = value;
    }
}

void apply_jsx_style_object(IRNode& node, const std::string& style_value) {
    auto body = extract_jsx_style_body(style_value);
    if (!body) return;
    for (const auto& entry : split_jsx_top_level(*body, ',')) {
        if (entry.empty() || entry.rfind("...", 0) == 0) continue;
        const auto parts = split_jsx_top_level(entry, ':');
        if (parts.size() < 2) continue;
        auto key = strip_jsx_value_quotes(parts.front());
        if (key.empty() || key.front() == '[') continue;
        std::string value = parts[1];
        for (size_t i = 2; i < parts.size(); ++i) value += ":" + parts[i];
        apply_jsx_style_property(node, key, value);
    }
}

std::optional<size_t> find_jsx_tag_end(const std::string& source, size_t start) {
    char quote = '\0';
    int brace_depth = 0;
    for (size_t i = start; i < source.size(); ++i) {
        const char c = source[i];
        if (quote != '\0') {
            if (c == '\\') ++i;
            else if (c == quote) quote = '\0';
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            quote = c;
            continue;
        }
        if (c == '{') ++brace_depth;
        else if (c == '}' && brace_depth > 0) --brace_depth;
        else if (c == '>' && brace_depth == 0) return i;
    }
    return std::nullopt;
}

std::string normalize_jsx_text(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    bool in_ws = false;
    for (unsigned char c : raw) {
        if (std::isspace(c)) {
            in_ws = true;
            continue;
        }
        if (in_ws && !out.empty()) out += ' ';
        out += static_cast<char>(c);
        in_ws = false;
    }
    auto text = trim_ascii_ws(out);
    if (text.find('{') != std::string::npos || text.find('}') != std::string::npos)
        return {};
    return text;
}

bool is_v0_host_jsx_tag(const std::string& tag) {
    static const std::unordered_set<std::string> kTags = {
        "div", "section", "header", "main", "footer", "nav", "article", "aside", "form",
        "h1", "h2", "h3", "h4", "h5", "h6", "p", "span", "label", "small", "strong",
        "em", "code", "button", "input", "textarea", "select", "option", "img", "canvas",
        "svg", "path", "rect", "line", "circle", "polyline", "polygon", "progress", "meter",
        "view", "text", "pressable"
    };
    return kTags.count(tag) != 0;
}

bool is_v0_void_jsx_tag(const std::string& tag) {
    return tag == "input" || tag == "img" || tag == "br" || tag == "hr" ||
           tag == "path" || tag == "rect" || tag == "line" || tag == "circle" ||
           tag == "polyline" || tag == "polygon";
}

std::string ir_type_for_v0_jsx_tag(const std::string& tag) {
    if (tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" ||
        tag == "h5" || tag == "h6" || tag == "p" || tag == "span" ||
        tag == "label" || tag == "small" || tag == "strong" || tag == "em" ||
        tag == "code" || tag == "option") {
        return "text";
    }
    if (tag == "text") return "text";
    if (tag == "button" || tag == "pressable") return "button";
    if (tag == "input" || tag == "textarea" || tag == "select") return tag;
    if (tag == "img") return "image";
    if (tag == "canvas") return "canvas";
    if (tag == "path") return "path";
    if (tag == "rect") return "rect";
    if (tag == "line") return "line";
    if (tag == "progress" || tag == "meter") return "meter";
    return "frame";
}

std::map<std::string, std::string> parse_jsx_attributes(std::string_view attrs) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    auto skip_ws = [&] {
        while (i < attrs.size() && std::isspace(static_cast<unsigned char>(attrs[i]))) ++i;
    };
    while (i < attrs.size()) {
        skip_ws();
        if (i >= attrs.size()) break;
        if (attrs[i] == '/' || attrs[i] == '{') {
            ++i;
            continue;
        }
        const size_t key_start = i;
        while (i < attrs.size()) {
            const unsigned char c = static_cast<unsigned char>(attrs[i]);
            if (std::isalnum(c) || attrs[i] == '-' || attrs[i] == '_' ||
                attrs[i] == ':' || attrs[i] == '.') {
                ++i;
            } else {
                break;
            }
        }
        if (i == key_start) {
            ++i;
            continue;
        }
        auto key = std::string(attrs.substr(key_start, i - key_start));
        skip_ws();
        if (i >= attrs.size() || attrs[i] != '=') {
            out[key] = "true";
            continue;
        }
        ++i;
        skip_ws();
        if (i >= attrs.size()) {
            out[key] = "";
            break;
        }

        std::string value;
        if (attrs[i] == '"' || attrs[i] == '\'' || attrs[i] == '`') {
            const char quote = attrs[i++];
            const size_t value_start = i;
            while (i < attrs.size()) {
                if (attrs[i] == '\\') {
                    i += 2;
                    continue;
                }
                if (attrs[i] == quote) break;
                ++i;
            }
            value = std::string(attrs.substr(value_start, i - value_start));
            if (i < attrs.size() && attrs[i] == quote) ++i;
        } else if (attrs[i] == '{') {
            const size_t value_start = i;
            int depth = 0;
            char quote = '\0';
            while (i < attrs.size()) {
                const char c = attrs[i];
                if (quote != '\0') {
                    if (c == '\\') ++i;
                    else if (c == quote) quote = '\0';
                } else if (c == '"' || c == '\'' || c == '`') {
                    quote = c;
                } else if (c == '{') {
                    ++depth;
                } else if (c == '}') {
                    --depth;
                    if (depth == 0) {
                        ++i;
                        break;
                    }
                }
                ++i;
            }
            value = std::string(attrs.substr(value_start, i - value_start));
        } else {
            const size_t value_start = i;
            while (i < attrs.size() && !std::isspace(static_cast<unsigned char>(attrs[i])) &&
                   attrs[i] != '/') {
                ++i;
            }
            value = std::string(attrs.substr(value_start, i - value_start));
        }
        out[std::move(key)] = std::move(value);
    }
    return out;
}

void apply_v0_jsx_classes(IRNode& node, const std::string& classes) {
    if (classes.empty()) return;
    node.attributes["className"] = classes;
    if (classes.find("flex-row") != std::string::npos)
        node.layout.direction = LayoutDirection::row;
    if (classes.find("flex-col") != std::string::npos)
        node.layout.direction = LayoutDirection::column;
    if (classes.find("grid") != std::string::npos)
        node.layout.display = "grid";
    if (classes.find("overflow-hidden") != std::string::npos)
        node.style.overflow = "hidden";
}

void apply_v0_jsx_attribute(IRNode& node,
                            const std::string& key,
                            const std::string& raw_value) {
    if (key == "style") {
        apply_jsx_style_object(node, raw_value);
        return;
    }
    const auto value = strip_jsx_value_quotes(raw_value);
    if (key == "className" || key == "class") {
        apply_v0_jsx_classes(node, value);
        return;
    }
    if (key == "htmlFor") {
        node.attributes["for"] = value;
        return;
    }
    if (key == "onPress" || key == "onpress") {
        node.attributes[key] = value.empty() ? "handler" : value;
        node.attributes["onClick"] = node.attributes[key];
        return;
    }
    if (key == "onClick" || key == "onclick" ||
        key == "onChange" || key == "onInput" ||
        key == "onPointerDown" || key == "onMouseDown") {
        node.attributes[key] = value.empty() ? "handler" : value;
        return;
    }
    if (key == "width") {
        if (auto number = parse_jsx_css_number(value)) node.style.width = *number;
    } else if (key == "height") {
        if (auto number = parse_jsx_css_number(value)) node.style.height = *number;
    }
    if (!key.empty())
        node.attributes[key] = value;
}

std::string input_contract_family(const IRNode& node) {
    const auto subtype = to_lower(attr_value(node, "type").value_or("text"));
    if (subtype == "range") return "input:range";
    if (subtype == "checkbox") return "input:checkbox";
    return "input:" + subtype;
}

void attach_v0_value_contract(IRNode& node,
                              const std::string& state_key,
                              const V0SourceContracts& contracts) {
    set_contract_attr(node, "pulpValueKey", state_key);
    if (auto it = contracts.state_initial_values.find(state_key);
        it != contracts.state_initial_values.end()) {
        set_contract_attr(node, "pulpInitialValue", it->second);
        set_contract_attr(node, "pulpDefaultValueSource", "useState");
    }
}

void attach_v0_source_contracts(IRNode& node, const V0SourceContracts& contracts) {
    const auto tag = attr_value(node, "jsxTag").value_or(node.name);
    const auto lower_tag = to_lower(tag);
    const auto value_expr = attr_value(node, "value");
    const auto checked_expr = attr_value(node, "checked");
    const auto on_change = attr_value(node, "onChange");
    const auto on_input = attr_value(node, "onInput");
    const auto on_click = attr_value(node, "onClick");
    const auto on_click_lower = attr_value(node, "onclick");
    const auto on_press = attr_value(node, "onPress");
    const auto on_press_lower = attr_value(node, "onpress");
    const auto on_pointer = attr_value(node, "onPointerDown");
    const auto on_mouse = attr_value(node, "onMouseDown");

    std::optional<std::string> state_key;
    if (value_expr) state_key = state_from_value_expression(*value_expr, contracts);
    if (!state_key && checked_expr) state_key = state_from_value_expression(*checked_expr, contracts);
    if (!state_key && on_change) state_key = state_from_event_expression(*on_change, contracts);
    if (!state_key && on_input) state_key = state_from_event_expression(*on_input, contracts);
    if (!state_key && on_click) state_key = state_from_event_expression(*on_click, contracts);
    if (!state_key && on_click_lower) state_key = state_from_event_expression(*on_click_lower, contracts);
    if (!state_key && on_press) state_key = state_from_event_expression(*on_press, contracts);
    if (!state_key && on_press_lower) state_key = state_from_event_expression(*on_press_lower, contracts);
    if (!state_key && on_pointer) state_key = state_from_event_expression(*on_pointer, contracts);
    if (!state_key && on_mouse) state_key = state_from_event_expression(*on_mouse, contracts);

    if (state_key) {
        attach_v0_value_contract(node, *state_key, contracts);
        set_contract_attr(node, "pulpRouteType", "native_cpp");
        set_contract_attr(node, "pulpSourceFamily",
                          lower_tag == "input" ? input_contract_family(node) : lower_tag);
    }

    if (lower_tag == "input") {
        const auto subtype = to_lower(attr_value(node, "type").value_or("text"));
        if (subtype == "range") {
            set_contract_attr(node, "pulpEventContract", on_change ? "range:onChange:setState" : "range:value");
            set_contract_attr(node, "pulpGestureContract", "range:drag");
            if (!node.style.width)
                node.style.width = 120.0f;
            if (!node.style.height)
                node.style.height = 20.0f;
        } else if (subtype == "checkbox") {
            set_contract_attr(node, "pulpEventContract", on_change ? "checkbox:onChange:setState" : "checkbox:checked");
            set_contract_attr(node, "pulpGestureContract", "checkbox:toggle");
        }
    } else if (lower_tag == "select") {
        set_contract_attr(node, "pulpEventContract", on_change ? "select:onChange:setState" : "select:value");
        set_contract_attr(node, "pulpGestureContract", "select:choose");
    } else if (lower_tag == "button" || lower_tag == "pressable") {
        if (on_click || on_click_lower || on_press || on_press_lower)
            set_contract_attr(node, "pulpEventContract", "button:onClick:setState");
        set_contract_attr(node, "pulpGestureContract", "button:click");
    } else if (lower_tag == "meter" || lower_tag == "progress") {
        if (value_expr && state_key)
            set_contract_attr(node, lower_tag == "meter" ? "pulpMeterValueKey" : "pulpValueKey", *state_key);
        set_contract_attr(node, "pulpRouteType", "native_cpp");
        set_contract_attr(node, "pulpSourceFamily", lower_tag);
    }

    for (auto& child : node.children)
        attach_v0_source_contracts(child, contracts);
}

void add_v0_jsx_text(IRNode& parent, std::string_view raw_text) {
    const auto text = normalize_jsx_text(raw_text);
    if (text.empty()) return;
    if (parent.type == "text" || parent.type == "button") {
        if (!parent.text_content.empty()) parent.text_content += " ";
        parent.text_content += text;
        return;
    }
    IRNode child;
    child.type = "text";
    child.name = "text";
    child.text_content = text;
    parent.children.push_back(std::move(child));
}

bool parse_v0_tsx_structural(const std::string& tsx, DesignIR& ir) {
    ir.root.type = "frame";
    ir.root.name = "V0Import";
    ir.root.layout.direction = LayoutDirection::column;

    struct StackEntry {
        std::string tag;
        IRNode* node;
    };
    std::vector<StackEntry> stack;
    stack.push_back({"<root>", &ir.root});

    size_t pos = 0;
    size_t last_text_end = 0;
    size_t node_count = 0;

    while (pos < tsx.size()) {
        const auto tag_start = tsx.find('<', pos);
        if (tag_start == std::string::npos) break;
        if (tag_start > last_text_end && stack.size() > 1)
            add_v0_jsx_text(*stack.back().node, std::string_view(tsx).substr(last_text_end, tag_start - last_text_end));
        if (tag_start + 1 >= tsx.size()) break;

        const char next = tsx[tag_start + 1];
        if (next == '>' || next == '!' || next == '?') {
            pos = tag_start + 1;
            last_text_end = pos;
            continue;
        }

        auto maybe_end = find_jsx_tag_end(tsx, tag_start + 1);
        if (!maybe_end) break;
        const auto tag_end = *maybe_end;
        auto inner = trim_ascii_ws(std::string_view(tsx).substr(tag_start + 1, tag_end - tag_start - 1));
        last_text_end = tag_end + 1;
        pos = tag_end + 1;

        if (inner.empty() || inner == "/" || inner.front() == '>') continue;

        bool closing = false;
        if (inner.front() == '/') {
            closing = true;
            inner = trim_ascii_ws(std::string_view(inner).substr(1));
        }

        bool self_closing = false;
        if (!inner.empty() && inner.back() == '/') {
            self_closing = true;
            inner = trim_ascii_ws(std::string_view(inner).substr(0, inner.size() - 1));
        }

        size_t name_end = 0;
        while (name_end < inner.size() &&
               !std::isspace(static_cast<unsigned char>(inner[name_end])) &&
               inner[name_end] != '/') {
            ++name_end;
        }
        if (name_end == 0) continue;

        auto tag = inner.substr(0, name_end);
        const auto lower_tag = to_lower(tag);
        if (closing) {
            for (size_t i = stack.size(); i > 1; --i) {
                if (stack[i - 1].tag == lower_tag) {
                    stack.resize(i - 1);
                    break;
                }
            }
            continue;
        }

        if (!is_v0_host_jsx_tag(lower_tag)) continue;

        const auto attrs = name_end < inner.size()
            ? std::string_view(inner).substr(name_end)
            : std::string_view{};

        IRNode node;
        node.type = ir_type_for_v0_jsx_tag(lower_tag);
        node.name = lower_tag;
        node.attributes["jsxTag"] = lower_tag;
        for (const auto& [key, value] : parse_jsx_attributes(attrs))
            apply_v0_jsx_attribute(node, key, value);

        if (node.type == "input") {
            if (auto it = node.attributes.find("aria-label"); it != node.attributes.end())
                node.audio_label = it->second;
            else if (auto it = node.attributes.find("name"); it != node.attributes.end())
                node.audio_label = it->second;
        }
        if (node.type == "meter")
            node.audio_widget = AudioWidgetType::meter;

        IRNode* parent = stack.back().node;
        parent->children.push_back(std::move(node));
        IRNode* inserted = &parent->children.back();
        ++node_count;

        if (!self_closing && !is_v0_void_jsx_tag(lower_tag))
            stack.push_back({lower_tag, inserted});
    }

    if (last_text_end < tsx.size() && stack.size() > 1)
        add_v0_jsx_text(*stack.back().node, std::string_view(tsx).substr(last_text_end));

    return node_count > 0 || !ir.root.children.empty();
}

void finish_v0_parse(DesignIR& ir, IRConfidence confidence) {
    ir.source = DesignSource::v0;
    ir.capture_method = "adapter_parse";
    ir.source_adapter = "v0-tsx";
    ir.source_version = "1";
    ir.root.provenance = IRProvenance{"v0-tsx", "1", {}};
    ir.root.confidence = confidence;
    promote_interactive_frames(ir.root);
    assign_anchors(ir.root, AnchorStrategy::content_hash);
}

} // namespace

DesignIR parse_v0_tsx(const std::string& tsx) {
    try {
        auto ir = parse_design_ir_json(tsx);
        finish_v0_parse(ir, IRConfidence::pass);
        return ir;
    } catch (...) {
        // Not JSON: continue with the bounded static TSX extractor.
    }

    DesignIR ir;
    ir.source = DesignSource::v0;
    ir.capture_method = "adapter_parse";
    ir.source_adapter = "v0-tsx";
    ir.source_version = "1";

    if (!parse_v0_tsx_structural(tsx, ir)) {
        ir.root.type = "frame";
        ir.root.name = "V0Import";
        ir.root.layout.direction = LayoutDirection::column;
        ir.fallback_reason = "input was not JSON and no supported host JSX tags were found";
        ir.diagnostics.push_back(make_v0_import_diagnostic(
            ImportDiagnosticSeverity::warning,
            ImportDiagnosticKind::fallback_used,
            "fallback-used",
            "<root>",
            ir.fallback_reason));
        finish_v0_parse(ir, IRConfidence::diverge);
        return ir;
    }

    if (ir.root.children.size() == 1) {
        auto promoted_root = std::move(ir.root.children.front());
        ir.root = std::move(promoted_root);
    }

    attach_v0_source_contracts(ir.root, extract_v0_source_contracts(tsx));

    ir.diagnostics.push_back(make_v0_import_diagnostic(
        ImportDiagnosticSeverity::warning,
        ImportDiagnosticKind::capture_partial,
        "capture-partial",
        "<root>",
        "TSX host JSX was structurally extracted; dynamic expressions and custom components require runtime import"));
    finish_v0_parse(ir, IRConfidence::diverge);
    return ir;
}

} // namespace pulp::view
