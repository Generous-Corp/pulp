#include <catch2/catch_test_macros.hpp>

#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <string>

using namespace pulp::host;

static std::string stereo_payload() {
    return R"({"assets":{},"edges":{"[\"audio\",{\"direction\":\"out\",\"kind\":\"audio\",\"node\":\"input\",\"port\":\"audio\"},{\"direction\":\"in\",\"kind\":\"audio\",\"node\":\"gain_a\",\"port\":\"audio\"},null]":{"channel_map":[["L","L"],["R","R"]]},"[\"audio\",{\"direction\":\"out\",\"kind\":\"audio\",\"node\":\"gain_a\",\"port\":\"audio\"},{\"direction\":\"in\",\"kind\":\"audio\",\"node\":\"gain_b\",\"port\":\"audio\"},null]":{"channel_map":[["L","L"],["R","R"]]},"[\"audio\",{\"direction\":\"out\",\"kind\":\"audio\",\"node\":\"gain_b\",\"port\":\"audio\"},{\"direction\":\"in\",\"kind\":\"audio\",\"node\":\"output\",\"port\":\"audio\"},null]":{"channel_map":[["L","L"],["R","R"]]}},"interfaces":[{"channels":["L","R"],"endpoint":{"direction":"out","kind":"audio","node":"input","port":"audio"},"key":"main-in","kind":"audio","role":"input"},{"channels":["L","R"],"endpoint":{"direction":"in","kind":"audio","node":"output","port":"audio"},"key":"main-out","kind":"audio","role":"output"}],"nodes":{"gain_a":{"config":{"gain":"f32:3f800000"},"kind":"gain"},"gain_b":{"config":{"gain":"f32:3f800000"},"kind":"gain"},"input":{"kind":"audio_input"},"output":{"kind":"audio_output"}},"policies":{},"profile":"pulp.graph.identity.v1"})";
}

TEST_CASE("D4 empty prepared export is detached and canonical") {
    SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);

    const auto bound =
        edit->bind_authored_identity("{\"assets\":{},\"edges\":{},\"interfaces\":[],\"nodes\":{},"
                                     "\"policies\":{},\"profile\":\"pulp.graph.identity.v1\"}",
                                     {});
    REQUIRE(bound);
    REQUIRE(edit->prepare(48000.0, 128) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);

    auto snapshot = edit->committed_execution_snapshot();
    REQUIRE(snapshot);
    const auto exported = snapshot.export_prepared_description();
    REQUIRE(exported);
    REQUIRE(exported.value->schema == "pulp.graph.prepared-io-gain.v1");
    REQUIRE(exported.value->max_block_frames == 128);
    REQUIRE(exported.value->channels.empty());
    REQUIRE_FALSE(exported.value->canonical_bytes().empty());
    REQUIRE(exported.value->content_hash().size() == 64);
}

