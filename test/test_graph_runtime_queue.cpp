#include <catch2/catch_test_macros.hpp>
#include <pulp/graph/graph_runtime_queue.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace {

pulp::graph::GraphCommand command(std::uint64_t sequence_id,
                                 pulp::graph::GraphCommandTiming timing,
                                 pulp::graph::NodeId node_id = 1) {
    pulp::graph::GraphCommand c;
    c.sequence_id = sequence_id;
    c.type = pulp::graph::GraphCommandType::SetNodeGain;
    c.node_id = node_id;
    c.timing = timing;
    c.value = static_cast<float>(sequence_id);
    return c;
}

pulp::graph::GraphCommandTiming immediate() {
    return {};
}

pulp::graph::GraphCommandTiming at(std::uint32_t frame_offset) {
    return {pulp::graph::GraphCommandTimingType::BlockOffset, frame_offset};
}

} // namespace

TEST_CASE("GraphRuntimeQueues drain graph commands in block-time order",
          "[graph][graph-runtime][queue]") {
    pulp::graph::GraphRuntimeQueues<4, 4, 4> queues;

    REQUIRE(queues.enqueue_command(command(1, at(8))));
    REQUIRE(queues.enqueue_command(command(2, immediate())));
    REQUIRE(queues.enqueue_command(command(3, at(3))));
    REQUIRE(queues.enqueue_command(command(4, at(3))));
    REQUIRE_FALSE(queues.enqueue_command(command(5, at(1))));

    std::array<pulp::graph::GraphTimedCommand, 4> drained{};
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
          "[graph][graph-runtime][queue]") {
    pulp::graph::GraphRuntimeQueues<2, 2, 2> queues;
    REQUIRE(queues.enqueue_command(command(1, at(999))));
    REQUIRE(queues.enqueue_command(command(2, at(0))));

    std::array<pulp::graph::GraphTimedCommand, 2> drained{};
    const auto count = queues.drain_commands_for_block(drained, 8);

    REQUIRE(count == 2);
    REQUIRE(drained[0].command.sequence_id == 2);
    REQUIRE(drained[0].block_offset == 0);
    REQUIRE(drained[1].command.sequence_id == 1);
    REQUIRE(drained[1].block_offset == 7);
}

