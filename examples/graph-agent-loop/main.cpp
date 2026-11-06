#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <pulp/host/forge_catalog_export.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
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
    std::filesystem::create_directories(output);
    const auto baseline_path = output / "a9-baseline.pulpgraph";
    const auto edited_path = output / "a9-edited.pulpgraph";

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

    std::cout << "graph-agent-loop PASS checks=" << checks << " catalog_nodes=" << catalog.size()
              << " baseline_blocks=" << baseline.blocks << " edited_gain=0.25\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "graph-agent-loop FAIL rule=" << error.what() << '\n';
    return 1;
}
