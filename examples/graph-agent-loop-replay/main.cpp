#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp::host;

namespace {
int checks = 0;

void require(bool value, const char* rule) {
    ++checks;
    if (!value)
        throw std::runtime_error(rule);
}

struct Rendered {
    std::vector<std::vector<float>> channels;
    int blocks = 0;
};

struct ResolvedReceipt {
    std::string plan_hash;
    std::string semantic_hash;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    require(file.good(), "open-fixture");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string replace_gain(std::string payload, std::string_view bits) {
    const std::string old = "f32:3f800000";
    const auto offset = payload.find(old);
    require(offset != std::string::npos, "edit-payload-gain");
    payload.replace(offset, old.size(), bits);
    return payload;
}

std::string semantic_fingerprint(const SignalGraph& graph) {
    std::ostringstream bytes;
    for (const auto id : graph.processing_order()) {
        const auto* node = graph.node(id);
        require(node != nullptr, "semantic-node");
        bytes << static_cast<int>(node->type) << ':' << node->name << ':' << node->num_input_ports
              << ':' << node->num_output_ports << ';';
    }
    for (const auto& edge : graph.connections()) {
        const auto* source = graph.node(edge.source_node);
        const auto* destination = graph.node(edge.dest_node);
        require(source != nullptr && destination != nullptr, "semantic-edge-node");
        bytes << source->name << ':' << edge.source_port << '>' << destination->name << ':'
              << edge.dest_port << ':' << edge.feedback << ':' << edge.midi << ':' << edge.automation
              << ':' << edge.audio_rate_modulation << ':' << edge.sidechain << ':' << edge.spectral
              << ';';
    }
    // This is an evidence-only stable digest. It intentionally does not claim
    // to be the graph identity or the resolved execution-plan hash.
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : bytes.str()) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << hash;
    return out.str();
}

Rendered render(SignalGraph& graph, const std::vector<std::vector<float>>& input) {
    OfflineSignalGraphHost host(graph);
    require(host.prepare({.sample_rate = 48000.0,
                          .block_frames = 4,
                          .input_channels = 2,
                          .output_channels = 2}),
            "prepare-render");
    float* input_ptrs[] = {const_cast<float*>(input[0].data()), const_cast<float*>(input[1].data())};
    const pulp::audio::BufferView<const float> view(input_ptrs, 2, input[0].size());
    const auto result = host.render({.frame_count = input[0].size(), .input = view});
    require(result.ok && result.blocks_rendered == 3, "render");
    Rendered rendered;
    rendered.blocks = result.blocks_rendered;
    for (std::size_t channel = 0; channel < 2; ++channel) {
        const auto values = result.audio.channel(static_cast<int>(channel));
        rendered.channels.emplace_back(values.begin(), values.end());
    }
    return rendered;
}

NodeId node_named(const SignalGraph::PreparedTopologyEdit& edit, std::string_view name) {
    for (const auto& node : edit.nodes())
        if (node.name == name)
            return node.id;
    return 0;
}

NodeId node_named(const SignalGraph& graph, std::string_view name) {
    for (const auto& node : graph.nodes())
        if (node.name == name)
            return node.id;
    return 0;
}

AuthoredRuntimeBinding binding_for(const SignalGraph::PreparedTopologyEdit& edit) {
    const auto input = node_named(edit, "input");
    const auto gain_a = node_named(edit, "gain_a");
    const auto gain_b = node_named(edit, "gain_b");
    const auto output = node_named(edit, "output");
    require(input && gain_a && gain_b && output, "binding-nodes");
    AuthoredRuntimeBinding binding;
    binding.nodes = {{input, "input"}, {gain_a, "gain_a"}, {gain_b, "gain_b"}, {output, "output"}};
    const auto add_channels = [&](NodeId id, bool output_direction, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count; ++i)
            binding.channels.push_back({id, "audio", output_direction, i == 0 ? "L" : "R", i, i});
    };
    add_channels(input, true, 2);
    add_channels(gain_a, false, 2);
    add_channels(gain_a, true, 2);
    add_channels(gain_b, false, 2);
    add_channels(gain_b, true, 2);
    add_channels(output, false, 2);
    binding.interfaces = {
        {"main-in", false, input, "audio", "L", 0, 0, 0},
        {"main-in", false, input, "audio", "R", 1, 0, 1},
        {"main-out", true, output, "audio", "L", 0, 0, 0},
        {"main-out", true, output, "audio", "R", 1, 0, 1},
    };
    return binding;
}

ResolvedReceipt resolved_receipt(SignalGraph& graph, const std::string& payload) {
    auto edit = graph.begin_prepared_topology_edit();
    require(edit != nullptr, "begin-resolved-edit");
    const auto bound = edit->bind_authored_identity(payload, binding_for(*edit));
    if (!bound)
        std::cerr << "resolved bind refusal=" << prepared_description_refusal_name(bound.refusal)
                  << '\n';
    require(static_cast<bool>(bound), "bind-resolved-identity");
    require(edit->prepare(48000.0, 4) == SignalGraph::PreparedTopologyEdit::Result::Prepared,
            "prepare-resolved-edit");
    require(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed,
            "commit-resolved-edit");
    const auto snapshot = edit->committed_execution_snapshot();
    require(static_cast<bool>(snapshot), "resolved-snapshot");
    const auto exported = snapshot.export_prepared_description();
    require(static_cast<bool>(exported), "resolved-export");
    require(!exported.value->canonical_bytes().empty(), "resolved-bytes");
    return {exported.value->content_hash(), exported.value->semantic_digest};
}