TEST_CASE("GraphRuntimeQueues account for realtime staging overflow",
          "[graph][graph-runtime][queue]") {
    pulp::graph::GraphRuntimeQueues<3, 2, 2> queues;
    REQUIRE(queues.enqueue_command(command(1, at(7))));
    REQUIRE(queues.enqueue_command(command(2, at(6))));
    REQUIRE(queues.enqueue_command(command(3, at(0))));

    std::array<pulp::graph::GraphTimedCommand, 1> drained{};
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
          "[graph][graph-runtime][queue]") {
    pulp::graph::GraphRuntimeQueues<2, 3, 2> queues;

    for (std::uint64_t i = 1; i <= 3; ++i) {
        pulp::graph::GraphEvent event;
        event.sequence_id = i;
        event.type = pulp::graph::GraphEventType::CommandAccepted;
        event.node_id = static_cast<pulp::graph::NodeId>(10 + i);
        event.block_offset = static_cast<std::uint32_t>(i);
        REQUIRE(queues.push_event_from_realtime(event));
    }

    pulp::graph::GraphEvent dropped;
    dropped.sequence_id = 4;
    dropped.type = pulp::graph::GraphEventType::CommandRejected;
    REQUIRE_FALSE(queues.push_event_from_realtime(dropped));

    pulp::graph::GraphEvent popped;
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
          "[graph][graph-runtime][queue][midi]") {
    pulp::graph::GraphRuntimeQueues<2, 2, 2> queues;

    pulp::graph::GraphMidiOutputEvent first;
    first.sequence_id = 1;
    first.node_id = 42;
    first.port = 1;
    first.block_offset = 3;
    first.event = pulp::midi::MidiEvent::note_on(0, 60, 100);

    pulp::graph::GraphMidiOutputEvent second = first;
    second.sequence_id = 2;
    second.block_offset = 5;
    second.event = pulp::midi::MidiEvent::note_off(0, 60);

    pulp::graph::GraphMidiOutputEvent third = first;
    third.sequence_id = 3;

    REQUIRE(queues.push_midi_output_from_realtime(first));
    REQUIRE(queues.push_midi_output_from_realtime(second));
    REQUIRE_FALSE(queues.push_midi_output_from_realtime(third));

    pulp::graph::GraphMidiOutputEvent popped;
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

// Keep `Concurrent` and `Race` in this title: sanitizers.yml selects
// TSan-focused tests by CTest name, not by lowercase Catch2 tags.
TEST_CASE("GraphRuntimeQueues Concurrent control/realtime handoff is Race clean",
          "[graph][graph-runtime][queue][concurrent][race]") {
    using namespace std::chrono_literals;

    constexpr std::uint32_t kBlockFrames = 64;
    constexpr std::uint64_t kCommands = 256;
    constexpr std::uint64_t kWindow = 8;
    constexpr auto kTimeout = 2s;

    pulp::graph::GraphRuntimeQueues<32, 32, 32> queues;
    std::atomic<bool> start{false};
    std::atomic<bool> command_phase_done{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<std::uint64_t> realtime_processed{0};
    std::atomic<std::uint64_t> realtime_sequence_sum{0};
    std::atomic<std::uint64_t> realtime_push_failures{0};

    struct RealtimeJoinGuard {
        std::thread& thread;
        std::atomic<bool>& command_phase_done;
        std::atomic<bool>& stop_requested;

        ~RealtimeJoinGuard() {
            command_phase_done.store(true, std::memory_order_release);
            stop_requested.store(true, std::memory_order_release);
            if (thread.joinable()) {
                thread.join();
            }
        }
    };

    std::thread realtime([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        std::array<pulp::graph::GraphTimedCommand, kWindow> drained{};
        while (!stop_requested.load(std::memory_order_acquire) &&
               (!command_phase_done.load(std::memory_order_acquire) ||
                realtime_processed.load(std::memory_order_acquire) < kCommands)) {
            const auto count = queues.drain_commands_for_block(drained, kBlockFrames);
            if (count == 0) {
                std::this_thread::yield();
                continue;
            }

            for (std::uint32_t i = 0; i < count; ++i) {
                const auto& timed = drained[i];
                pulp::graph::GraphEvent event;
                event.sequence_id = timed.command.sequence_id;
                event.type = pulp::graph::GraphEventType::CommandAccepted;
                event.node_id = timed.command.node_id;
                event.block_offset = timed.block_offset;
                event.value = timed.command.value;
                if (!queues.push_event_from_realtime(event)) {
                    realtime_push_failures.fetch_add(1, std::memory_order_relaxed);
                }

                pulp::graph::GraphMidiOutputEvent midi_event;
                midi_event.sequence_id = timed.command.sequence_id;
                midi_event.node_id = timed.command.node_id;
                midi_event.block_offset = timed.block_offset;
                midi_event.event = pulp::midi::MidiEvent::note_on(
                    0,
                    static_cast<std::uint8_t>(60 + (timed.command.sequence_id % 12)),
                    100);
                if (!queues.push_midi_output_from_realtime(midi_event)) {
                    realtime_push_failures.fetch_add(1, std::memory_order_relaxed);
                }

                realtime_sequence_sum.fetch_add(timed.command.sequence_id,
                                                std::memory_order_relaxed);
                realtime_processed.fetch_add(1, std::memory_order_release);
            }
        }
    });
    RealtimeJoinGuard realtime_join_guard{
        realtime,
        command_phase_done,
        stop_requested,
    };

    start.store(true, std::memory_order_release);

    std::array<bool, kCommands + 1> event_sequences_seen{};
    std::array<bool, kCommands + 1> midi_sequences_seen{};
    std::uint64_t expected_sequence_sum = 0;
    std::uint64_t events_seen = 0;
    std::uint64_t event_sequence_sum = 0;
    std::uint64_t midi_seen = 0;
    std::uint64_t midi_sequence_sum = 0;
    bool timed_out = false;

    for (std::uint64_t first = 1; first <= kCommands; first += kWindow) {
        const auto last = std::min<std::uint64_t>(kCommands, first + kWindow - 1);
        for (std::uint64_t sequence = first; sequence <= last; ++sequence) {
            REQUIRE(queues.enqueue_command(command(
                sequence,
                at(static_cast<std::uint32_t>(sequence - first)),
                static_cast<pulp::graph::NodeId>(sequence % 8))));
            expected_sequence_sum += sequence;
        }

        const auto target_count = last;
        const auto deadline = std::chrono::steady_clock::now() + kTimeout;
        while ((events_seen < target_count || midi_seen < target_count) &&
               std::chrono::steady_clock::now() < deadline) {
            pulp::graph::GraphEvent event;
            if (queues.pop_event(event)) {
                REQUIRE(event.type == pulp::graph::GraphEventType::CommandAccepted);
                REQUIRE(event.sequence_id == events_seen + 1);
                REQUIRE(event.sequence_id >= first);
                REQUIRE(event.sequence_id <= last);
                REQUIRE_FALSE(event_sequences_seen[event.sequence_id]);
                event_sequences_seen[event.sequence_id] = true;
                ++events_seen;
                event_sequence_sum += event.sequence_id;
            }

            pulp::graph::GraphMidiOutputEvent midi_event;
            if (queues.pop_midi_output(midi_event)) {
                REQUIRE(midi_event.event.is_note_on());
                REQUIRE(midi_event.sequence_id == midi_seen + 1);
                REQUIRE(midi_event.sequence_id >= first);
                REQUIRE(midi_event.sequence_id <= last);
                REQUIRE_FALSE(midi_sequences_seen[midi_event.sequence_id]);
                midi_sequences_seen[midi_event.sequence_id] = true;
                ++midi_seen;
                midi_sequence_sum += midi_event.sequence_id;
            }

            if (events_seen < target_count || midi_seen < target_count) {
                std::this_thread::yield();
            }
        }

        if (events_seen < target_count || midi_seen < target_count) {
            timed_out = true;
            break;
        }
    }

    command_phase_done.store(true, std::memory_order_release);
    if (timed_out) {
        stop_requested.store(true, std::memory_order_release);
    }
    realtime.join();

    INFO("events_seen=" << events_seen << " midi_seen=" << midi_seen
         << " realtime_processed=" << realtime_processed.load()
         << " push_failures=" << realtime_push_failures.load());
    REQUIRE_FALSE(timed_out);
    REQUIRE(realtime_push_failures.load() == 0);
    REQUIRE(realtime_processed.load() == kCommands);
    REQUIRE(realtime_sequence_sum.load() == expected_sequence_sum);
    REQUIRE(events_seen == kCommands);
    REQUIRE(event_sequence_sum == expected_sequence_sum);
    REQUIRE(midi_seen == kCommands);
    REQUIRE(midi_sequence_sum == expected_sequence_sum);
    for (std::uint64_t sequence = 1; sequence <= kCommands; ++sequence) {
        REQUIRE(event_sequences_seen[sequence]);
        REQUIRE(midi_sequences_seen[sequence]);
    }

    const auto stats = queues.stats();
    REQUIRE(stats.commands_enqueued == kCommands);
    REQUIRE(stats.commands_drained == kCommands);
    REQUIRE(stats.events_enqueued == kCommands);
    REQUIRE(stats.midi_outputs_enqueued == kCommands);
    REQUIRE(stats.dropped_commands == 0);
    REQUIRE(stats.dropped_events == 0);
    REQUIRE(stats.dropped_midi_outputs == 0);
}

TEST_CASE("GraphRuntimeQueues reset offline queues and counters",
          "[graph][graph-runtime][queue]") {
    pulp::graph::GraphRuntimeQueues<2, 2, 2> queues;
    REQUIRE(queues.enqueue_command(command(1, immediate())));

    pulp::graph::GraphEvent event;
    event.sequence_id = 1;
    REQUIRE(queues.push_event_from_realtime(event));

    pulp::graph::GraphMidiOutputEvent midi_event;
    midi_event.sequence_id = 1;
    midi_event.event = pulp::midi::MidiEvent::note_on(0, 64, 80);
    REQUIRE(queues.push_midi_output_from_realtime(midi_event));

    queues.reset_offline();

    std::array<pulp::graph::GraphTimedCommand, 2> drained{};
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
