#include <catch2/catch_test_macros.hpp>
#include <pulp/host/graph_runtime_queue.hpp>

#include <array>
#include <cstdint>

namespace {

pulp::host::GraphCommand command(std::uint64_t sequence_id,
                                 pulp::host::GraphCommandTiming timing,
                                 pulp::host::NodeId node_id = 1) {
    pulp::host::GraphCommand c;
    c.sequence_id = sequence_id;
    c.type = pulp::host::GraphCommandType::SetNodeGain;
    c.node_id = node_id;
    c.timing = timing;
    c.value = static_cast<float>(sequence_id);
    return c;
}

pulp::host::GraphCommandTiming immediate() {
    return {};
}

pulp::host::GraphCommandTiming at(std::uint32_t frame_offset) {
    return {pulp::host::GraphCommandTimingType::BlockOffset, frame_offset};
}

} // namespace

TEST_CASE("GraphRuntimeQueues drain graph commands in block-time order",
          "[host][graph-runtime][queue]") {
    pulp::host::GraphRuntimeQueues<4, 4, 4> queues;

    REQUIRE(queues.enqueue_command(command(1, at(8))));
    REQUIRE(queues.enqueue_command(command(2, immediate())));
    REQUIRE(queues.enqueue_command(command(3, at(3))));
    REQUIRE(queues.enqueue_command(command(4, at(3))));
    REQUIRE_FALSE(queues.enqueue_command(command(5, at(1))));

    std::array<pulp::host::GraphTimedCommand, 4> drained{};
    const auto count = queues.drain_commands_for_block(drained, 16);

    REQUIRE(count == 4);
    REQUIRE(drained[0].command.sequence_id == 2);
    REQUIRE(drained[0].block_offset == 0);
    REQUIRE(drained[1].command.sequence_id == 3);
    REQUIRE(drained[1].block_offset == 3);
    REQUIRE(drained[2].command.sequence_id == 4);
    REQUIRE(drained[2].block_offset == 3);
    REQUIRE(drained[3].command.sequence_id == 1);
    REQUIRE(drained[3].block_offset == 8);

    const auto stats = queues.stats();
    REQUIRE(stats.commands_enqueued == 4);
    REQUIRE(stats.commands_drained == 4);
    REQUIRE(stats.dropped_commands == 1);
}

TEST_CASE("GraphRuntimeQueues clamp command offsets to the current block",
          "[host][graph-runtime][queue]") {
    pulp::host::GraphRuntimeQueues<2, 2, 2> queues;
    REQUIRE(queues.enqueue_command(command(1, at(999))));
    REQUIRE(queues.enqueue_command(command(2, at(0))));

    std::array<pulp::host::GraphTimedCommand, 2> drained{};
    const auto count = queues.drain_commands_for_block(drained, 8);

    REQUIRE(count == 2);
    REQUIRE(drained[0].command.sequence_id == 2);
    REQUIRE(drained[0].block_offset == 0);
    REQUIRE(drained[1].command.sequence_id == 1);
    REQUIRE(drained[1].block_offset == 7);
}

TEST_CASE("GraphRuntimeQueues account for realtime staging overflow",
          "[host][graph-runtime][queue]") {
    pulp::host::GraphRuntimeQueues<3, 2, 2> queues;
    REQUIRE(queues.enqueue_command(command(1, at(7))));
    REQUIRE(queues.enqueue_command(command(2, at(6))));
    REQUIRE(queues.enqueue_command(command(3, at(0))));

    std::array<pulp::host::GraphTimedCommand, 1> drained{};
    const auto count = queues.drain_commands_for_block(drained, 8);

    REQUIRE(count == 1);
    REQUIRE(drained[0].command.sequence_id == 3);
    REQUIRE(drained[0].block_offset == 0);

    const auto stats = queues.stats();
    REQUIRE(stats.commands_enqueued == 3);
    REQUIRE(stats.commands_drained == 1);
    REQUIRE(stats.dropped_commands == 2);
}