GraphLoadOptions hermetic_options() {
    GraphLoadOptions options;
    options.load_plugins = false;
    options.include_default_plugin_roots = false;
    options.scan_mode = GraphPluginScanMode::none;
    return options;
}

} // namespace

int main(int argc, char** argv) try {
    require(argc >= 3, "usage");
    const std::filesystem::path output = argv[1];
    const std::filesystem::path plan = argv[2];
    const auto mode = read_file(plan);
    const bool wrong_edit = mode.find("wrong-edit") != std::string::npos;
    const bool wrong_replay = mode.find("wrong-replay") != std::string::npos;
    require(!(wrong_edit && wrong_replay), "plan-mode");
    std::filesystem::create_directories(output);
    const auto baseline_path = output / "baseline.pulpgraph";
    const auto edited_path = output / "edited.pulpgraph";
    const auto baseline_payload = read_file(std::filesystem::path(A9_REPLAY_FIXTURE_DIR) /
                                            "stereo-semantic.json");
    const std::vector<std::vector<float>> input = {
        {1.0f, -0.5f, 0.25f, -0.125f, 0.0625f, -0.03125f, 0.015625f, -0.0078125f, 0.00390625f},
        {-0.25f, 0.125f, -0.0625f, 0.03125f, -0.015625f, 0.0078125f, -0.00390625f, 0.001953125f, -0.0009765625f},
    };

    SignalGraph authored;
    const auto in = authored.add_input_node(2, "input");
    const auto gain_a = authored.add_gain_node("gain_a");
    const auto gain_b = authored.add_gain_node("gain_b");
    const auto out = authored.add_output_node(2, "output");
    require(in && gain_a && gain_b && out, "author-nodes");
    for (std::uint32_t channel = 0; channel < 2; ++channel)
        require(authored.connect(in, channel, gain_a, channel) &&
                    authored.connect(gain_a, channel, gain_b, channel) &&
                    authored.connect(gain_b, channel, out, channel),
                "author-edges");
    require(authored.set_node_gain(gain_a, 1.0f) && authored.set_node_gain(gain_b, 1.0f),
            "author-gain");
    const auto authored_resolved = resolved_receipt(authored, baseline_payload);
    require(GraphSerializer::to_file(authored, baseline_path).ok, "save-baseline");

    SignalGraph baseline_graph;
    require(GraphSerializer::from_file(baseline_graph, baseline_path, hermetic_options()).ok,
            "load-baseline");
    const auto baseline_semantic = semantic_fingerprint(baseline_graph);
    const auto baseline_resolved = resolved_receipt(baseline_graph, baseline_payload);
    require(baseline_resolved.plan_hash == authored_resolved.plan_hash, "save-preserves-plan");
    const auto baseline_render = render(baseline_graph, input);

    require(baseline_graph.set_node_gain(node_named(baseline_graph, "gain_a"), 0.25f),
            "edit-gain");
    require(GraphSerializer::to_file(baseline_graph, edited_path).ok, "save-edited");
    const auto edited_payload = replace_gain(baseline_payload, "f32:3e800000");
    const auto edited_resolved = resolved_receipt(baseline_graph, edited_payload);
    const auto edited_render = render(baseline_graph, input);
    require(edited_render.channels != baseline_render.channels, "edit-changes-render");
    require(semantic_fingerprint(baseline_graph) == baseline_semantic, "edit-preserves-semantic");
    require(edited_resolved.plan_hash != baseline_resolved.plan_hash, "edit-changes-resolved-plan");
    if (wrong_edit)
        require(edited_render.channels == baseline_render.channels, "wrong-edit-oracle");

    SignalGraph replay_graph;
    const auto replay_path = wrong_replay ? edited_path : baseline_path;
    require(GraphSerializer::from_file(replay_graph, replay_path, hermetic_options()).ok,
            "load-replay");
    const auto replay_resolved =
        resolved_receipt(replay_graph, wrong_replay ? edited_payload : baseline_payload);
    const auto replay_render = render(replay_graph, input);
    if (wrong_replay)
        require(replay_render.channels == baseline_render.channels, "wrong-replay-input");
    else
        require(replay_render.channels == baseline_render.channels, "replay-restores-render");
    require(semantic_fingerprint(replay_graph) == baseline_semantic, "replay-restores-semantic");
    require(replay_resolved.plan_hash == baseline_resolved.plan_hash, "replay-restores-resolved-plan");

    std::cout << "graph-agent-loop-replay PASS checks=" << checks
              << " baseline_semantic=" << baseline_semantic
              << " baseline_resolved_plan=" << baseline_resolved.plan_hash
              << " edited_resolved_plan=" << edited_resolved.plan_hash
              << " replay_resolved_plan=" << replay_resolved.plan_hash << "\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "graph-agent-loop-replay FAIL rule=" << error.what() << '\n';
    return 1;
}
