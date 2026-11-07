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

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path);
    require(file.good(), "open-action-plan");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void require_plan_token(const std::string& plan, std::string_view token, const char* rule) {
    require(plan.find(token) != std::string::npos, rule);
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
    // The fingerprint deliberately excludes mutable parameter values. Node IDs are
    // also excluded because a replay is allowed to allocate fresh authoring IDs.
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

void validate_action_plan(const std::string& plan) {
    require_plan_token(plan, "\"schema\": \"pulp.a9.action-plan/v1\"", "plan-schema");
    require_plan_token(plan, "\"model_id\": \"deterministic-offline-actions-v1\"", "model-pin");
    require_plan_token(plan, "\"model_digest\": \"sha256:", "model-digest");
    require_plan_token(plan, "\"tool_id\": \"pulp-graph-agent-loop\"", "tool-pin");
    require_plan_token(plan, "\"tool_version\": \"1\"", "tool-version");
    require_plan_token(plan, "\"source_sha\": \"2f3d36dae75c3dca283c58e47f7731356f088e0d\"",
                       "source-pin");
    require_plan_token(plan, "\"network_policy\": \"deny\"", "network-denial");
    require_plan_token(plan, "\"plugin_policy\": \"deny-ambient\"", "ambient-plugin-denial");
    require_plan_token(plan, "\"catalog-discover\"", "action-catalog");
    require_plan_token(plan, "\"author-graph\"", "action-author");
    require_plan_token(plan, "\"validate-replay\"", "action-validate");
    require_plan_token(plan, "\"render-probe\"", "action-render");
    require_plan_token(plan, "\"bounded-edit\"", "action-edit");
    require_plan_token(plan, "\"saved-input-replay\"", "action-replay");
    require_plan_token(plan, "\"semantic_fingerprint\"", "fingerprint-rule");
    require_plan_token(plan, "\"wrong-edit-oracle\"", "mutation-edit");
    require_plan_token(plan, "\"wrong-replay-input\"", "mutation-replay");
    // A plan is local-only by construction. Reject URLs and an explicit opt-in
    // to network/plugin execution even if a caller edits one policy field.
    require(plan.find("http://") == std::string::npos && plan.find("https://") == std::string::npos,
            "plan-network-url");
    require(plan.find("\"network_policy\": \"allow\"") == std::string::npos, "plan-network-allow");
    require(plan.find("\"plugin_policy\": \"allow-ambient\"") == std::string::npos,
            "plan-plugin-allow");
}

void write_action_log(const std::filesystem::path& path, std::string_view plan_digest,
                      std::string_view baseline_fingerprint, std::string_view edited_fingerprint) {
    std::ofstream file(path);
    require(file.good(), "open-action-log");
    file << "{\"schema\":\"pulp.a9.action-log/v1\",\"plan_digest\":\"" << plan_digest
         << "\",\"network\":\"denied\",\"ambient_plugins\":\"denied\"}\n";
    for (const auto& action : {"catalog-discover", "author-graph", "validate-replay",
                               "render-probe", "bounded-edit", "saved-input-replay"})
        file << "{\"action\":\"" << action << "\",\"status\":\"pass\"}\n";
    file << "{\"semantic_fingerprint\":\"" << baseline_fingerprint
         << "\",\"edited_semantic_fingerprint\":\"" << edited_fingerprint << "\"}\n";
}

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
        argc == 2 ? argv[1] : std::filesystem::temp_directory_path();
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
    validate_action_plan(plan_text);
    const auto plan_digest = fnv1a_hex(plan_text);

    // Catalog discovery is a value-owned, public SDK operation; no private catalog headers
    // or ambient plugin paths are needed by this fixture.
    const auto catalog = forge_catalog_export_nodes();
    require(!catalog.empty(), "catalog-discovery");
    require(audit_catalog_export(catalog).empty(), "catalog-audit");

    const std::vector<float> input = {1.0f,      -0.5f,     0.25f,       -0.125f,    0.0625f,
                                      -0.03125f, 0.015625f, -0.0078125f, 0.00390625f};
    SignalGraph graph;
    make_graph(graph, 0.5f);
    const std::string notation = GraphSerializer::to_json(graph);
    require(!notation.empty(), "notation");
    {
        std::ofstream file(baseline_path);
        require(file.good(), "open-baseline");
        file << notation;
    }

    // Reloading through the file API is the validation step. Plugins are explicitly disabled,
    // so this cannot execute an ambient library while reproducing the saved inputs.
    SignalGraph validated;
    const auto loaded =
        GraphSerializer::from_file(validated, baseline_path, hermetic_load_options());
    require(loaded.ok && loaded.missing_plugins.empty(), "validate-reload");
    const auto canonical = GraphSerializer::to_json(validated);
    require(canonical == notation, "canonical-notation");

    const auto baseline = render(validated, input, 4);
    require(baseline.blocks == 3, "baseline-blocks");
    const auto baseline_fingerprint = semantic_fingerprint(validated);
    const auto stage = node_named(validated, "stage");
    const auto output_node = node_named(validated, "output");
    require(stage != 0 && output_node != 0, "reload-node-names");
    const auto source = validated.node(stage)->out("audio");
    const auto destination = validated.node(output_node)->in("audio");
    OfflineSignalGraphHost probe_host(validated);
    require(
        probe_host.prepare(
            {.sample_rate = 48000.0, .block_frames = 4, .input_channels = 1, .output_channels = 1}),
        "probe-prepare");
    require(validated.attach_audio_edge_probe(source, destination) ==
                SignalGraph::EdgeProbeStatus::Attached,
            "probe-attach");
    float* probe_channels[] = {const_cast<float*>(input.data())};
    const pulp::audio::BufferView<const float> probe_view(probe_channels, 1, input.size());
    const auto probe_result = probe_host.render({.frame_count = input.size(), .input = probe_view});
    require(probe_result.ok, "probe-render");
    Rendered probed;
    probed.samples.assign(probe_result.audio.channel(0).begin(),
                          probe_result.audio.channel(0).end());
    probed.blocks = probe_result.blocks_rendered;
    require(probed.samples == baseline.samples, "probe-render-compare");
    pulp::graph::GraphRuntimeProbeSample sample;
    require(validated.read_audio_edge_probe(sample) && sample.frames == 4, "probe-record");

    // One bounded edit is recorded separately, then the original saved inputs are replayed.
    require(validated.set_node_gain(stage, 0.25f), "bounded-edit");
    const auto edited = render(validated, input, 4);
    require(edited.samples != baseline.samples, "edit-changes-render");
    const auto edited_fingerprint = semantic_fingerprint(validated);
    require(edited_fingerprint == baseline_fingerprint, "edit-preserves-topology");
    {
        std::ofstream file(edited_path);
        require(file.good(), "open-edited");
        file << GraphSerializer::to_json(validated);
    }

    SignalGraph reproduced;
    const auto replay =
        GraphSerializer::from_file(reproduced, baseline_path, hermetic_load_options());
    require(replay.ok, "reproduce-reload");
    const auto reproduced_render = render(reproduced, input, 4);
    require(reproduced_render.samples == baseline.samples, "reproduce-baseline");
    require(reproduced_render.samples != edited.samples, "reproduce-is-before-edit");
    require(semantic_fingerprint(reproduced) == baseline_fingerprint, "replay-preserves-topology");
    write_action_log(action_log_path, plan_digest, baseline_fingerprint, edited_fingerprint);

    std::cout << "graph-agent-loop PASS checks=" << checks << " catalog_nodes=" << catalog.size()
              << " baseline_blocks=" << baseline.blocks << " edited_gain=0.25"
              << " plan_digest=" << plan_digest << " semantic_fingerprint=" << baseline_fingerprint
              << " network=denied ambient_plugins=denied\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "graph-agent-loop FAIL rule=" << error.what() << '\n';
    return 1;
}