TEST_CASE("GraphRuntimeQueues publish realtime graph events to control side",
          "[host][graph-runtime][queue]") {
    pulp::host::GraphRuntimeQueues<2, 3, 2> queues;

    for (std::uint64_t i = 1; i <= 3; ++i) {
        pulp::host::GraphEvent event;
        event.sequence_id = i;
        event.type = pulp::host::GraphEventType::CommandAccepted;
        event.node_id = static_cast<pulp::host::NodeId>(10 + i);
        event.block_offset = static_cast<std::uint32_t>(i);
        REQUIRE(queues.push_event_from_realtime(event));
    }

    pulp::host::GraphEvent dropped;
    dropped.sequence_id = 4;
    dropped.type = pulp::host::GraphEventType::CommandRejected;
    REQUIRE_FALSE(queues.push_event_from_realtime(dropped));

    pulp::host::GraphEvent popped;
    REQUIRE(queues.pop_event(popped));
    REQUIRE(popped.sequence_id == 1);
    REQUIRE(popped.node_id == 11);
    REQUIRE(queues.pop_event(popped));
    REQUIRE(popped.sequence_id == 2);
    REQUIRE(queues.pop_event(popped));
    REQUIRE(popped.sequence_id == 3);
    REQUIRE_FALSE(queues.pop_event(popped));

    const auto stats = queues.stats();
    REQUIRE(stats.events_enqueued == 3);
    REQUIRE(stats.dropped_events == 1);
}

TEST_CASE("GraphRuntimeQueues publish realtime MIDI output separately from graph events",
          "[host][graph-runtime][queue][midi]") {
    pulp::host::GraphRuntimeQueues<2, 2, 2> queues;

    pulp::host::GraphMidiOutputEvent first;
    first.sequence_id = 1;
    first.node_id = 42;
    first.port = 1;
    first.block_offset = 3;
    first.event = pulp::midi::MidiEvent::note_on(0, 60, 100);

    pulp::host::GraphMidiOutputEvent second = first;
    second.sequence_id = 2;
    second.block_offset = 5;
    second.event = pulp::midi::MidiEvent::note_off(0, 60);

    pulp::host::GraphMidiOutputEvent third = first;
    third.sequence_id = 3;

    REQUIRE(queues.push_midi_output_from_realtime(first));
    REQUIRE(queues.push_midi_output_from_realtime(second));
    REQUIRE_FALSE(queues.push_midi_output_from_realtime(third));

    pulp::host::GraphMidiOutputEvent popped;
    REQUIRE(queues.pop_midi_output(popped));
    REQUIRE(popped.sequence_id == 1);
    REQUIRE(popped.node_id == 42);
    REQUIRE(popped.port == 1);
    REQUIRE(popped.block_offset == 3);
    REQUIRE(popped.event.is_note_on());
    REQUIRE(queues.pop_midi_output(popped));
    REQUIRE(popped.sequence_id == 2);
    REQUIRE(popped.block_offset == 5);
    REQUIRE(popped.event.is_note_off());
    REQUIRE_FALSE(queues.pop_midi_output(popped));

    const auto stats = queues.stats();
    REQUIRE(stats.midi_outputs_enqueued == 2);
    REQUIRE(stats.dropped_midi_outputs == 1);
}

TEST_CASE("GraphRuntimeQueues reset offline queues and counters",
          "[host][graph-runtime][queue]") {
    pulp::host::GraphRuntimeQueues<2, 2, 2> queues;
    REQUIRE(queues.enqueue_command(command(1, immediate())));

    pulp::host::GraphEvent event;
    event.sequence_id = 1;
    REQUIRE(queues.push_event_from_realtime(event));

    pulp::host::GraphMidiOutputEvent midi_event;
    midi_event.sequence_id = 1;
    midi_event.event = pulp::midi::MidiEvent::note_on(0, 64, 80);
    REQUIRE(queues.push_midi_output_from_realtime(midi_event));

    queues.reset_offline();

    std::array<pulp::host::GraphTimedCommand, 2> drained{};
    REQUIRE(queues.drain_commands_for_block(drained, 8) == 0);
    REQUIRE_FALSE(queues.pop_event(event));
    REQUIRE_FALSE(queues.pop_midi_output(midi_event));

    const auto stats = queues.stats();
    REQUIRE(stats.commands_enqueued == 0);
    REQUIRE(stats.commands_drained == 0);
    REQUIRE(stats.events_enqueued == 0);
    REQUIRE(stats.midi_outputs_enqueued == 0);
    REQUIRE(stats.dropped_commands == 0);
    REQUIRE(stats.dropped_events == 0);
    REQUIRE(stats.dropped_midi_outputs == 0);
}
