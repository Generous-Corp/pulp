#include <algorithm>
#include <array>
#include <cctype>
#include <choc/text/choc_JSON.h>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <initializer_list>
#include <pulp/host/forge_catalog_export.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_set>
#include <vector>

using namespace pulp::host;

namespace {
int checks = 0;
void require(bool condition, const char* rule) {
    ++checks;
    if (!condition)
        throw std::runtime_error(rule);
}
struct Rendered {
    std::vector<float> samples;
    int blocks = 0;
};
struct ActionStep {
    std::string id, kind, target, input;
    double value = 0.0;
    bool has_value = false, load_plugins = false, edge_probe = false;
};
struct ActionPlan {
    std::vector<ActionStep> steps;
    float edit_value = 0.0f;
    std::string edit_target, replay_input;
};

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path);
    require(file.good(), "open-action-plan");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// choc's parser supplies syntax and value typing. This lexical pass adds the
// strict duplicate-key rule required by the action-plan boundary.
class DuplicateKeyScanner {
  public:
    explicit DuplicateKeyScanner(std::string_view text) : text_(text) {}
    void scan() {
        value();
        whitespace();
        if (position_ != text_.size())
            throw std::runtime_error("plan-json-trailing-data");
    }

  private:
    std::string_view text_;
    std::size_t position_ = 0;
    void whitespace() {
        while (position_ < text_.size()) {
            const auto c = static_cast<unsigned char>(text_[position_]);
            if (c != ' ' && c != '\n' && c != '\r' && c != '\t')
                break;
            ++position_;
        }
    }
    void expect(char expected) {
        whitespace();
        if (position_ >= text_.size() || text_[position_] != expected)
            throw std::runtime_error("plan-json-structure");
        ++position_;
    }
    static int hex_digit(char c) {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }
    void append_codepoint(std::string& out, std::uint32_t codepoint) {
        if (codepoint <= 0x7f) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0x10ffff) {
            out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            throw std::runtime_error("plan-json-unicode");
        }
    }
    std::uint32_t unicode_escape() {
        if (position_ + 4 > text_.size())
            throw std::runtime_error("plan-json-unicode");
        std::uint32_t codepoint = 0;
        for (int i = 0; i < 4; ++i) {
            const auto digit = hex_digit(text_[position_++]);
            if (digit < 0)
                throw std::runtime_error("plan-json-unicode");
            codepoint = (codepoint << 4) | static_cast<std::uint32_t>(digit);
        }
        return codepoint;
    }
    std::string string_value() {
        expect('"');
        std::string decoded;
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"')
                return decoded;
            if (c == '\\') {
                if (position_ >= text_.size())
                    throw std::runtime_error("plan-json-string");
                const char escaped = text_[position_++];
                switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    decoded.push_back(escaped);
                    break;
                case 'b':
                    decoded.push_back('\b');
                    break;
                case 'f':
                    decoded.push_back('\f');
                    break;
                case 'n':
                    decoded.push_back('\n');
                    break;
                case 'r':
                    decoded.push_back('\r');
                    break;
                case 't':
                    decoded.push_back('\t');
                    break;
                case 'u': {
                    const auto first = unicode_escape();
                    if (first >= 0xd800 && first <= 0xdbff) {
                        if (position_ + 2 > text_.size() || text_[position_] != '\\' ||
                            text_[position_ + 1] != 'u')
                            throw std::runtime_error("plan-json-unicode");
                        position_ += 2;
                        const auto second = unicode_escape();
                        if (second < 0xdc00 || second > 0xdfff)
                            throw std::runtime_error("plan-json-unicode");
                        append_codepoint(decoded,
                                         0x10000 + ((first - 0xd800) << 10) + (second - 0xdc00));
                    } else if (first >= 0xdc00 && first <= 0xdfff) {
                        throw std::runtime_error("plan-json-unicode");
                    } else {
                        append_codepoint(decoded, first);
                    }
                    break;
                }
                default:
                    throw std::runtime_error("plan-json-string");
                }
            } else {
                if (static_cast<unsigned char>(c) < 0x20)
                    throw std::runtime_error("plan-json-string");
                decoded.push_back(c);
            }
        }
        throw std::runtime_error("plan-json-string");
    }
    void value() {
        whitespace();
        if (position_ >= text_.size())
            throw std::runtime_error("plan-json-value");
        switch (text_[position_]) {
        case '{':
            object();
            return;
        case '[':
            array();
            return;
        case '"':
            (void)string_value();
            return;
        default:
            break;
        }
        const auto start = position_;
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c == ',' || c == ']' || c == '}' || c == ' ' || c == '\n' || c == '\r' || c == '\t')
                break;
            ++position_;
        }
        if (position_ == start)
            throw std::runtime_error("plan-json-value");
    }
    void array() {
        expect('[');
        whitespace();
        if (position_ < text_.size() && text_[position_] == ']') {
            ++position_;
            return;
        }
        for (;;) {
            value();
            whitespace();
            if (position_ < text_.size() && text_[position_] == ']') {
                ++position_;
                return;
            }
            expect(',');
        }
    }
    void object() {
        expect('{');
        std::unordered_set<std::string> keys;
        whitespace();
        if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            return;
        }
        for (;;) {
            whitespace();
            const auto key = string_value();
            if (!keys.insert(key).second)
                throw std::runtime_error("plan-duplicate-key");
            expect(':');
            value();
            whitespace();
            if (position_ < text_.size() && text_[position_] == '}') {
                ++position_;
                return;
            }
            expect(',');
        }
    }
};

