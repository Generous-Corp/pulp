#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <cstdint>
#include <iostream>
#include <string>

namespace {
std::string stereo_payload() {
    return R"G3({"assets":{},"edges":{"[\"audio\",{\"direction\":\"out\",\"kind\":\"audio\",\"node\":\"gain_a\",\"port\":\"audio\"},{\"direction\":\"in\",\"kind\":\"audio\",\"node\":\"gain_b\",\"port\":\"audio\"},null]":{"channel_map":[["L","L"],["R","R"]],"destination":{"direction":"in","kind":"audio","node":"gain_b","port":"audio"},"feedback":false,"lane":"audio","semantic_order":null,"source":{"direction":"out","kind":"audio","node":"gain_a","port":"audio"}},"[\"audio\",{\"direction\":\"out\",\"kind\":\"audio\",\"node\":\"gain_b\",\"port\":\"audio\"},{\"direction\":\"in\",\"kind\":\"audio\",\"node\":\"output\",\"port\":\"audio\"},null]":{"channel_map":[["L","L"],["R","R"]],"destination":{"direction":"in","kind":"audio","node":"output","port":"audio"},"feedback":false,"lane":"audio","semantic_order":null,"source":{"direction":"out","kind":"audio","node":"gain_b","port":"audio"}},"[\"audio\",{\"direction\":\"out\",\"kind\":\"audio\",\"node\":\"input\",\"port\":\"audio\"},{\"direction\":\"in\",\"kind\":\"audio\",\"node\":\"gain_a\",\"port\":\"audio\"},null]":{"channel_map":[["L","L"],["R","R"]],"destination":{"direction":"in","kind":"audio","node":"gain_a","port":"audio"},"feedback":false,"lane":"audio","semantic_order":null,"source":{"direction":"out","kind":"audio","node":"input","port":"audio"}}},"interfaces":[{"channels":["L","R"],"endpoint":{"direction":"out","kind":"audio","node":"input","port":"audio"},"key":"main-in","kind":"audio","role":"input"},{"channels":["L","R"],"endpoint":{"direction":"in","kind":"audio","node":"output","port":"audio"},"key":"main-out","kind":"audio","role":"output"}],"nodes":{"gain_a":{"config":{"gain":"f32:3f800000"},"implementation":"pulp.builtin.gain.v1","initial_state":{"mode":"default","revision":"gain.v1"},"kind":"gain","kind_contract":"gain.v1","parameter_bindings":{},"policies":{},"ports":{"in":{"audio":{"channels":["L","R"],"contract":"audio-port.v1","kind":"audio"}},"out":{"audio":{"channels":["L","R"],"contract":"audio-port.v1","kind":"audio"}}}},"gain_b":{"config":{"gain":"f32:3f800000"},"implementation":"pulp.builtin.gain.v1","initial_state":{"mode":"default","revision":"gain.v1"},"kind":"gain","kind_contract":"gain.v1","parameter_bindings":{},"policies":{},"ports":{"in":{"audio":{"channels":["L","R"],"contract":"audio-port.v1","kind":"audio"}},"out":{"audio":{"channels":["L","R"],"contract":"audio-port.v1","kind":"audio"}}}},"input":{"config":{},"implementation":"pulp.builtin.audio-input.v1","initial_state":{"mode":"default","revision":"io.v1"},"kind":"audio_input","kind_contract":"io.v1","parameter_bindings":{},"policies":{},"ports":{"in":{},"out":{"audio":{"channels":["L","R"],"contract":"audio-port.v1","kind":"audio"}}}},"output":{"config":{},"implementation":"pulp.builtin.audio-output.v1","initial_state":{"mode":"default","revision":"io.v1"},"kind":"audio_output","kind_contract":"io.v1","parameter_bindings":{},"policies":{},"ports":{"in":{"audio":{"channels":["L","R"],"contract":"audio-port.v1","kind":"audio"}},"out":{}}}},"policies":{"event_order":"none","fault":"fail","feedback":"forbid","rate_conversion":"none","seed":null,"state_retention":"none"},"profile":"pulp.graph.identity.v1"})G3";
}

int run_empty() {
    pulp::host::SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    if (!edit)
        return 1;
    if (!edit->bind_authored_identity("{\"assets\":{},\"edges\":{},\"interfaces\":[],\"nodes\":{},"
                                      "\"policies\":{\"event_order\":\"none\",\"fault\":\"fail\",\"feedback\":\"forbid\",\"rate_conversion\":\"none\",\"seed\":null,\"state_retention\":\"none\"},\"profile\":\"pulp.graph.identity.v1\"}",
                                      {}))
        return 2;
    if (edit->prepare(48000.0, 128) !=
        pulp::host::SignalGraph::PreparedTopologyEdit::Result::Prepared)
        return 3;
    if (edit->commit() != pulp::host::SignalGraph::PreparedTopologyEdit::Result::Committed)
        return 4;
    const auto description = edit->committed_execution_snapshot().export_prepared_description();
    if (!description)
        return 5;
    std::cout << description.value->canonical_bytes() << '\n';
    return 0;
}

int run_stereo() {
    pulp::host::SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    if (!edit)
        return 10;
    const auto input = edit->add_input_node(2);
    const auto gain_a = edit->add_gain_node();
    const auto gain_b = edit->add_gain_node();
    const auto output = edit->add_output_node(2);
    for (std::uint32_t channel = 0; channel < 2; ++channel) {
        if (!edit->connect(input, channel, gain_a, channel) ||
            !edit->connect(gain_a, channel, gain_b, channel) ||
            !edit->connect(gain_b, channel, output, channel))
            return 11;
    }
    pulp::host::AuthoredRuntimeBinding binding;
    binding.nodes = {{input, "input"}, {gain_a, "gain_a"}, {gain_b, "gain_b"}, {output, "output"}};
    const auto add_channels = [&](std::uint64_t node, bool output_direction, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count; ++i)
            binding.channels.push_back({node, "audio", output_direction, i == 0 ? "L" : "R", i, i});
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
    if (!edit->bind_authored_identity(stereo_payload(), binding) ||
        edit->prepare(48000.0, 128) !=
            pulp::host::SignalGraph::PreparedTopologyEdit::Result::Prepared ||
        edit->commit() != pulp::host::SignalGraph::PreparedTopologyEdit::Result::Committed)
        return 12;
    const auto description = edit->committed_execution_snapshot().export_prepared_description();
    if (!description || description.value->nodes.size() != 4 ||
        description.value->edges.size() != 6)
        return 13;
    std::cout << description.value->canonical_bytes() << '\n';
    return 0;
}
} // namespace

int main() {
    const auto empty = run_empty();
    if (empty != 0)
        return empty;
    return run_stereo();
}
