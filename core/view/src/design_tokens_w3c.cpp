// design_tokens_w3c.cpp — W3C Design Tokens (DTCG) emitter for IRTokens.
//
// Additive authoring-side export: the envelope token file (design_tokens.cpp)
// and the runtime Theme pair (w3c_tokens.cpp) are untouched. Values are built
// as a choc::value tree and serialized with choc::json::toString, so escaping
// and number formatting stay on the same JSON writer the parsers round-trip
// through. unordered_map sources are sorted before insertion so the emitted
// document is byte-stable across runs.
//
// String tokens have no standard DTCG type, so the emitter never invents one:
// names that clearly denote a font family are promoted to `$type:
// "fontFamily"`, and every other string is parked losslessly under the
// document-root `$extensions["dev.pulp.nonStandardTokens"]`. The emitted
// token groups therefore contain only standard-typed tokens, which
// validate_dtcg() checks.

#include <pulp/view/design_tokens_w3c.hpp>

#include <choc/containers/choc_Value.h>
#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::view {
namespace {

// "brand/primary" → {"brand", "primary"}. Empty segments from doubled or
// leading/trailing separators are dropped; a name that is empty or all
// separators keeps its raw spelling as a single key rather than vanishing.
std::vector<std::string> split_group_path(const std::string& name) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : name) {
        if (c == '/') {
            if (!current.empty()) parts.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) parts.push_back(std::move(current));
    if (parts.empty()) parts.push_back(name);
    return parts;
}

struct GroupNode {
    std::map<std::string, GroupNode> children;  // sorted → deterministic output
    std::optional<choc::value::Value> token;
};

void insert_token(GroupNode& root, const std::string& name, choc::value::Value token) {
    GroupNode* node = &root;
    for (auto& part : split_group_path(name))
        node = &node->children[part];
    node->token = std::move(token);
}

// A name that is both a token and a group prefix ("brand" and
// "brand/primary") merges into one object carrying the token's $ members
// plus the child groups, rather than dropping either side.
choc::value::Value group_to_value(const GroupNode& node) {
    auto obj = node.token ? *node.token : choc::value::createObject("");
    for (const auto& [key, child] : node.children)
        obj.addMember(key, group_to_value(child));
    return obj;
}

void attach_source_extension(choc::value::Value& token_obj,
                             const IRTokens& tokens,
                             const std::string& canonical_path) {
    auto it = tokens.source_identity.find(canonical_path);
    if (it == tokens.source_identity.end()) return;
    const auto& id = it->second;
    auto source = choc::value::createObject("");
    if (!id.source_id.empty()) source.addMember("id", id.source_id);
    if (!id.source_collection.empty()) source.addMember("collection", id.source_collection);
    if (!id.source_mode.empty()) source.addMember("mode", id.source_mode);
    if (!id.source_adapter.empty()) source.addMember("adapter", id.source_adapter);
    if (source.size() == 0) return;
    auto extensions = choc::value::createObject("");
    extensions.addMember("dev.pulp.source", source);
    token_obj.addMember("$extensions", extensions);
}