template <typename V> auto member(const V& object, const char* key, const char* rule) {
    require(object.isObject() && object.hasObjectMember(key), rule);
    return object[key];
}
template <typename V>
void only_fields(const V& object, std::initializer_list<std::string_view> allowed,
                 const char* rule) {
    require(object.isObject(), rule);
    for (uint32_t i = 0; i < object.size(); ++i) {
        const auto name = object.getObjectMemberAt(i).name;
        require(std::find(allowed.begin(), allowed.end(), name) != allowed.end(), rule);
    }
}
template <typename V>
std::string string_member(const V& object, const char* key, const char* rule) {
    const auto value = member(object, key, rule);
    require(value.isString(), rule);
    return std::string(value.getString());
}
template <typename V> bool bool_member(const V& object, const char* key, const char* rule) {
    const auto value = member(object, key, rule);
    require(value.isBool(), rule);
    return value.getBool();
}
template <typename V> double number_member(const V& object, const char* key, const char* rule) {
    const auto value = member(object, key, rule);
    require(value.isInt32() || value.isInt64() || value.isFloat32() || value.isFloat64(), rule);
    return value.isFloat64() || value.isFloat32() ? value.getFloat64()
                                                  : static_cast<double>(value.getInt64());
}
std::string lower_ascii(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const auto c : text)
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return result;
}
bool is_hex_string(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](const char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}
bool is_uri_or_network_string(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
        ++first;
    value.remove_prefix(first);
    if (value.size() >= 2 &&
        ((value[0] == '/' && value[1] == '/') || (value[0] == '\\' && value[1] == '\\')))
        return true;
    if (!value.empty() && value[0] == '[')
        return true;
    const auto colon = value.find(':');
    if (colon != std::string_view::npos && std::isdigit(static_cast<unsigned char>(value[0]))) {
        const auto host = value.substr(0, colon);
        const auto port = value.substr(colon + 1);
        const auto host_like = std::all_of(host.begin(), host.end(), [](const char c) {
            return std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == ':' ||
                   (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        });
        const auto port_end = port.find_first_of("/\\?#");
        const auto port_number = port.substr(0, port_end);
        if (host_like && !port_number.empty() &&
            std::all_of(port_number.begin(), port_number.end(),
                        [](const char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            return true;
    }
    if (colon == std::string_view::npos || colon == 0 ||
        !std::isalpha(static_cast<unsigned char>(value[0])))
        return false;
    for (std::size_t i = 1; i < colon; ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (!std::isalnum(c) && value[i] != '+' && value[i] != '-' && value[i] != '.')
            return false;
    }
    // sha256:<64 hex digits> is the plan's content-addressed model pin, not a URI.
    return !(lower_ascii(value.substr(0, colon)) == "sha256" && value.size() == colon + 1 + 64 &&
             is_hex_string(value.substr(colon + 1)));
}
template <typename V> void reject_urls(const V& value) {
    if (value.isString()) {
        const auto text = std::string(value.getString());
        require(!is_uri_or_network_string(text), "plan-network-url");
    } else if (value.isArray())
        for (uint32_t i = 0; i < value.size(); ++i)
            reject_urls(value[i]);
    else if (value.isObject())
        for (uint32_t i = 0; i < value.size(); ++i)
            reject_urls(value.getObjectMemberAt(i).value);
}

ActionPlan parse_action_plan(const std::string& text) {
    DuplicateKeyScanner(text).scan();
    choc::value::Value root;
    try {
        root = choc::json::parse(text);
    } catch (...) {
        throw std::runtime_error("plan-json-parse");
    }
    require(root.isObject(), "plan-object");
    only_fields(root,
                {"schema", "model_id", "model_digest", "tool_id", "tool_version", "source_sha",
                 "network_policy", "plugin_policy", "steps", "assertions", "mutations"},
                "plan-field");
    require(string_member(root, "schema", "plan-schema") == "pulp.a9.action-plan/v1",
            "plan-schema");
    require(string_member(root, "model_id", "model-pin") == "deterministic-offline-actions-v1",
            "model-pin");
    const auto model_digest = string_member(root, "model_digest", "model-digest");
    require(model_digest.size() == 71 && model_digest.rfind("sha256:", 0) == 0 &&
                is_hex_string(std::string_view(model_digest).substr(7)),
            "model-digest");
    require(string_member(root, "tool_id", "tool-pin") == "pulp-graph-agent-loop", "tool-pin");
    require(string_member(root, "tool_version", "tool-version") == "1", "tool-version");
    require(string_member(root, "source_sha", "source-pin") ==
                "2f3d36dae75c3dca283c58e47f7731356f088e0d",
            "source-pin");
    const auto network_policy = string_member(root, "network_policy", "plan-network-policy");
    const auto network_policy_lower = lower_ascii(network_policy);
    require(network_policy_lower == "deny",
            network_policy_lower == "allow" ? "plan-network-allow" : "plan-network-policy");
    const auto plugin_policy = string_member(root, "plugin_policy", "plan-plugin-policy");
    const auto plugin_policy_lower = lower_ascii(plugin_policy);
    require(plugin_policy_lower == "deny-ambient",
            plugin_policy_lower == "allow-ambient" ? "plan-plugin-allow" : "plan-plugin-policy");
    // Apply the URI/network fence after validating the digest and policy fields so a malformed
    // digest cannot be mistaken for a URI scheme, while all other strings remain fenced.
    reject_urls(root);
    const auto& steps = member(root, "steps", "plan-steps");
    require(steps.isArray(), "plan-steps");
    constexpr std::array<std::pair<std::string_view, std::string_view>, 6> expected_steps = {{
        {"catalog-discover", "catalog.discover"},
        {"author-graph", "graph.author"},
        {"validate-replay", "graph.validate"},
        {"render-probe", "graph.render"},
        {"bounded-edit", "graph.edit"},
        {"saved-input-replay", "graph.replay"},
    }};
    require(steps.size() == expected_steps.size(), "action-catalog");
    ActionPlan plan;
    for (uint32_t i = 0; i < steps.size(); ++i) {
        const auto& entry = steps[i];
        require(entry.isObject(), "plan-action-object");
        only_fields(entry, {"id", "kind", "target", "input", "value", "load_plugins", "edge_probe"},
                    "plan-action-field");
        ActionStep step;
        step.id = string_member(entry, "id", "plan-action-id");
        step.kind = string_member(entry, "kind", "plan-action-kind");
        require(step.id == expected_steps[i].first, "action-catalog");
        require(step.kind == expected_steps[i].second, "action-kind");
        require(std::none_of(plan.steps.begin(), plan.steps.end(),
                             [&](const auto& prior) { return prior.id == step.id; }),
                "plan-action-duplicate");
        if (entry.hasObjectMember("target"))
            step.target = string_member(entry, "target", "plan-action-target");
        if (entry.hasObjectMember("input"))
            step.input = string_member(entry, "input", "plan-action-input");
        if (entry.hasObjectMember("value")) {
            step.value = number_member(entry, "value", "plan-action-value");
            step.has_value = true;
        }
        if (entry.hasObjectMember("load_plugins"))
            step.load_plugins = bool_member(entry, "load_plugins", "plan-plugin-load");
        if (entry.hasObjectMember("edge_probe"))
            step.edge_probe = bool_member(entry, "edge_probe", "plan-edge-probe");
        if (i == 0 || i == 1)
            require(!entry.hasObjectMember("target") && !entry.hasObjectMember("input") &&
                        !entry.hasObjectMember("value") && !entry.hasObjectMember("load_plugins") &&
                        !entry.hasObjectMember("edge_probe"),
                    "plan-action-fields");
        if (i == 2)
            require(!entry.hasObjectMember("target") && !entry.hasObjectMember("input") &&
                        !entry.hasObjectMember("value") && entry.hasObjectMember("load_plugins") &&
                        !step.load_plugins && !entry.hasObjectMember("edge_probe"),
                    "plan-plugin-load");
        if (i == 3)
            require(!entry.hasObjectMember("target") && !entry.hasObjectMember("input") &&
                        !entry.hasObjectMember("value") && !entry.hasObjectMember("load_plugins") &&
                        entry.hasObjectMember("edge_probe") && step.edge_probe,
                    "plan-edge-probe");
        if (i == 4)
            require(!entry.hasObjectMember("input") && !entry.hasObjectMember("load_plugins") &&
                        !entry.hasObjectMember("edge_probe") && entry.hasObjectMember("target") &&
                        entry.hasObjectMember("value"),
                    "action-edit");
        if (i == 5)
            require(!entry.hasObjectMember("target") && !entry.hasObjectMember("value") &&
                        !entry.hasObjectMember("load_plugins") &&
                        !entry.hasObjectMember("edge_probe") && entry.hasObjectMember("input"),
                    "action-replay");
        plan.steps.push_back(std::move(step));
    }
    const auto has_step = [&](std::string_view id) {
        return std::any_of(plan.steps.begin(), plan.steps.end(),
                           [&](const auto& step) { return step.id == id; });
    };
    require(has_step("catalog-discover"), "action-catalog");
    require(has_step("author-graph"), "action-author");
    require(has_step("validate-replay"), "action-validate");
    require(has_step("render-probe"), "action-render");
    require(has_step("bounded-edit"), "action-edit");
    require(has_step("saved-input-replay"), "action-replay");
    const auto& edit = *std::find_if(plan.steps.begin(), plan.steps.end(),
                                     [](const auto& step) { return step.id == "bounded-edit"; });
    require(edit.kind == "graph.edit" && edit.target == "stage.gain" && edit.has_value,
            "action-edit");
    require(std::isfinite(edit.value), "action-edit-value");
    plan.edit_target = edit.target;
    plan.edit_value = static_cast<float>(edit.value);
    const auto& replay = *std::find_if(plan.steps.begin(), plan.steps.end(), [](const auto& step) {
        return step.id == "saved-input-replay";
    });
    require(replay.kind == "graph.replay" && replay.input == "a9-baseline.pulpgraph",
            "action-replay");
    plan.replay_input = replay.input;
    const auto& validate =
        *std::find_if(plan.steps.begin(), plan.steps.end(),
                      [](const auto& step) { return step.id == "validate-replay"; });
    require(validate.load_plugins == false, "plan-plugin-load");
    const auto& probe = *std::find_if(plan.steps.begin(), plan.steps.end(),
                                      [](const auto& step) { return step.id == "render-probe"; });
    require(probe.edge_probe, "plan-edge-probe");
    const auto& assertions = member(root, "assertions", "plan-assertions");
    require(assertions.isArray() && assertions.size() == 1, "plan-assertions");
    const auto& fingerprint = assertions[0];
    only_fields(fingerprint, {"id", "relation"}, "plan-assertion-field");
    require(string_member(fingerprint, "id", "fingerprint-rule") == "semantic_fingerprint",
            "fingerprint-rule");
    require(string_member(fingerprint, "relation", "fingerprint-relation") ==
                "baseline == replay; edit preserves topology",
            "fingerprint-relation");
    const auto& mutations = member(root, "mutations", "plan-mutations");
    require(mutations.isArray() && mutations.size() == 2, "plan-mutations");
    constexpr std::array<std::array<std::string_view, 3>, 2> expected_mutations = {{
        {"wrong-edit-oracle", "edit-changes-render", "reject"},
        {"wrong-replay-input", "replay-baseline", "reject"},
    }};
    for (uint32_t i = 0; i < mutations.size(); ++i) {
        const auto& mutation = mutations[i];
        require(mutation.isObject(), "plan-mutation-object");
        only_fields(mutation, {"id", "target", "expected"}, "plan-mutation-field");
        const auto id = string_member(mutation, "id", "plan-mutation-id");
        require(id == expected_mutations[i][0], "mutation-catalog");
        require(string_member(mutation, "target", "mutation-target") == expected_mutations[i][1],
                "mutation-target");
        require(string_member(mutation, "expected", "mutation-expected") ==
                    expected_mutations[i][2],
                "mutation-expected");
    }
    return plan;
}

std::string fnv1a_hex(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}
std::string semantic_fingerprint(const SignalGraph& graph) {
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

Rendered render(SignalGraph& graph, const std::vector<float>& input, int block_frames) {
    OfflineSignalGraphHost host(graph);
    require(host.prepare({.sample_rate = 48000.0,
                          .block_frames = block_frames,
                          .input_channels = 1,
                          .output_channels = 1}),
            "prepare");
    float* channels[] = {const_cast<float*>(input.data())};
    const pulp::audio::BufferView<const float> view(channels, 1, input.size());
    const auto result = host.render({.frame_count = input.size(), .input = view});
    require(result.ok, "render");
    Rendered rendered;
    rendered.blocks = result.blocks_rendered;
    const auto channel = result.audio.channel(0);
    rendered.samples.assign(channel.begin(), channel.end());
    return rendered;
}
void make_graph(SignalGraph& graph, float gain) {
    const auto input = graph.add_input_node(1, "input");
    const auto stage = graph.add_gain_node("stage");
    const auto output = graph.add_output_node(1, "output");
    require(graph.connect(input, 0, stage, 0) && graph.connect(stage, 0, output, 0),
            "author-topology");
    require(graph.set_node_gain(stage, gain), "author-gain");
}
NodeId node_named(const SignalGraph& graph, const std::string& name) {
    for (const auto id : graph.processing_order()) {
        const auto* node = graph.node(id);
        if (node != nullptr && node->name == name)
            return id;
    }
    return 0;
}
GraphLoadOptions hermetic_load_options() {
    GraphLoadOptions options;
    options.load_plugins = false;
    options.include_default_plugin_roots = false;
    options.scan_mode = GraphPluginScanMode::none;
    return options;
}
} // namespace

int main(int argc, char** argv) try {
    const std::filesystem::path output =
        argc >= 2 ? argv[1] : std::filesystem::temp_directory_path();
#ifdef A9_ACTION_PLAN_PATH
    constexpr const char* kDefaultActionPlan = A9_ACTION_PLAN_PATH;
#else
    constexpr const char* kDefaultActionPlan = "fixtures/action-plan.json";
#endif
    const auto action_plan =
        argc >= 3 ? std::filesystem::path(argv[2]) : std::filesystem::path(kDefaultActionPlan);
    const auto baseline_path = output / "a9-baseline.pulpgraph";
    const auto edited_path = output / "a9-edited.pulpgraph";
    const auto action_log_path = output / "a9-action-log.jsonl";
    const auto plan_text = read_text(action_plan);
    const auto plan = parse_action_plan(plan_text);
    std::filesystem::create_directories(output);
    const auto plan_digest = fnv1a_hex(plan_text);
    ActionLog action_log(action_log_path, plan_digest);
    decltype(forge_catalog_export_nodes()) catalog;
    SignalGraph graph;
    SignalGraph validated;
    SignalGraph reproduced;
    Rendered baseline;
    Rendered edited;
    std::string notation, baseline_fingerprint, edited_fingerprint;
    NodeId stage = 0;
    NodeId output_node = 0;
    const std::vector<float> input = {1.0f,      -0.5f,     0.25f,       -0.125f,    0.0625f,
                                      -0.03125f, 0.015625f, -0.0078125f, 0.00390625f};
    for (const auto& step : plan.steps) {
        if (step.id == "catalog-discover") {
            catalog = forge_catalog_export_nodes();
            require(!catalog.empty(), "catalog-discovery");
            require(audit_catalog_export(catalog).empty(), "catalog-audit");
        } else if (step.id == "author-graph") {
            make_graph(graph, 0.5f);
            notation = GraphSerializer::to_json(graph);
            require(!notation.empty(), "notation");
            std::ofstream file(baseline_path);
            require(file.good(), "open-baseline");
            file << notation;
        } else if (step.id == "validate-replay") {
            const auto loaded =
                GraphSerializer::from_file(validated, baseline_path, hermetic_load_options());
            require(loaded.ok && loaded.missing_plugins.empty(), "validate-reload");
            require(GraphSerializer::to_json(validated) == notation, "canonical-notation");
            baseline = render(validated, input, 4);
            require(baseline.blocks == 3, "baseline-blocks");
            baseline_fingerprint = semantic_fingerprint(validated);
        } else if (step.id == "render-probe") {
            stage = node_named(validated, "stage");
            output_node = node_named(validated, "output");
            require(stage != 0 && output_node != 0, "reload-node-names");
            const auto source = validated.node(stage)->out("audio");
            const auto destination = validated.node(output_node)->in("audio");
            OfflineSignalGraphHost probe_host(validated);
            require(probe_host.prepare({.sample_rate = 48000.0,
                                        .block_frames = 4,
                                        .input_channels = 1,
                                        .output_channels = 1}),
                    "probe-prepare");
            require(validated.attach_audio_edge_probe(source, destination) ==
                        SignalGraph::EdgeProbeStatus::Attached,
                    "probe-attach");
            float* probe_channels[] = {const_cast<float*>(input.data())};
            const pulp::audio::BufferView<const float> probe_view(probe_channels, 1, input.size());
            const auto probe_result =
                probe_host.render({.frame_count = input.size(), .input = probe_view});
            require(probe_result.ok, "probe-render");
            require(std::vector<float>(probe_result.audio.channel(0).begin(),
                                       probe_result.audio.channel(0).end()) == baseline.samples,
                    "probe-render-compare");
            pulp::graph::GraphRuntimeProbeSample sample;
            require(validated.read_audio_edge_probe(sample) && sample.frames == 4, "probe-record");
        } else if (step.id == "bounded-edit") {
            require(stage != 0 && plan.edit_target == "stage.gain", "action-edit");
            require(validated.set_node_gain(stage, plan.edit_value), "bounded-edit");
            edited = render(validated, input, 4);
            require(edited.samples != baseline.samples, "edit-changes-render");
            edited_fingerprint = semantic_fingerprint(validated);
            require(edited_fingerprint == baseline_fingerprint, "edit-preserves-topology");
            std::ofstream file(edited_path);
            require(file.good(), "open-edited");
            file << GraphSerializer::to_json(validated);
        } else if (step.id == "saved-input-replay") {
            require(plan.replay_input == baseline_path.filename().string(), "action-replay");
            const auto replay =
                GraphSerializer::from_file(reproduced, baseline_path, hermetic_load_options());
            require(replay.ok, "reproduce-reload");
            const auto reproduced_render = render(reproduced, input, 4);
            require(reproduced_render.samples == baseline.samples, "reproduce-baseline");
            require(reproduced_render.samples != edited.samples, "reproduce-is-before-edit");
            require(semantic_fingerprint(reproduced) == baseline_fingerprint,
                    "replay-preserves-topology");
        } else
            throw std::runtime_error("plan-action-unknown");
        action_log.record(step.id);
    }
    action_log.fingerprint(baseline_fingerprint, edited_fingerprint);
    require(std::filesystem::exists(action_log_path), "action-log-exists");
    std::cout << "graph-agent-loop PASS checks=" << checks << " catalog_nodes=" << catalog.size()
              << " baseline_blocks=" << baseline.blocks << " edited_gain=" << plan.edit_value
              << " plan_digest=" << plan_digest << " semantic_fingerprint=" << baseline_fingerprint
              << " action_log=" << action_log_path << " network=denied ambient_plugins=denied\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "graph-agent-loop FAIL rule=" << error.what() << '\n';
    return 1;
}
