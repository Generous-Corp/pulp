// design_tokens_w3c.cpp — W3C Design Tokens (DTCG) emitter for IRTokens.
//
// Additive authoring-side export: the envelope token file (design_tokens.cpp)
// and the runtime Theme pair (w3c_tokens.cpp) are untouched. Values are built
// as a choc::value tree and serialized with choc::json::toString, so escaping
// and number formatting stay on the same JSON writer the parsers round-trip
// through. unordered_map sources are sorted before insertion so the emitted
// document is byte-stable across runs.

#include <pulp/view/design_tokens_w3c.hpp>

#include <choc/containers/choc_Value.h>
#include <choc/text/choc_JSON.h>

#include <map>
#include <optional>
#include <string>
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

    if (!tokens.strings.empty()) {
        GroupNode group;
        std::map<std::string, std::string> sorted(tokens.strings.begin(), tokens.strings.end());
        for (const auto& [name, text] : sorted) {
            auto token = choc::value::createObject("");
            token.addMember("$type", "string");
            token.addMember("$value", text);
            attach_source_extension(token, tokens, "strings." + name);
            insert_token(group, name, std::move(token));
        }
        root.addMember("strings", group_to_value(group));
    }

    return choc::json::toString(root, pretty);
}

}  // namespace pulp::view
