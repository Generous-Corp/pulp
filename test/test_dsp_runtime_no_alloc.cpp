#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_stream_handoff.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/loop_reader.hpp>
#include <pulp/audio/loop_renderer.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/graph/graph_runtime_plan.hpp>
#include <pulp/graph/graph_runtime_queue.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/mpe_buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace {

using pulp::format::BusRole;
using pulp::format::EventBlock;
using pulp::format::GraphRuntimeExecutor;
using pulp::format::GraphRuntimeNodeBinding;
using pulp::format::GraphRuntimeNodeProcessContext;
using pulp::format::GraphRuntimeSnapshot;
using pulp::format::ProcessBlock;
using pulp::graph::GraphCommand;
using pulp::graph::GraphCommandTimingType;
using pulp::graph::GraphRuntimeConnectionSpec;
using pulp::graph::GraphRuntimeNodeKind;
using pulp::graph::GraphRuntimeNodeSpec;
using pulp::graph::GraphRuntimeQueues;
using pulp::graph::NodeId;

struct FixedStereoBlock {
    std::array<float, 64> left{};
    std::array<float, 64> right{};
    std::array<float*, 2> mutable_channels{left.data(), right.data()};
    std::array<const float*, 2> const_channels{left.data(), right.data()};

    pulp::audio::BufferView<float> mutable_view(std::uint32_t frames = 64) noexcept {
        return {mutable_channels.data(), mutable_channels.size(), frames};
    }

    pulp::audio::BufferView<const float> const_view(std::uint32_t frames = 64) const noexcept {
        return {const_channels.data(), const_channels.size(), frames};
    }
};

struct AllocationSnapshot {
    std::size_t count = 0;
    std::size_t bytes = 0;
};

AllocationSnapshot snapshot_allocations(
    const pulp::test::RtAllocationProbe& probe) noexcept {
    return {probe.allocation_count(), probe.allocated_bytes()};
}

void require_no_alloc(AllocationSnapshot snapshot) {
    REQUIRE(snapshot.count == 0);
    REQUIRE(snapshot.bytes == 0);
}

GraphRuntimeNodeSpec node(NodeId id,
                          std::uint32_t inputs,
                          std::uint32_t outputs,
                          GraphRuntimeNodeKind kind = GraphRuntimeNodeKind::Processor) {
    return {id, kind, inputs, outputs};
}

GraphRuntimeConnectionSpec connect(NodeId source,
                                   std::uint32_t source_port,
                                   NodeId dest,
                                   std::uint32_t dest_port) {
    return {source, source_port, dest, dest_port, false, false};
}

GraphCommand command(std::uint64_t sequence_id,
                     NodeId node_id,
                     std::uint32_t offset) {
    GraphCommand c;
    c.sequence_id = sequence_id;
    c.node_id = node_id;
    c.timing = {GraphCommandTimingType::BlockOffset, offset};
    c.value = static_cast<float>(sequence_id);
    return c;
}

struct VisitLog {
    std::uint32_t nodes = 0;
    std::uint32_t command_count = 0;
};

bool record_visit(ProcessBlock& block,
                  const GraphRuntimeNodeProcessContext& context,
                  void* user_data) noexcept {
    auto* log = static_cast<VisitLog*>(user_data);
    if (block.frame_count != 64 || context.node == nullptr || context.plan == nullptr) {
        return false;
    }
    ++log->nodes;
    log->command_count = static_cast<std::uint32_t>(context.command_results.size());
    return true;
}

GraphRuntimeSnapshot make_snapshot(std::span<const GraphRuntimeNodeSpec> nodes,
                                   std::span<const GraphRuntimeConnectionSpec> connections,
                                   std::span<const GraphRuntimeNodeBinding> bindings) {
    auto plan = pulp::graph::build_graph_runtime_plan(nodes, connections);
    REQUIRE(plan.ok());
    GraphRuntimeSnapshot snapshot;
    REQUIRE(snapshot.reset(std::move(plan.plan), bindings));
    return snapshot;
}

ProcessBlock valid_graph_block() {
    ProcessBlock block;
    block.sample_rate = 48000.0;
    block.frame_count = 64;
    block.render_speed = 1.0;
    return block;
}

}  // namespace

TEST_CASE("ProcessBlock event and bus hot views do not allocate",
          "[format][process-block][rt-safety][no-alloc]") {
    FixedStereoBlock input;
    FixedStereoBlock output;
    input.left.fill(0.25f);
    input.right.fill(0.5f);

    pulp::state::ParameterEventQueue parameter_events;
    REQUIRE(parameter_events.push({2, 21, 0.25f, 0}));
    REQUIRE(parameter_events.push({1, 3, 0.75f, 8}));

    pulp::midi::MidiBuffer midi_in;
    auto note = pulp::midi::MidiEvent::note_on(0, 60, 100);
    note.sample_offset = 11;
    auto cc = pulp::midi::MidiEvent::cc(0, 1, 64);
    cc.sample_offset = 4;
    midi_in.add(note);
    midi_in.add(cc);
    midi_in.add_sysex(std::vector<std::uint8_t>{0xF0, 0x7D, 0x01, 0xF7}, 7);

    pulp::midi::MidiBuffer midi_out;
    pulp::midi::MpeBuffer mpe_input;
    pulp::midi::UmpBuffer ump_input;
    pulp::format::BusBufferSet buses;
    EventBlock events;
    events.parameter_events = &parameter_events;
    events.midi_in = &midi_in;
    events.midi_out = &midi_out;
    events.mpe_input = &mpe_input;
    events.ump_input = &ump_input;

    parameter_events.sort();
    midi_in.sort();
    buses.clear();
    REQUIRE(buses.add_input("main", input.const_view(), BusRole::Main));
    REQUIRE(buses.add_output("main", output.mutable_view(), BusRole::Main));

    bool block_valid = false;
    std::size_t parameter_count = 0;
    std::size_t midi_count = 0;
    std::size_t sysex_count = 0;
    bool events_empty = true;
    AllocationSnapshot allocations;
    {
        pulp::test::RtAllocationProbe probe;
        ProcessBlock block;
        block.sample_rate = 48000.0;
        block.frame_count = 64;
        block.render_speed = 1.0;
        block.buses = &buses;
        block.events = &events;
        block_valid = block.validate();
        parameter_count = events.parameter_event_count();
        midi_count = events.midi_input_event_count();
        sysex_count = events.sysex_event_count();
        events_empty = events.empty();
        allocations = snapshot_allocations(probe);
    }

    require_no_alloc(allocations);
    REQUIRE(block_valid);
    REQUIRE(parameter_count == 2);
    REQUIRE(midi_count == 2);
    REQUIRE(sysex_count == 1);
    REQUIRE_FALSE(events_empty);
    REQUIRE(parameter_events.events().front().sample_offset == 3);
    REQUIRE(midi_in[0].sample_offset == 4);
}

