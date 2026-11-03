#include <catch2/catch_test_macros.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <span>
#include <utility>

using pulp::host::GraphResult;
using pulp::host::NodeId;
using pulp::host::PortKind;
using pulp::host::SampleKernelConfig;
using pulp::host::SampleKernelConfigKind;
using pulp::host::SampleRegionDefinition;
using pulp::host::SampleRegionParameterContract;
using pulp::host::SampleRegionParameterOwner;
using pulp::host::SignalGraph;

namespace {

SampleKernelConfig sample_none() {
    return {SampleKernelConfigKind::None, 0, 0.0f};
}

SampleKernelConfig sample_boundary() {
    return {SampleKernelConfigKind::BoundaryIndex, 0, 0.0f};
}

struct SampleRegionFixture {
    SignalGraph graph;
    NodeId input = graph.add_input_node(1);
    NodeId gain = graph.add_gain_node("gain");
    NodeId output = graph.add_output_node(1);
    std::unique_ptr<SampleRegionParameterOwner> parameter_owner;
    std::unique_ptr<SignalGraph::PreparedTopologyEdit> edit;
    NodeId region_input = 0;
    NodeId add = 0;
    NodeId delay = 0;
    NodeId region_output = 0;

    SampleRegionFixture() {
        REQUIRE(graph.connect(input, 0, gain, 0));
        REQUIRE(graph.connect(gain, 0, output, 0));
        REQUIRE(graph.prepare(48000.0, 64));
        parameter_owner = SampleRegionParameterOwner::create(
            {},
            SampleRegionParameterContract::from_regions(std::span<const SampleRegionDefinition>{}));
        REQUIRE(parameter_owner);
        edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->bind_sample_region_parameters(parameter_owner->binding()).accepted);
        REQUIRE(register_builtin_sample_region_types(*edit));
        region_input = edit->add_custom_node("pulp.core.sample-region.input");
        add = edit->add_custom_node("pulp.core.sample-region.add");
        delay = edit->add_custom_node("pulp.core.unit-delay");
        region_output = edit->add_custom_node("pulp.core.sample-region.output");
        REQUIRE(edit->connect(input, 0, region_input, 0));
        REQUIRE(edit->connect(region_input, 0, add, 0));
        REQUIRE(edit->connect(add, 0, delay, 0));
        REQUIRE(edit->connect(add, 0, region_output, 0));
        REQUIRE(edit->connect(region_output, 0, output, 0));

        SampleRegionDefinition definition;
        definition.region_id = 1;
        definition.members = {
            {region_input, "pulp.core.sample-region.input", 1, sample_boundary()},
            {add, "pulp.core.sample-region.add", 1, sample_none()},
            {delay, "pulp.core.unit-delay", 1, sample_none()},
            {region_output, "pulp.core.sample-region.output", 1, sample_boundary()},
        };
        definition.input_boundaries = {region_input};
        definition.output_boundaries = {region_output};
        REQUIRE(edit->declare_sample_region(std::move(definition)).accepted);
        REQUIRE(edit->connect_in_sample_region(1, delay, 0, add, 1).accepted);
        REQUIRE(edit->prepare(48000.0, 64) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
        REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
        edit.reset();
    }
};

} // namespace

TEST_CASE("named graph ports report actionable diagnostics") {
    SignalGraph graph;
    const auto source = graph.add_gain_node("source");
    const auto sink = graph.add_gain_node("sink");
    const auto src = graph.node(source)->out("audio");
    const auto dst = graph.node(sink)->in("audio");
    REQUIRE(graph.connect(src, dst));

    auto unknown = graph.node(source)->out("missing");
    auto result = graph.connect(unknown, dst);
    CHECK(result.code == GraphResult::Code::UnknownPort);
    CHECK(result.message().find("unknown output port 'missing'") != std::string::npos);

    auto mismatch = src;
    mismatch.kind = PortKind::Midi;
    result = graph.connect(mismatch, dst);
    CHECK(result.code == GraphResult::Code::KindMismatch);
    CHECK(result.message().find("port kind mismatch") != std::string::npos);
}

TEST_CASE("named graph ports reject an instantaneous cycle with a message") {
    SignalGraph graph;
    const auto a = graph.add_gain_node("a");
    const auto b = graph.add_gain_node("b");
    REQUIRE(graph.connect(graph.node(a)->out("audio"), graph.node(b)->in("audio")));
    const auto result = graph.connect(graph.node(b)->out("audio"), graph.node(a)->in("audio"));
    CHECK(result.code == GraphResult::Code::IllegalCycle);
    CHECK(result.message().find("illegal instantaneous cycle") != std::string::npos);
}

TEST_CASE("MIDI graph ports retain their kind") {
    SignalGraph graph;
    const auto midi_source = graph.add_midi_input_node("midi source");
    const auto midi_sink = graph.add_midi_output_node("midi sink");
    const auto audio = graph.add_gain_node("audio");

    CHECK(graph.node(midi_source)->out("audio").kind == PortKind::Midi);
    CHECK(graph.node(midi_sink)->in("audio").kind == PortKind::Midi);
    CHECK(graph.connect(graph.node(midi_source)->out("audio"), graph.node(midi_sink)->in("audio")));
    const auto mismatch =
        graph.connect(graph.node(midi_source)->out("audio"), graph.node(audio)->in("audio"));
    CHECK(mismatch.code == GraphResult::Code::KindMismatch);
}

TEST_CASE("named graph ports reject handles with the wrong direction") {
    SignalGraph graph;
    const auto source = graph.add_gain_node("source");
    const auto dest = graph.add_gain_node("dest");

    const auto source_is_input =
        graph.connect(graph.node(source)->in("audio"), graph.node(dest)->in("audio"));
    CHECK_FALSE(source_is_input);
    CHECK(source_is_input.message().find("source handle is not an output port") !=
          std::string::npos);

    const auto destination_is_output =
        graph.connect(graph.node(source)->out("audio"), graph.node(dest)->out("audio"));
    CHECK_FALSE(destination_is_output);
    CHECK(destination_is_output.message().find("destination handle is not an input port") !=
          std::string::npos);
}

TEST_CASE("named graph ports reject direct mutation of sample-region nodes") {
    SampleRegionFixture fixture;
    const auto result =
        fixture.graph.connect(fixture.graph.node(fixture.region_input)->out("audio"),
                              fixture.graph.node(fixture.gain)->in("audio"));
    CHECK_FALSE(result);
    CHECK(result.message().find("sample-region nodes cannot be mutated directly") !=
          std::string::npos);
}