TEST_CASE("D4 export rejects a retired owner generation") {
    SignalGraph graph;
    auto first = graph.begin_prepared_topology_edit();
    REQUIRE(first);
    REQUIRE(
        first->bind_authored_identity("{\"assets\":{},\"edges\":{},\"interfaces\":[],\"nodes\":{},"
                                      "\"policies\":{},\"profile\":\"pulp.graph.identity.v1\"}",
                                      {}));
    REQUIRE(first->prepare(48000.0, 128) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(first->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    auto retired = first->committed_execution_snapshot();

    auto second = graph.begin_prepared_topology_edit();
    REQUIRE(second);
    REQUIRE(second->prepare(48000.0, 128) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(second->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    REQUIRE(retired.export_prepared_description().refusal ==
            PreparedDescriptionRefusal::SnapshotChanged);
}

TEST_CASE("D4 refuses an unbound prepared snapshot") {
    SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    REQUIRE(edit->prepare(48000.0, 128) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    const auto snapshot = edit->committed_execution_snapshot();
    const auto exported = snapshot.export_prepared_description();
    REQUIRE(exported.refusal == PreparedDescriptionRefusal::LogicalIdentityMissing);
}

TEST_CASE("D4 binds and exports a closed stereo Gain chain") {
    SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    const auto input = edit->add_input_node(2, "Input");
    const auto gain_a = edit->add_gain_node("Gain A");
    const auto gain_b = edit->add_gain_node("Gain B");
    const auto output = edit->add_output_node(2, "Output");
    REQUIRE(input != 0);
    REQUIRE(gain_a != 0);
    REQUIRE(gain_b != 0);
    REQUIRE(output != 0);
    for (std::uint32_t channel = 0; channel < 2; ++channel) {
        REQUIRE(edit->connect(input, channel, gain_a, channel));
        REQUIRE(edit->connect(gain_a, channel, gain_b, channel));
        REQUIRE(edit->connect(gain_b, channel, output, channel));
    }

    AuthoredRuntimeBinding binding;
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
        {"main", false, input, "audio", "L", 0, 0, 0},
        {"main", false, input, "audio", "R", 1, 0, 1},
        {"main", true, output, "audio", "L", 0, 0, 0},
        {"main", true, output, "audio", "R", 1, 0, 1},
    };
    const auto bound = edit->bind_authored_identity(stereo_payload(), binding);
    INFO(prepared_description_refusal_name(bound.refusal));
    REQUIRE(bound);
    REQUIRE(edit->prepare(48000.0, 128) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    const auto snapshot = edit->committed_execution_snapshot();
    const auto exported = snapshot.export_prepared_description();
    REQUIRE(exported);
    REQUIRE(exported.value->nodes.size() == 4);
    REQUIRE(exported.value->edges.size() == 6);
    REQUIRE(exported.value->channels == std::vector<std::string>{"L", "R"});
    REQUIRE(exported.value->prepared_plan_node_count == 4);
    REQUIRE(exported.value->prepared_plan_connection_count == 6);
    REQUIRE(exported.value->serial_pool_bound);
    REQUIRE(exported.value->input_modes.size() == 6);
    REQUIRE(exported.value->connection_delay_samples.size() == 6);
    REQUIRE(exported.value->reads_in_place.size() == 6);
    REQUIRE(exported.value->pool_slot_count == exported.value->buffer_slot_count);
}

TEST_CASE("D4 rejects a swapped authored channel") {
    SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    const auto input = edit->add_input_node(2);
    const auto output = edit->add_output_node(2);
    REQUIRE(edit->connect(input, 0, output, 0));
    REQUIRE(edit->connect(input, 1, output, 1));
    AuthoredRuntimeBinding binding;
    binding.nodes = {{input, "input"}, {output, "output"}};
    binding.channels = {
        {input, "audio", true, "L", 0, 0},
        {input, "audio", true, "R", 1, 1},
        {output, "audio", false, "L", 0, 0},
        {output, "audio", false, "R", 1, 1},
    };
    binding.interfaces = {
        {"main", false, input, "audio", "L", 0, 0, 0},
        {"main", false, input, "audio", "R", 1, 0, 1},
        {"main", true, output, "audio", "L", 0, 0, 0},
        {"main", true, output, "audio", "R", 1, 0, 1},
    };
    std::swap(binding.channels[0].authored_channel, binding.channels[1].authored_channel);
    REQUIRE(
        edit->bind_authored_identity(
                "{\"assets\":{},\"edges\":{},\"interfaces\":[{\"channels\":[\"L\",\"R\"],\"kind\":"
                "\"audio\",\"role\":\"input\"},{\"channels\":[\"L\",\"R\"],\"kind\":\"audio\","
                "\"role\":\"output\"}],\"nodes\":{\"input\":{\"kind\":\"audio_input\"},\"gain_a\":{"
                "\"kind\":\"gain\"},\"gain_b\":{\"config\":{\"gain\":\"f32:3f800000\"},\"kind\":"
                "\"gain\"},\"output\":{\"kind\":\"audio_"
                "output\"}},\"policies\":{},\"profile\":\"pulp.graph.identity.v1\"}",
                binding)
            .refusal == PreparedDescriptionRefusal::AuthoredBindingMismatch);
}
