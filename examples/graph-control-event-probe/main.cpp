#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/midi/message.hpp>

#include <array>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>

using namespace pulp;

namespace {
void require(bool value, const char* rule) {
    if (!value)
        throw std::runtime_error(rule);
}

bool process_node(format::ProcessBlock& block,
                  const format::GraphRuntimeNodeProcessContext& context, void*) noexcept {
    if (context.node_param_events == nullptr || context.node_param_events->size() != 2)
        return false;
    (void)context.node_inputs;
    (void)context.node_outputs;
    return true;
}

void control_probe(bool planted) {
    using namespace graph;
    using namespace format;
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    GraphRuntimeConnectionSpec automation{1, 0, 2, 0};
    automation.kind = GraphRuntimeConnectionKind::Automation;
    automation.automation.param_id = 7;
    automation.automation.range_lo = 0.0f;
    automation.automation.range_hi = 1.0f;
    automation.automation.bounds_lo = 0.0f;
    automation.automation.bounds_hi = 1.0f;
    const std::array connections = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        automation,
        GraphRuntimeConnectionSpec{2, 0, 3, 0},
    };
    const std::array bindings = {
        GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{2, process_node, nullptr, true},
        GraphRuntimeNodeBinding{3, nullptr, nullptr, false},
    };
    auto built = build_graph_runtime_plan(nodes, connections);
    require(built.ok(), "control-plan");
    GraphRuntimeSnapshot snapshot;
    require(snapshot.reset(std::move(built.plan), bindings), "control-snapshot");
    GraphRuntimeBufferPool pool;
    require(pool.reset(snapshot.buffer_slot_count(), 8), "control-pool");
    GraphRuntimeAutomationScratch automation_scratch;
    require(automation_scratch.reset(snapshot.plan(), snapshot.bindings(), 8),
            "control-automation");
    std::array<float, 8> input{};
    input.fill(0.75f);
    const std::array<const float*, 1> in_channels{input.data()};
    std::array<float, 8> output{};
    const std::array<float*, 1> out_channels{output.data()};
    BusBufferSet buses;
    require(buses.add_input("main", audio::BufferView<const float>(in_channels.data(), 1, 8)) &&
                buses.add_output("main", audio::BufferView<float>(out_channels.data(), 1, 8)),
            "control-buses");
    ProcessBlock block;
    block.sample_rate = 48000.0;
    block.frame_count = 8;
    block.buses = &buses;
    GraphRuntimeControlEdgeProbe control;
    control.source_index = 0;
    control.destination_index = 1;
    control.parameter_id = 7;
    control.edge_index = 1;
    control.samples.attach();
    GraphRuntimeEdgeProbeSet probes;
    probes.control = &control;
    GraphRuntimeExecutor executor;
    if (!planted)
        require(executor
                    .process_routed(block, snapshot, pool, nullptr, &automation_scratch, {}, {}, {},
                                    {}, {}, nullptr, &probes)
                    .ok(),
                "control-process");
    GraphRuntimeProbeSample sample;
    require(control.samples.try_pop(sample), "control-publication");
    require(sample.kind == GraphRuntimeProbeKind::Control && sample.value == 0.75f &&
                sample.value_end == 0.75f && sample.payload_id == 7 && sample.frames == 8,
            "control-oracle");
}

void event_probe(bool planted) {
    using namespace graph;
    using namespace format;
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::MidiInput, 0, 0, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::MidiOutput, 0, 0, 1, 0},
    };
    GraphRuntimeConnectionSpec edge{1, 0, 2, 0};
    edge.kind = GraphRuntimeConnectionKind::Event;
    auto built = build_graph_runtime_plan(nodes, std::span(&edge, 1));
    require(built.ok(), "event-plan");
    const std::array bindings = {GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
                                 GraphRuntimeNodeBinding{2, nullptr, nullptr, false}};
    GraphRuntimeSnapshot snapshot;
    require(snapshot.reset(std::move(built.plan), bindings), "event-snapshot");
    GraphRuntimeBufferPool pool;
    require(pool.reset(snapshot.buffer_slot_count(), 8), "event-pool");
    GraphRuntimeMidiScratch midi;
    require(midi.reset(snapshot.node_count(), 1), "event-midi");
    require(midi.out(0)->add(pulp::midi::MidiEvent::note_on(0, 60, 100)), "event-seed");
    require(!midi.out(0)->add(pulp::midi::MidiEvent::note_on(0, 61, 100)), "event-source-drop");
    GraphRuntimeEventEdgeProbe event;
    event.source_index = 0;
    event.destination_index = 1;
    event.edge_index = 0;
    event.samples.attach();
    GraphRuntimeEdgeProbeSet probes;
    probes.event = &event;
    ProcessBlock block;
    block.sample_rate = 48000.0;
    block.frame_count = 8;
    GraphRuntimeExecutor executor;
    if (!planted)
        require(executor
                    .process_routed(block, snapshot, pool, &midi, nullptr, {}, {}, {}, {}, {},
                                    nullptr, &probes)
                    .ok(),
                "event-process");
    GraphRuntimeProbeSample sample;
    require(event.samples.try_pop(sample), "event-publication");
    require(sample.kind == GraphRuntimeProbeKind::Event && sample.payload_count == 1 &&
                sample.payload_dropped == 1 && sample.payload_status != 0 &&
                sample.payload_offset == 0,
            "event-oracle");
}
} // namespace

int main(int argc, char** argv) try {
    const auto argument = argc == 2 ? std::string_view(argv[1]) : std::string_view{};
    control_probe(argument == "--planted-missing-publication");
    event_probe(argument == "--planted-missing-event-publication");
    std::cout << "graph-control-event-probe PASS\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "graph-control-event-probe FAIL rule=" << error.what() << '\n';
    return 1;
}
