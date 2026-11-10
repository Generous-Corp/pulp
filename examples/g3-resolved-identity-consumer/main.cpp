#include <array>
#include <iostream>
#include <pulp/host/graph_identity.hpp>
#include <pulp/host/prepared_description.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>
#include <tuple>
static std::string stereo_payload() {
    return R"G3({"assets":{},"edges":{"[\"audio\",{\"direction\":\"out\",\"kind\":\"audio\",\"node\":\"gain_a\",\"port\":\"audio\"},{\"direction\":\"in\",\"kind\":\"audio\",\"node\":\"gain_b\",\"port\":\"audio\"},null]":{"channel_map":[["L","L"],["R","R"]],"destination":{"direction":"in","kind":"audio","node":"gain_b","port":"audio"},"feedback":false,"lane":"audio","semantic_order":null,"source":{"direction":"out","kind":"audio","node":"gain_a","port":"audio"}},"[\"audio\",{\"direction\":\"out\",\"kind\":\"audio\",\"node\":\"gain_b\",\"port\":\"audio\"},{\"direction\":\"in\",\"kind\":\"audio\",\"node\":\"output\",\"port\":\"audio\"},null]":{"channel_map":[["L","L"],["R","R"]],"destination":{"direction":"in","kind":"audio","node":"output","port":"audio"},"feedback":false,"lane":"audio","semantic_order":null,"source":{"direction":"out","kind":"audio","node":"gain_b","port":"audio"}},"[\"audio\",{\"direction\":\"out\",\"kind\":\"audio\",\"node\":\"input\",\"port\":\"audio\"},{\"direction\":\"in\",\"kind\":\"audio\",\"node\":\"gain_a\",\"port\":\"audio\"},null]":{"channel_map":[["L","L"],["R","R"]],"destination":{"direction":"in","kind":"audio","node":"gain_a","port":"audio"},"feedback":false,"lane":"audio","semantic_order":null,"source":{"direction":"out","kind":"audio","node":"input","port":"audio"}}},"interfaces":[{"channels":["L","R"],"endpoint":{"direction":"out","kind":"audio","node":"input","port":"audio"},"key":"main-in","kind":"audio","role":"input"},{"channels":["L","R"],"endpoint":{"direction":"in","kind":"audio","node":"output","port":"audio"},"key":"main-out","kind":"audio","role":"output"}],"nodes":{"gain_a":{"config":{"gain":"f32:3f800000"},"implementation":"pulp.builtin.gain.v1","initial_state":{"mode":"default","revision":"gain.v1"},"kind":"gain","kind_contract":"gain.v1","parameter_bindings":{},"policies":{},"ports":{"in":{"audio":{"channels":["L","R"],"contract":"audio-port.v1","kind":"audio"}},"out":{"audio":{"channels":["L","R"],"contract":"audio-port.v1","kind":"audio"}}}},"gain_b":{"config":{"gain":"f32:3f800000"},"implementation":"pulp.builtin.gain.v1","initial_state":{"mode":"default","revision":"gain.v1"},"kind":"gain","kind_contract":"gain.v1","parameter_bindings":{},"policies":{},"ports":{"in":{"audio":{"channels":["L","R"],"contract":"audio-port.v1","kind":"audio"}},"out":{"audio":{"channels":["L","R"],"contract":"audio-port.v1","kind":"audio"}}}},"input":{"config":{},"implementation":"pulp.builtin.audio-input.v1","initial_state":{"mode":"default","revision":"io.v1"},"kind":"audio_input","kind_contract":"io.v1","parameter_bindings":{},"policies":{},"ports":{"in":{},"out":{"audio":{"channels":["L","R"],"contract":"audio-port.v1","kind":"audio"}}}},"output":{"config":{},"implementation":"pulp.builtin.audio-output.v1","initial_state":{"mode":"default","revision":"io.v1"},"kind":"audio_output","kind_contract":"io.v1","parameter_bindings":{},"policies":{},"ports":{"in":{"audio":{"channels":["L","R"],"contract":"audio-port.v1","kind":"audio"}},"out":{}}}},"policies":{"event_order":"none","fault":"fail","feedback":"forbid","rate_conversion":"none","seed":null,"state_retention":"none"},"profile":"pulp.graph.identity.v1"})G3";
}
int run_stereo() {
    pulp::host::SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    if (!edit)
        return 10;
    const auto input = edit->add_input_node(2), gain_a = edit->add_gain_node(),
               gain_b = edit->add_gain_node(), output = edit->add_output_node(2);
    for (std::uint32_t c = 0; c < 2; ++c)
        if (!edit->connect(input, c, gain_a, c) || !edit->connect(gain_a, c, gain_b, c) ||
            !edit->connect(gain_b, c, output, c))
            return 11;
    pulp::host::AuthoredRuntimeBinding binding;
    binding.nodes = {{input, "input"}, {gain_a, "gain_a"}, {gain_b, "gain_b"}, {output, "output"}};
    for (const auto& item :
         std::array<std::tuple<std::uint64_t, bool, std::uint32_t>, 6>{{{input, true, 2},
                                                                        {gain_a, false, 2},
                                                                        {gain_a, true, 2},
                                                                        {gain_b, false, 2},
                                                                        {gain_b, true, 2},
                                                                        {output, false, 2}}}) {
        for (std::uint32_t i = 0; i < std::get<2>(item); ++i)
            binding.channels.push_back(
                {std::get<0>(item), "audio", std::get<1>(item), i ? "R" : "L", i, i});
    }
    binding.interfaces = {{"main-in", false, input, "audio", "L", 0, 0, 0},
                          {"main-in", false, input, "audio", "R", 1, 0, 1},
                          {"main-out", true, output, "audio", "L", 0, 0, 0},
                          {"main-out", true, output, "audio", "R", 1, 0, 1}};
    if (!edit->bind_authored_identity(stereo_payload(), binding) ||
        edit->prepare(48000, 128) !=
            pulp::host::SignalGraph::PreparedTopologyEdit::Result::Prepared ||
        edit->commit() != pulp::host::SignalGraph::PreparedTopologyEdit::Result::Committed)
        return 12;
    auto description = edit->committed_execution_snapshot().export_prepared_description();
    if (!description || description.value->nodes.size() != 4 ||
        description.value->edges.size() != 6)
        return 13;
    auto identity = pulp::host::resolved_fingerprint(*description.value);
    auto receipt = pulp::host::capture_graph_identity_receipt(*description.value);
    if (!identity || !receipt || identity.value().canonical_bytes().empty() ||
        identity.value().digest().size() != 71)
        return 14;
    std::cout << identity.value().canonical_bytes().size() << " " << identity.value().digest()
              << "\n";
    return 0;
}