std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Conservative promotion gate: only names that unambiguously denote a font
// family get $type "fontFamily". Segments split on both "/" (the emitter's
// group separator) and "." (the dotted names designmd/Theme sources use).
// Promoted when any segment is "font"/"fontfamily"/"font-family"/"typeface"
// or the final segment is "family"/"font". Everything else is parked, never
// guessed at.
bool is_font_family_name(const std::string& name) {
    const auto lower = to_lower(name);
    std::vector<std::string> segments;
    std::string current;
    for (char c : lower) {
        if (c == '/' || c == '.') {
            if (!current.empty()) segments.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) segments.push_back(std::move(current));
    if (segments.empty()) return false;

    static constexpr std::array<std::string_view, 4> family_segments = {
        "font", "fontfamily", "font-family", "typeface"};
    for (const auto& segment : segments)
        for (auto candidate : family_segments)
            if (segment == candidate) return true;
    const auto& last = segments.back();
    return last == "family" || last == "font";
}

// "Inter, SF Pro, sans-serif" → DTCG array form ["Inter","SF Pro","sans-serif"];
// a value without commas stays a plain string.
choc::value::Value font_family_value(const std::string& text) {
    if (text.find(',') == std::string::npos)
        return choc::value::createString(text);
    auto trim = [](std::string s) {
        auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
        while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
        return s;
    };
    auto stack = choc::value::createEmptyArray();
    std::string current;
    for (char c : text) {
        if (c == ',') {
            auto entry = trim(std::move(current));
            if (!entry.empty()) stack.addArrayElement(entry);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    auto entry = trim(std::move(current));
    if (!entry.empty()) stack.addArrayElement(entry);
    if (stack.size() == 0) return choc::value::createString(text);
    return stack;
}

}  // namespace

std::string to_w3c_tokens_json(const IRTokens& tokens, bool pretty) {
    auto root = choc::value::createObject("");

    if (!tokens.colors.empty()) {
        GroupNode group;
        std::map<std::string, std::string> sorted(tokens.colors.begin(), tokens.colors.end());
        for (const auto& [name, hex] : sorted) {
            auto token = choc::value::createObject("");
            token.addMember("$type", "color");
            token.addMember("$value", hex);
            attach_source_extension(token, tokens, "colors." + name);
            insert_token(group, name, std::move(token));
        }
        root.addMember("colors", group_to_value(group));
    }

    if (!tokens.dimensions.empty()) {
        GroupNode group;
        std::map<std::string, float> sorted(tokens.dimensions.begin(), tokens.dimensions.end());
        for (const auto& [name, px] : sorted) {
            auto value = choc::value::createObject("");
            value.addMember("value", px);
            value.addMember("unit", "px");
            auto token = choc::value::createObject("");
            token.addMember("$type", "dimension");
            token.addMember("$value", value);
            attach_source_extension(token, tokens, "dimensions." + name);
            insert_token(group, name, std::move(token));
        }
        root.addMember("dimensions", group_to_value(group));
    }

    // Strings carry no standard DTCG type, so only confident font families
    // become tokens; everything else is parked (losslessly, with provenance)
    // under the document-root $extensions rather than dropped or emitted with
    // an invented $type.
    std::map<std::string, std::string> parked;
    if (!tokens.strings.empty()) {
        GroupNode group;
        bool any_font = false;
        std::map<std::string, std::string> sorted(tokens.strings.begin(), tokens.strings.end());
        for (const auto& [name, text] : sorted) {
            if (!is_font_family_name(name)) {
                parked.emplace(name, text);
                continue;
            }
            any_font = true;
            auto token = choc::value::createObject("");
            token.addMember("$type", "fontFamily");
            token.addMember("$value", font_family_value(text));
            attach_source_extension(token, tokens, "strings." + name);
            insert_token(group, name, std::move(token));
        }
        if (any_font) root.addMember("strings", group_to_value(group));
    }

    if (!parked.empty()) {
        auto bucket = choc::value::createObject("");
        for (const auto& [name, text] : parked) {
            auto entry = choc::value::createObject("");
            entry.addMember("value", text);
            auto it = tokens.source_identity.find("strings." + name);
            if (it != tokens.source_identity.end()) {
                const auto& id = it->second;
                if (!id.source_id.empty()) entry.addMember("id", id.source_id);
                if (!id.source_collection.empty()) entry.addMember("collection", id.source_collection);
                if (!id.source_mode.empty()) entry.addMember("mode", id.source_mode);
                if (!id.source_adapter.empty()) entry.addMember("adapter", id.source_adapter);
            }
            bucket.addMember(name, entry);
        }
        auto extensions = choc::value::createObject("");
        extensions.addMember("dev.pulp.nonStandardTokens", bucket);
        root.addMember("$extensions", extensions);
    }

    return choc::json::toString(root, pretty);
}

namespace {

constexpr std::array<std::string_view, 13> dtcg_standard_types = {
    "color",  "dimension",   "fontFamily", "fontWeight", "duration",
    "cubicBezier", "number", "strokeStyle", "border",    "transition",
    "shadow", "gradient",    "typography"};

bool is_standard_type(std::string_view type) {
    for (auto t : dtcg_standard_types)
        if (type == t) return true;
    return false;
}

bool is_allowed_reserved_key(std::string_view key) {
    return key == "$value" || key == "$type" || key == "$description" ||
           key == "$extensions" || key == "$deprecated";
}

std::string join_path(const std::string& parent, const char* name) {
    return parent.empty() ? std::string(name) : parent + "." + name;
}

void check_value_shape(const choc::value::ValueView& value,
                       std::string_view type,
                       const std::string& path,
                       std::vector<std::string>& out) {
    if (type == "color") {
        if (!value.isString())
            out.push_back("token '" + path + "': color $value must be a string");
    } else if (type == "dimension") {
        if (!value.isObject()) {
            out.push_back("token '" + path +
                          "': dimension $value must be an object {value, unit}");
            return;
        }
        if (!value.hasObjectMember("value") ||
            !(value["value"].isFloat() || value["value"].isInt()))
            out.push_back("token '" + path +
                          "': dimension $value.value must be a number");
        if (!value.hasObjectMember("unit") || !value["unit"].isString())
            out.push_back("token '" + path +
                          "': dimension $value.unit must be a string");
    } else if (type == "fontFamily") {
        if (value.isString()) return;
        if (value.isArray()) {
            for (uint32_t i = 0; i < value.size(); ++i)
                if (!value[i].isString()) {
                    out.push_back("token '" + path +
                                  "': fontFamily $value array must contain only strings");
                    return;
                }
            return;
        }
        out.push_back("token '" + path +
                      "': fontFamily $value must be a string or array of strings");
    }
    // Other standard types: shape not checked (best-effort validator).
}

// Walks one object node. A node with $value is a token; without, a group.
// A merged token+group object (a name that is both) is treated as both:
// its $-keys are validated as a token's, its non-$ members recurse as
// children. `inherited_type` is the nearest ancestor $type, per DTCG group
// type inheritance.
void validate_node(const choc::value::ValueView& node,
                   const std::string& path,
                   const std::string& inherited_type,
                   std::vector<std::string>& out) {
    std::string own_type;
    if (node.hasObjectMember("$type")) {
        auto t = node["$type"];
        if (t.isString())
            own_type = std::string(t.getString());
        else
            out.push_back("'" + (path.empty() ? std::string("<root>") : path) +
                          "': $type must be a string");
    }
    const std::string& effective_type = own_type.empty() ? inherited_type : own_type;
    const bool is_token = node.hasObjectMember("$value");

    for (uint32_t i = 0; i < node.size(); ++i) {
        auto member = node.getObjectMemberAt(i);
        const std::string display = path.empty() ? std::string("<root>") : path;
        if (member.name[0] == '$') {
            if (!is_allowed_reserved_key(member.name)) {
                out.push_back("'" + display + "': unknown reserved key '" +
                              member.name + "'");
                continue;
            }
            if (std::string_view(member.name) == "$extensions") {
                if (!member.value.isObject()) {
                    out.push_back("'" + display + "': $extensions must be an object");
                    continue;
                }
                for (uint32_t j = 0; j < member.value.size(); ++j) {
                    auto ext = member.value.getObjectMemberAt(j);
                    if (std::string_view(ext.name).find('.') == std::string_view::npos)
                        out.push_back("'" + display + "': $extensions key '" +
                                      ext.name + "' is not namespaced");
                }
            }
            continue;
        }
        // Non-$ member: a child token or group — must be an object.
        auto child_path = join_path(path, member.name);
        if (!member.value.isObject()) {
            out.push_back("'" + child_path +
                          "': group member is neither a token nor a group (expected object)");
            continue;
        }
        validate_node(member.value, child_path, effective_type, out);
    }

    if (is_token) {
        const std::string display = path.empty() ? std::string("<root>") : path;
        if (effective_type.empty()) {
            out.push_back("token '" + display + "' has no resolvable $type");
        } else if (!is_standard_type(effective_type)) {
            out.push_back("token '" + display + "' has non-standard $type '" +
                          effective_type + "'");
        } else {
            check_value_shape(node["$value"], effective_type, display, out);
        }
    }
}

}  // namespace

std::vector<std::string> validate_dtcg(const std::string& json) {
    std::vector<std::string> violations;
    choc::value::Value root;
    try {
        root = choc::json::parse(json);
    } catch (const std::exception& e) {
        violations.push_back(std::string("invalid JSON: ") + e.what());
        return violations;
    }
    if (!root.isObject()) {
        violations.push_back("document root must be an object");
        return violations;
    }
    validate_node(root, "", "", violations);
    return violations;
}

}  // namespace pulp::view
