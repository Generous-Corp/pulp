#include <catch2/catch_test_macros.hpp>

#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

using namespace pulp::host;

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
    const auto bound = edit->bind_authored_identity(
        "{\"assets\":{},\"edges\":{},\"interfaces\":[{\"channels\":[\"L\",\"R\"],\"kind\":"
        "\"audio\",\"role\":\"input\"},{\"channels\":[\"L\",\"R\"],\"kind\":\"audio\",\"role\":"
        "\"output\"}],\"nodes\":{\"input\":{\"kind\":\"audio_input\"},\"gain_a\":{\"kind\":"
        "\"gain\"},\"gain_b\":{\"config\":{\"gain\":\"f32:3f800000\"},\"kind\":\"gain\"},"
        "\"output\":{\"kind\":\"audio_output\"}},"
        "\"policies\":{},\"profile\":\"pulp.graph.identity.v1\"}",
        binding);
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
