#include <algorithm>
#include <choc/text/choc_JSON.h>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <pulp/host/forge_catalog_export.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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
    std::string string_value() {
        expect('"');
        std::string raw;
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"')
                return raw;
            if (c == '\\') {
                if (position_ >= text_.size())
                    throw std::runtime_error("plan-json-string");
                raw.push_back(c);
                raw.push_back(text_[position_++]);
            } else {
                if (static_cast<unsigned char>(c) < 0x20)
                    throw std::runtime_error("plan-json-string");
                raw.push_back(c);
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
template <typename V> void reject_urls(const V& value) {
    if (value.isString()) {
        const auto text = std::string(value.getString());
        require(text.rfind("http://", 0) != 0 && text.rfind("https://", 0) != 0,
                "plan-network-url");
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
    reject_urls(root);
    require(string_member(root, "schema", "plan-schema") == "pulp.a9.action-plan/v1",
            "plan-schema");
    require(string_member(root, "model_id", "model-pin") == "deterministic-offline-actions-v1",
            "model-pin");
    require(string_member(root, "model_digest", "model-digest").rfind("sha256:", 0) == 0,
            "model-digest");
    require(string_member(root, "tool_id", "tool-pin") == "pulp-graph-agent-loop", "tool-pin");
    require(string_member(root, "tool_version", "tool-version") == "1", "tool-version");
    require(string_member(root, "source_sha", "source-pin") ==
                "2f3d36dae75c3dca283c58e47f7731356f088e0d",
            "source-pin");
    const auto network_policy = string_member(root, "network_policy", "plan-network-policy");
    require(network_policy == "deny",
            network_policy == "allow" ? "plan-network-allow" : "plan-network-policy");
    const auto plugin_policy = string_member(root, "plugin_policy", "plan-plugin-policy");
    require(plugin_policy == "deny-ambient",
            plugin_policy == "allow-ambient" ? "plan-plugin-allow" : "plan-plugin-policy");
    const auto& steps = member(root, "steps", "plan-steps");
    require(steps.isArray(), "plan-steps");
    ActionPlan plan;
    for (uint32_t i = 0; i < steps.size(); ++i) {
        const auto& entry = steps[i];
        require(entry.isObject(), "plan-action-object");
        ActionStep step;
        step.id = string_member(entry, "id", "plan-action-id");
        step.kind = string_member(entry, "kind", "plan-action-kind");
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
    require(replay.kind == "graph.replay" && replay.input == "baseline.pulpgraph", "action-replay");
    plan.replay_input = replay.input;
    const auto& validate =
        *std::find_if(plan.steps.begin(), plan.steps.end(),
                      [](const auto& step) { return step.id == "validate-replay"; });
    require(validate.load_plugins == false, "plan-plugin-load");
    const auto& probe = *std::find_if(plan.steps.begin(), plan.steps.end(),
                                      [](const auto& step) { return step.id == "render-probe"; });
    require(probe.edge_probe, "plan-edge-probe");
    const auto& assertions = member(root, "assertions", "plan-assertions");
    require(assertions.isArray(), "plan-assertions");
    bool fingerprint = false;
    for (uint32_t i = 0; i < assertions.size(); ++i)
        if (assertions[i].isObject() && assertions[i].hasObjectMember("id") &&
            assertions[i]["id"].isString() &&
            assertions[i]["id"].getString() == "semantic_fingerprint")
            fingerprint = true;
    require(fingerprint, "fingerprint-rule");
    const auto& mutations = member(root, "mutations", "plan-mutations");
    require(mutations.isArray() && mutations.size() >= 2, "plan-mutations");
    bool edit_mutation = false, replay_mutation = false;
    for (uint32_t i = 0; i < mutations.size(); ++i) {
        const auto& mutation = mutations[i];
        require(mutation.isObject(), "plan-mutation-object");
        const auto id = string_member(mutation, "id", "plan-mutation-id");
        if (id == "wrong-edit-oracle")
            edit_mutation = true;
        if (id == "wrong-replay-input")
            replay_mutation = true;
    }
    require(edit_mutation, "mutation-edit");
    require(replay_mutation, "mutation-replay");
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
    std::filesystem::create_directories(output);
    const auto baseline_path = output / "a9-baseline.pulpgraph";
    const auto edited_path = output / "a9-edited.pulpgraph";
    const auto action_log_path = output / "a9-action-log.jsonl";
    const auto plan_text = read_text(action_plan);
    const auto plan = parse_action_plan(plan_text);
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