int main() {
    pulp::host::PreparedDescriptionData unprepared;
    const auto result = pulp::host::resolved_fingerprint(unprepared);
    if (result || result.error() != pulp::host::GraphIdentityError::UnsupportedPreparedProfile)
        return 1;
    std::cout << pulp::host::graph_identity_error_name(result.error()) << '\n';
    pulp::host::SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    if (!edit ||
        !edit->bind_authored_identity(
            "{\"assets\":{},\"edges\":{},\"interfaces\":[],\"nodes\":{},"
            "\"policies\":{\"event_order\":\"none\",\"fault\":\"fail\",\"feedback\":\"forbid\","
            "\"rate_conversion\":\"none\",\"seed\":null,\"state_retention\":\"none\"},\"profile\":"
            "\"pulp.graph.identity.v1\"}",
            {}) ||
        edit->prepare(48000.0, 128) !=
            pulp::host::SignalGraph::PreparedTopologyEdit::Result::Prepared ||
        edit->commit() != pulp::host::SignalGraph::PreparedTopologyEdit::Result::Committed)
        return 2;
    const auto detached = edit->committed_execution_snapshot().export_prepared_description();
    if (!detached)
        return 3;
    const auto resolved = pulp::host::resolved_fingerprint(*detached.value);
    const auto receipt = pulp::host::capture_graph_identity_receipt(*detached.value);
    if (!resolved || !receipt || receipt.value().resolved.digest() != resolved.value().digest())
        return 4;
    std::cout << receipt.value().canonical_bytes().size() << '\n';
    return run_stereo();
}
