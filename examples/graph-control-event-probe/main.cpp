#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/midi/message.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

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

void dense_probe() {
    using namespace graph;
    using namespace format;
    for (const auto delivery : {AudioRateModulationDelivery::LegacyParameterEvents,
                                AudioRateModulationDelivery::DenseViews}) {
        const std::array nodes = {
            GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
            GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Gain, 1, 1, 0, 0, false, 3},
            GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::Processor, 1, 1},
            GraphRuntimeNodeSpec{4, GraphRuntimeNodeKind::AudioOutput, 1, 0},
        };
        GraphRuntimeConnectionSpec edge{1, 0, 3, 0};
        edge.kind = GraphRuntimeConnectionKind::Automation;
        edge.automation = {7, -1.0f, 2.0f, 0.0f, false, true, 0.0f, 1.0f};
        const std::array connections = {
            GraphRuntimeConnectionSpec{1, 0, 2, 0},
            GraphRuntimeConnectionSpec{2, 0, 3, 0},
            edge,
            GraphRuntimeConnectionSpec{3, 0, 4, 0},
        };
        const auto receive = [](ProcessBlock&, const GraphRuntimeNodeProcessContext& context,
                                void*) noexcept { return context.node_param_events != nullptr; };
        std::array bindings = {
            GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
            GraphRuntimeNodeBinding{2, nullptr, nullptr, false},
            GraphRuntimeNodeBinding{3, receive, nullptr, true},
            GraphRuntimeNodeBinding{4, nullptr, nullptr, false},
        };
        bindings[2].audio_rate_modulation_delivery = delivery;
        auto built = build_graph_runtime_plan(nodes, connections);
        require(built.ok(), "dense-plan");
        GraphRuntimeSnapshot snapshot;
        require(snapshot.reset(std::move(built.plan), bindings), "dense-snapshot");
        std::uint32_t selected = 0;
        for (; selected < snapshot.plan().connections.size(); ++selected)
            if (is_automation_conn(snapshot.plan().connections[selected]))
                break;
        const auto& delays = snapshot.buffer_assignment().connection_delay_samples;
        require(delays[selected] == 3, "dense-positive-pdc");
        GraphRuntimeBufferPool pool;
        require(pool.reset(snapshot.buffer_slot_count(), 8, delays), "dense-pool");
        GraphRuntimeAutomationScratch scratch;
        require(scratch.reset(snapshot.plan(), snapshot.bindings(), 8), "dense-scratch");
        GraphRuntimeControlEdgeProbe control;
        control.source_index = 0;
        control.destination_index = 2;
        control.edge_index = selected;
        control.parameter_id = 7;
        control.samples.attach();
        GraphRuntimeEdgeProbeSet probes;
        probes.control = &control;
        GraphRuntimeExecutor executor;
        std::vector<float> history(3, 0.0f);
        std::uint64_t timestamp = 0, sequence = 0;
        for (const int frames : {8, 1, 3, 7, 5}) {
            std::array<float, 8> input{}, output{}, expected{};
            float peak = 0.0f;
            double squares = 0.0;
            for (int f = 0; f < frames; ++f) {
                input[f] = static_cast<float>((timestamp + f) % 7) * 0.25f - 0.25f;
                history.push_back(input[f]);
                expected[f] = -1.0f + 3.0f * std::clamp(history[timestamp + f], 0.0f, 1.0f);
                peak = std::max(peak, std::abs(expected[f]));
                squares += static_cast<double>(expected[f]) * expected[f];
            }
            const float* ips[] = {input.data()};
            float* ops[] = {output.data()};
            BusBufferSet buses;
            require(buses.add_input("main", audio::BufferView<const float>(ips, 1, frames)) &&
                        buses.add_output("main", audio::BufferView<float>(ops, 1, frames)),
                    "dense-buses");
            ProcessBlock block;
            block.sample_rate = 48000;
            block.frame_count = frames;
            block.buses = &buses;
            require(executor
                        .process_routed(block, snapshot, pool, nullptr, &scratch, {}, {}, {}, {},
                                        {}, nullptr, &probes)
                        .ok(),
                    "dense-process");
            GraphRuntimeProbeSample sample;
            require(control.samples.try_pop(sample), "dense-publication");
            require(sample.kind == GraphRuntimeProbeKind::Control &&
                        sample.sequence == sequence++ && sample.timestamp_samples == timestamp &&
                        sample.edge_index == selected && sample.frames == frames &&
                        sample.payload_count == frames && sample.payload_id == 7 &&
                        sample.value == expected[0] && sample.value_end == expected[frames - 1] &&
                        sample.peak == peak &&
                        sample.rms == static_cast<float>(std::sqrt(squares / frames)),
                    "dense-oracle");
            timestamp += frames;
        }
    }
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
    dense_probe();
    std::cout << "graph-control-event-probe PASS\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "graph-control-event-probe FAIL rule=" << error.what() << '\n';
    return 1;
}
