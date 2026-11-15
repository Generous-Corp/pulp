#include "plan_boundary.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <pulp/host/forge_catalog_export.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <vector>

using namespace a9;
using namespace pulp::host;
namespace {
struct Rendered {
    std::vector<float> samples;
    int blocks = 0;
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
        if (step == "catalog-discover") {
            catalog = forge_catalog_export_nodes();
            require(!catalog.empty(), "catalog-discovery");
            require(audit_catalog_export(catalog).empty(), "catalog-audit");
        } else if (step == "author-graph") {
            make_graph(graph, 0.5f);
            notation = GraphSerializer::to_json(graph);
            require(!notation.empty(), "notation");
            std::ofstream file(baseline_path);
            require(file.good(), "open-baseline");
            file << notation;
        } else if (step == "validate-replay") {
            const auto loaded =
                GraphSerializer::from_file(validated, baseline_path, hermetic_load_options());
            require(loaded.ok && loaded.missing_plugins.empty(), "validate-reload");
            require(GraphSerializer::to_json(validated) == notation, "canonical-notation");
            baseline = render(validated, input, 4);
            require(baseline.blocks == 3, "baseline-blocks");
            baseline_fingerprint = semantic_fingerprint(validated);
        } else if (step == "render-probe") {
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
        } else if (step == "bounded-edit") {
            require(stage != 0, "action-edit");
            require(validated.set_node_gain(stage, plan.edit_value), "bounded-edit");
            edited = render(validated, input, 4);
            require(edited.samples != baseline.samples, "edit-changes-render");
            edited_fingerprint = semantic_fingerprint(validated);
            require(edited_fingerprint == baseline_fingerprint, "edit-preserves-topology");
            std::ofstream file(edited_path);
            require(file.good(), "open-edited");
            file << GraphSerializer::to_json(validated);
        } else if (step == "saved-input-replay") {
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
        action_log.record(step);
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
