// Fail-closed action-plan boundary and receipt for the graph agent loop: the
// plan parser, the topology fingerprint and the JSONL action log. These are
// the verification harness around the authoring glue in main.cpp.
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <choc/text/choc_JSON.h>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <pulp/host/signal_graph.hpp>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace a9 {
using pulp::host::SignalGraph;

inline int checks = 0;
inline void require(bool condition, const char* rule) {
    ++checks;
    if (!condition)
        throw std::runtime_error(rule);
}
struct ActionPlan {
    std::vector<std::string> steps;
    float edit_value = 0.0f;
    std::string replay_input;
};

inline std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path);
    require(file.good(), "open-action-plan");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// choc's parser supplies syntax, value typing and strict duplicate-key
// rejection (escaped and Unicode spellings of a key collide). Everything below
// pins the plan to one exact shape, so no extra field or string can smuggle a
// policy change past the boundary.
template <typename V> auto member(const V& object, const char* key, const char* rule) {
    require(object.isObject() && object.hasObjectMember(key), rule);
    return object[key];
}
template <typename V>
void only_fields(const V& object, std::vector<std::string_view> allowed, const char* rule) {
    require(object.isObject() && object.size() == allowed.size(), rule);
    for (const auto name : allowed)
        require(object.hasObjectMember(std::string(name)), rule);
}
template <typename V>
std::string string_member(const V& object, const char* key, const char* rule) {
    const auto value = member(object, key, rule);
    require(value.isString(), rule);
    return std::string(value.getString());
}
inline std::string lower_ascii(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
inline bool is_sha256_pin(std::string_view text) {
    return text.size() == 71 && text.rfind("sha256:", 0) == 0 &&
           std::all_of(text.begin() + 7, text.end(),
                       [](unsigned char c) { return std::isxdigit(c); });
}
// URI schemes, protocol-relative or UNC authorities, bracketed IPv6 hosts and
// bare host:port strings are all network references. The sha256 model pin is a
// content address, not a scheme.
inline bool is_uri_or_network_string(const std::string& value) {
    static const std::regex network(
        R"(^\s*(?://|\\\\|\[|[0-9][0-9a-fA-F.]*:[0-9]+(?:[/\\?#]|$)|[A-Za-z][A-Za-z0-9+.-]*:))");
    return !is_sha256_pin(value) && std::regex_search(value, network);
}
template <typename V> void reject_urls(const V& value) {
    if (value.isString())
        require(!is_uri_or_network_string(std::string(value.getString())), "plan-network-url");
    else if (value.isArray() || value.isObject())
        for (uint32_t i = 0; i < value.size(); ++i)
            reject_urls(value.isArray() ? value[i] : value.getObjectMemberAt(i).value);
}

// The ordered action catalog: id, kind, and the exact members (beyond id and
// kind) each step carries.
struct StepSpec {
    std::string_view id, kind;
    std::vector<std::string_view> members;
};
inline ActionPlan parse_action_plan(const std::string& text) {
    choc::value::Value root;
    try {
        root = choc::json::parse(text);
    } catch (...) {
        throw std::runtime_error("plan-json-parse");
    }
    only_fields(root,
                {"schema", "model_id", "model_digest", "tool_id", "tool_version", "source_sha",
                 "network_policy", "plugin_policy", "steps", "assertions", "mutations"},
                "plan-field");
    require(string_member(root, "schema", "plan-schema") == "pulp.a9.action-plan/v1",
            "plan-schema");
    require(string_member(root, "model_id", "model-pin") == "deterministic-offline-actions-v1",
            "model-pin");
    require(is_sha256_pin(string_member(root, "model_digest", "model-digest")), "model-digest");
    require(string_member(root, "tool_id", "tool-pin") == "pulp-graph-agent-loop", "tool-pin");
    require(string_member(root, "tool_version", "tool-version") == "1", "tool-version");
    require(string_member(root, "source_sha", "source-pin") ==
                "3fad8b42a44d496361f97eeb469639e3a2b9e3c6",
            "source-pin");
    const auto network = lower_ascii(string_member(root, "network_policy", "plan-network-policy"));
    require(network == "deny", network == "allow" ? "plan-network-allow" : "plan-network-policy");
    const auto plugins = lower_ascii(string_member(root, "plugin_policy", "plan-plugin-policy"));
    require(plugins == "deny-ambient",
            plugins == "allow-ambient" ? "plan-plugin-allow" : "plan-plugin-policy");
    // Fence every string after the digest and policy fields are judged, so a
    // malformed digest cannot be mistaken for a URI scheme.
    reject_urls(root);

    const std::vector<StepSpec> catalog = {
        {"catalog-discover", "catalog.discover", {}},
        {"author-graph", "graph.author", {}},
        {"validate-replay", "graph.validate", {"load_plugins"}},
        {"render-probe", "graph.render", {"edge_probe"}},
        {"bounded-edit", "graph.edit", {"target", "value"}},
        {"saved-input-replay", "graph.replay", {"input"}},
    };
    const auto steps = member(root, "steps", "plan-steps");
    require(steps.isArray() && steps.size() == catalog.size(), "action-catalog");
    ActionPlan plan;
    for (uint32_t i = 0; i < steps.size(); ++i) {
        const auto entry = steps[i];
        auto members = catalog[i].members;
        members.insert(members.end(), {"id", "kind"});
        only_fields(entry, members, "plan-action-field");
        require(string_member(entry, "id", "plan-action-id") == catalog[i].id, "action-catalog");
        require(string_member(entry, "kind", "plan-action-kind") == catalog[i].kind, "action-kind");
        plan.steps.emplace_back(catalog[i].id);
    }
    const auto load_plugins = steps[2]["load_plugins"], edge_probe = steps[3]["edge_probe"];
    require(load_plugins.isBool() && !load_plugins.getBool(), "plan-plugin-load");
    require(edge_probe.isBool() && edge_probe.getBool(), "plan-edge-probe");
    const auto edit_value = steps[4]["value"];
    require(string_member(steps[4], "target", "action-edit") == "stage.gain", "action-edit");
    require(edit_value.isInt() || edit_value.isFloat(), "action-edit-value");
    plan.edit_value =
        static_cast<float>(edit_value.isFloat() ? edit_value.getFloat64() : edit_value.getInt64());
    require(std::isfinite(plan.edit_value), "action-edit-value");
    plan.replay_input = string_member(steps[5], "input", "action-replay");
    require(plan.replay_input == "a9-baseline.pulpgraph", "action-replay");

    const auto assertions = member(root, "assertions", "plan-assertions");
    require(assertions.isArray() && assertions.size() == 1, "plan-assertions");
    only_fields(assertions[0], {"id", "relation"}, "plan-assertion-field");
    require(string_member(assertions[0], "id", "fingerprint-rule") == "semantic_fingerprint",
            "fingerprint-rule");
    require(string_member(assertions[0], "relation", "fingerprint-relation") ==
                "baseline == replay; edit preserves topology",
            "fingerprint-relation");
    const auto mutations = member(root, "mutations", "plan-mutations");
    constexpr std::array<std::array<std::string_view, 3>, 2> expected_mutations = {{
        {"wrong-edit-oracle", "edit-changes-render", "reject"},
        {"wrong-replay-input", "replay-baseline", "reject"},
    }};
    require(mutations.isArray() && mutations.size() == expected_mutations.size(), "plan-mutations");
    for (uint32_t i = 0; i < mutations.size(); ++i) {
        only_fields(mutations[i], {"id", "target", "expected"}, "plan-mutation-field");
        require(string_member(mutations[i], "id", "plan-mutation-id") == expected_mutations[i][0],
                "mutation-catalog");
        require(string_member(mutations[i], "target", "mutation-target") ==
                    expected_mutations[i][1],
                "mutation-target");
        require(string_member(mutations[i], "expected", "mutation-expected") ==
                    expected_mutations[i][2],
                "mutation-expected");
    }
    return plan;
}

inline std::string fnv1a_hex(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}
inline std::string semantic_fingerprint(const SignalGraph& graph) {
    std::ostringstream out;
    for (const auto id : graph.processing_order()) {
        const auto* node = graph.node(id);
        require(node != nullptr, "fingerprint-node");
        out << static_cast<int>(node->type) << ':' << node->name << ':' << node->num_input_ports
            << ':' << node->num_output_ports << ';';
    }
    for (const auto& edge : graph.connections()) {
        const auto* source = graph.node(edge.source_node);
        const auto* destination = graph.node(edge.dest_node);
        require(source != nullptr && destination != nullptr, "fingerprint-edge-node");
        out << source->name << ':' << edge.source_port << '>' << destination->name << ':'
            << edge.dest_port << ':' << edge.feedback << ':' << edge.midi << ':' << edge.automation
            << ':' << edge.audio_rate_modulation << ':' << edge.sidechain << ':' << edge.spectral
            << ';';
    }
    return fnv1a_hex(out.str());
}

class ActionLog {
  public:
    ActionLog(const std::filesystem::path& path, std::string_view digest) : file_(path) {
        require(file_.good(), "open-action-log");
        file_ << "{\"schema\":\"pulp.a9.action-log/v1\",\"plan_digest\":\"" << digest
              << "\",\"network\":\"denied\",\"ambient_plugins\":\"denied\"}\n";
        file_.flush();
    }
    void record(std::string_view id) {
        file_ << "{\"action\":\"" << id << "\",\"status\":\"pass\"}\n";
        file_.flush();
    }
    void fingerprint(std::string_view baseline, std::string_view edited) {
        file_ << "{\"semantic_fingerprint\":\"" << baseline
              << "\",\"edited_semantic_fingerprint\":\"" << edited << "\"}\n";
        file_.flush();
    }

  private:
    std::ofstream file_;
};

} // namespace a9
