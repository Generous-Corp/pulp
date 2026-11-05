#include <pulp/audio/buffer.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/host/signal_graph_subgraph.hpp>

#include <cmath>
#include <iostream>
#include <vector>

namespace {

using namespace pulp::host;

SubgraphTemplate stereo_template(bool swap_outputs) {
    SubgraphTemplate recipe;
    recipe.identity = "cookbook.static-stereo.v1";
    recipe.nodes = {{"mono", NodeType::AudioInput, 1, 1.0f},
                    {"left", NodeType::Gain, 1, 1.0f},
                    {"right", NodeType::Gain, 1, 1.0f},
                    {"stereo", NodeType::AudioOutput, 2, 1.0f}};
    recipe.edges = {{"mono", 0, "left", 0},
                    {"mono", 0, "right", 0},
                    {"left", 0, "stereo", static_cast<PortIndex>(swap_outputs ? 1 : 0)},
                    {"right", 0, "stereo", static_cast<PortIndex>(swap_outputs ? 0 : 1)}};
    recipe.exposed_ports = {{"mono_in", "mono", 0, true, PortKind::Audio},
                            {"left_out", "left", 0, true, PortKind::Audio},
                            {"right_out", "right", 0, true, PortKind::Audio}};
    recipe.parameters = {{"left_gain", "left", 0.75f, 0.0f, 1.0f},
                         {"right_gain", "right", 0.25f, 0.0f, 1.0f}};
    return recipe;
}

SubgraphTemplate source_stage_template() {
    SubgraphTemplate recipe;
    recipe.identity = "cookbook.source-stage.v1";
    recipe.nodes = {{"input", NodeType::AudioInput, 1, 1.0f}, {"gain", NodeType::Gain, 1, 1.0f}};
    recipe.edges = {{"input", 0, "gain", 0}};
    recipe.exposed_ports = {{"input", "input", 0, true, PortKind::Audio},
                            {"output", "gain", 0, true, PortKind::Audio}};
    recipe.parameters = {{"gain", "gain", 0.5f, 0.0f, 1.0f}};
    return recipe;
}

SubgraphTemplate sink_stage_template() {
    SubgraphTemplate recipe;
    recipe.identity = "cookbook.sink-stage.v1";
    recipe.nodes = {{"gain", NodeType::Gain, 1, 1.0f}, {"output", NodeType::AudioOutput, 1, 1.0f}};
    recipe.edges = {{"gain", 0, "output", 0}};
    recipe.exposed_ports = {{"input", "gain", 0, false, PortKind::Audio},
                            {"output", "output", 0, false, PortKind::Audio}};
    recipe.parameters = {{"gain", "gain", 0.25f, 0.0f, 1.0f}};
    return recipe;
}

bool matches_impulse(const pulp::audio::Buffer<float>& audio, float left, float right) {
    if (audio.num_channels() != 2 || audio.num_samples() != 32)
        return false;
    for (std::size_t frame = 0; frame < 32; ++frame) {
        const float expected_left = frame == 8 ? left : 0.0f;
        const float expected_right = frame == 8 ? right : 0.0f;
        if (std::abs(audio.channel(0)[frame] - expected_left) > 1.0e-6f ||
            std::abs(audio.channel(1)[frame] - expected_right) > 1.0e-6f)
            return false;
    }
    return true;
}

bool check_recipe(bool swap_outputs) {
    SignalGraph graph;
    auto first = instantiate_subgraph(graph, stereo_template(swap_outputs), "place-a", 0);
    auto second = instantiate_subgraph(graph, stereo_template(false), "place-b", 1);
    if (!first || !second || first.instance.nodes.size() != 4 ||
        second.instance.nodes.size() != 4 || first.instance.ports.at("mono_in").output != true ||
        !first.instance.ports.at("left_out").output ||
        !first.instance.ports.at("right_out").output ||
        first.instance.ports.at("left_out").node == second.instance.ports.at("left_out").node) {
        return false;
    }

    auto silent = second.instance.save_state();
    silent.parameters.at("left_gain") = 0.0f;
    silent.parameters.at("right_gain") = 0.0f;
    if (!restore_subgraph_state(graph, second.instance, silent))
        return false;

    OfflineSignalGraphHost host(graph);
    if (!host.prepare({.sample_rate = 48000.0,
                       .block_frames = 16,
                       .input_channels = 1,
                       .output_channels = 2})) {
        return false;
    }

    std::vector<float> stimulus(32, 0.0f);
    stimulus[8] = 1.0f;
    float* input_ptrs[] = {stimulus.data()};
    pulp::audio::BufferView<const float> input(input_ptrs, 1, stimulus.size());
    const auto first_render = host.render({.frame_count = stimulus.size(), .input = input});
    if (!first_render.ok)
        return false;
    const bool initial_matches = matches_impulse(first_render.audio, 0.75f, 0.25f);
    if (swap_outputs)
        return !initial_matches && matches_impulse(first_render.audio, 0.25f, 0.75f);
    if (!initial_matches)
        return false;

    auto restored = first.instance.save_state();
    restored.parameters.at("left_gain") = 0.0f;
    restored.parameters.at("right_gain") = 1.0f;
    if (!restore_subgraph_state(graph, first.instance, restored) ||
        graph.node_gain(second.instance.parameter_nodes.at("left_gain")) != 0.0f ||
        graph.node_gain(second.instance.parameter_nodes.at("right_gain")) != 0.0f) {
        return false;
    }
    const auto second_render = host.render({.frame_count = stimulus.size(), .input = input});
    if (!second_render.ok || !matches_impulse(second_render.audio, 0.0f, 1.0f))
        return false;

    const auto before_reject = first.instance.save_state();
    auto invalid = before_reject;
    invalid.parameters.at("left_gain") = 2.0f;
    if (restore_subgraph_state(graph, first.instance, invalid) ||
        first.instance.save_state().parameters != before_reject.parameters ||
        graph.node_gain(first.instance.parameter_nodes.at("left_gain")) != 0.0f ||
        graph.node_gain(first.instance.parameter_nodes.at("right_gain")) != 1.0f) {
        return false;
    }
    const auto after_reject = host.render({.frame_count = stimulus.size(), .input = input});
    return after_reject.ok && matches_impulse(after_reject.audio, 0.0f, 1.0f);
}

bool check_composed_stages() {
    SignalGraph graph;
    auto source = instantiate_subgraph(graph, source_stage_template(), "source", 0);
    auto sink = instantiate_subgraph(graph, sink_stage_template(), "sink", 1);
    if (!source || !sink)
        return false;
    const auto before = graph.connections().size();
    const auto connected =
        connect_subgraph_ports(graph, source.instance, "output", sink.instance, "input");
    if (!connected || graph.connections().size() != before + 1)
        return false;

    OfflineSignalGraphHost host(graph);
    if (!host.prepare({.sample_rate = 48000.0,
                       .block_frames = 16,
                       .input_channels = 1,
                       .output_channels = 1}))
        return false;
    std::vector<float> stimulus(32, 0.0f);
    stimulus[8] = 1.0f;
    float* input_ptrs[] = {stimulus.data()};
    pulp::audio::BufferView<const float> input(input_ptrs, 1, stimulus.size());
    const auto render = host.render({.frame_count = stimulus.size(), .input = input});
    if (!render.ok || render.audio.num_channels() != 1 || render.audio.num_samples() != 32)
        return false;
    for (std::size_t frame = 0; frame < 32; ++frame) {
        const float expected = frame == 8 ? 0.125f : 0.0f;
        if (std::abs(render.audio.channel(0)[frame] - expected) > 1.0e-6f)
            return false;
    }

    const auto missing =
        connect_subgraph_ports(graph, source.instance, "missing", sink.instance, "input");
    if (missing || missing.code != GraphResult::Code::UnknownPort ||
        graph.connections().size() != before + 1)
        return false;
    const auto wrong_direction =
        connect_subgraph_ports(graph, sink.instance, "output", source.instance, "output");
    return !wrong_direction && wrong_direction.code == GraphResult::Code::UnknownPort &&
           wrong_direction.message().find("subgraph boundary") != std::string::npos &&
           graph.connections().size() == before + 1;
}

} // namespace

int main() {
    if (!check_recipe(false)) {
        std::cerr << "static stereo recipe failed\n";
        return 1;
    }
    if (!check_recipe(true)) {
        std::cerr << "planted channel swap was not rejected\n";
        return 2;
    }
    if (!check_composed_stages()) {
        std::cerr << "exposed-port composition or planted negatives failed\n";
        return 3;
    }
    std::cout << "static stereo recipe, exposed-port composition, and planted negatives passed\n";
    return 0;
}