TEST_CASE("GraphRuntimeExecutor queue drain hot path does not allocate",
          "[format][graph-runtime][rt-safety][no-alloc]") {
    const std::array nodes = {
        node(10, 0, 1, GraphRuntimeNodeKind::AudioInput),
        node(20, 1, 1),
        node(30, 1, 0, GraphRuntimeNodeKind::AudioOutput),
    };
    const std::array connections = {
        connect(10, 0, 20, 0),
        connect(20, 0, 30, 0),
    };
    VisitLog log;
    const std::array bindings = {
        GraphRuntimeNodeBinding{10, record_visit, &log, true},
        GraphRuntimeNodeBinding{20, record_visit, &log, true},
        GraphRuntimeNodeBinding{30, record_visit, &log, true},
    };
    auto snapshot = make_snapshot(nodes, connections, bindings);

    GraphRuntimeQueues<4, 4, 4> queues;
    REQUIRE(queues.enqueue_command(command(1, 20, 9)));
    REQUIRE(queues.enqueue_command(command(2, 99, 3)));

    GraphRuntimeExecutor executor;
    auto block = valid_graph_block();
    pulp::format::GraphRuntimeExecutorResult result;
    pulp::graph::GraphEvent event;
    bool popped_first = false;
    bool popped_second = false;
    bool popped_third = true;
    AllocationSnapshot allocations;
    {
        pulp::test::RtAllocationProbe probe;
        result = executor.process(block, snapshot, queues);
        popped_first = queues.pop_event(event);
        popped_second = queues.pop_event(event);
        popped_third = queues.pop_event(event);
        allocations = snapshot_allocations(probe);
    }

    require_no_alloc(allocations);
    REQUIRE(result.ok());
    REQUIRE(result.nodes_processed == 3);
    REQUIRE(result.commands_drained == 2);
    REQUIRE(result.commands_accepted == 1);
    REQUIRE(result.commands_rejected == 1);
    REQUIRE(log.nodes == 3);
    REQUIRE(log.command_count == 2);
    REQUIRE(popped_first);
    REQUIRE(popped_second);
    REQUIRE_FALSE(popped_third);
}

TEST_CASE("Sampler handoff and loop render hot paths do not allocate",
          "[audio][sampler][rt-safety][no-alloc]") {
    FixedStereoBlock source;
    FixedStereoBlock handoff_output;
    FixedStereoBlock loop_output;
    for (std::size_t i = 0; i < source.left.size(); ++i) {
        source.left[i] = static_cast<float>(i) / 64.0f;
        source.right[i] = 1.0f - static_cast<float>(i) / 64.0f;
    }

    pulp::audio::AudioStreamHandoff handoff;
    REQUIRE(handoff.prepare({2, 2, 48000.0, 48000.0, 128, 64, 64}));

    pulp::audio::LoopRegion region;
    region.start_frame = 0;
    region.end_frame = 64;
    region.crossfade_frames = 4;
    region.source_sample_rate = 48000.0;
    region.playback_mode = pulp::audio::LoopPlaybackMode::Forward;

    pulp::audio::LoopRenderer loop_renderer;
    REQUIRE(loop_renderer.set_region(region, 64));
    loop_renderer.start();

    std::uint64_t pushed = 0;
    pulp::audio::AudioStreamHandoffPullResult pulled;
    float loop_sample = 0.0f;
    pulp::audio::LoopRenderResult loop_result;
    AllocationSnapshot allocations;
    {
        pulp::test::RtAllocationProbe probe;
        pushed = handoff.push(source.const_view(), 32);
        pulled = handoff.pull(handoff_output.mutable_view(), 32);
        loop_sample = pulp::audio::LoopReader::read_validated(source.const_view(),
                                                             region,
                                                             0,
                                                             12.25);
        loop_result = loop_renderer.render(source.const_view(),
                                           loop_output.mutable_view(),
                                           32);
        allocations = snapshot_allocations(probe);
    }

    require_no_alloc(allocations);
    REQUIRE(pushed == 32);
    REQUIRE(pulled.rendered_frames == 32);
    REQUIRE_FALSE(pulled.underrun);
    REQUIRE(loop_sample > 0.0f);
    REQUIRE(loop_result.rendered_frames == 32);
    REQUIRE(loop_renderer.active());
}
