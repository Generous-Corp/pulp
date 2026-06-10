#pragma once

#include <pulp/graph/graph_types.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/runtime/spsc_queue.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace pulp::graph {

enum class GraphCommandType : std::uint8_t {
    SetNodeGain,
    SetNodeParameter,
    SetNodeBypass,
    ResetNode,
    InjectMidi,
    TransportJump,
    ActivateSnapshot,
    DeactivateSnapshot,
};

enum class GraphCommandTimingType : std::uint8_t {
    Immediate,
    BlockOffset,
};

struct GraphCommandTiming {
    GraphCommandTimingType type = GraphCommandTimingType::Immediate;
    std::uint32_t frame_offset = 0;
};

struct GraphCommand {
    std::uint64_t sequence_id = 0;
    GraphCommandType type = GraphCommandType::SetNodeGain;
    NodeId node_id = 0;
    PortIndex port = 0;
    std::uint32_t target_id = 0;
    GraphCommandTiming timing;
    float value = 0.0f;
    bool bool_value = false;
    midi::MidiEvent midi;
};

struct GraphTimedCommand {
    GraphCommand command;
    std::uint32_t block_offset = 0;
    std::uint32_t order = 0;
};

enum class GraphEventType : std::uint8_t {
    CommandAccepted,
    CommandRejected,
    CommandDropped,
    SnapshotActivated,
    SnapshotDeactivated,
    NodeReset,
    LatencyChanged,
    TailChanged,
    Diagnostic,
};

struct GraphEvent {
    std::uint64_t sequence_id = 0;
    GraphEventType type = GraphEventType::CommandAccepted;
    NodeId node_id = 0;
    std::uint32_t target_id = 0;
    std::uint32_t block_offset = 0;
    float value = 0.0f;
    bool bool_value = false;
};

struct GraphMidiOutputEvent {
    std::uint64_t sequence_id = 0;
    NodeId node_id = 0;
    PortIndex port = 0;
    std::uint32_t block_offset = 0;
    midi::MidiEvent event;
};

struct GraphRuntimeQueueStats {
    std::uint64_t commands_enqueued = 0;
    std::uint64_t commands_drained = 0;
    std::uint64_t events_enqueued = 0;
    std::uint64_t midi_outputs_enqueued = 0;
    std::uint64_t dropped_commands = 0;
    std::uint64_t dropped_events = 0;
    std::uint64_t dropped_midi_outputs = 0;
};

/// Fixed-capacity graph command/event queues for realtime graph runtimes.
///
/// Direction is explicit:
/// - enqueue_command() is called from the control thread and drained by the
///   realtime graph at the start of a block.
/// - push_event_from_realtime() and push_midi_output_from_realtime() are called
///   by the realtime graph and drained by the control thread.
///
/// The class owns no heap storage after construction beyond the fixed FIFO
/// storage owned by runtime::SpscQueue. It does not apply commands to a graph;
/// graph runtimes consume GraphTimedCommand values and decide the mutation
/// policy for the active snapshot.
template<std::size_t CommandCapacity = 128,
         std::size_t EventCapacity = 256,
         std::size_t MidiOutputCapacity = 256>
class GraphRuntimeQueues {
    static_assert(CommandCapacity > 0, "CommandCapacity must be > 0");
    static_assert(EventCapacity > 0, "EventCapacity must be > 0");
    static_assert(MidiOutputCapacity > 0, "MidiOutputCapacity must be > 0");
    static_assert(std::is_nothrow_copy_constructible_v<GraphCommand> &&
                  std::is_nothrow_copy_assignable_v<GraphCommand>,
                  "GraphCommand must stay RT-copyable");
    static_assert(std::is_nothrow_copy_constructible_v<GraphEvent> &&
                  std::is_nothrow_copy_assignable_v<GraphEvent>,
                  "GraphEvent must stay RT-copyable");
    static_assert(std::is_nothrow_copy_constructible_v<GraphMidiOutputEvent> &&
                  std::is_nothrow_copy_assignable_v<GraphMidiOutputEvent>,
                  "GraphMidiOutputEvent must stay RT-copyable");

public:
    bool enqueue_command(const GraphCommand& command) noexcept {
        if (!commands_.try_push(command)) {
            dropped_commands_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        commands_enqueued_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    std::uint32_t drain_commands_for_block(std::span<GraphTimedCommand> out,
                                           std::uint32_t block_frames) noexcept {
        std::array<GraphTimedCommand, CommandCapacity> staged{};
        std::uint32_t staged_count = 0;
        std::uint32_t order = 0;
        while (auto popped = commands_.try_pop()) {
            if (staged_count >= staged.size()) {
                dropped_commands_.fetch_add(1, std::memory_order_relaxed);
                ++order;
                continue;
            }
            auto command = *popped;
            staged[staged_count++] = {
                command,
                command_offset(command.timing, block_frames),
                order++,
            };
        }

        std::span<GraphTimedCommand> sorted(staged.data(), staged_count);
        sort_timed_commands(sorted);

        const auto count = static_cast<std::uint32_t>(
            std::min<std::size_t>(out.size(), staged_count));
        for (std::uint32_t i = 0; i < count; ++i) {
            out[i] = sorted[i];
        }
        if (staged_count > count) {
            dropped_commands_.fetch_add(staged_count - count, std::memory_order_relaxed);
        }
        commands_drained_.fetch_add(count, std::memory_order_relaxed);
        return count;
    }

    bool push_event_from_realtime(const GraphEvent& event) noexcept {
        if (!events_.try_push(event)) {
            dropped_events_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        events_enqueued_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool pop_event(GraphEvent& event) noexcept {
        auto popped = events_.try_pop();
        if (!popped.has_value()) return false;
        event = *popped;
        return true;
    }

    bool push_midi_output_from_realtime(const GraphMidiOutputEvent& event) noexcept {
        if (!midi_outputs_.try_push(event)) {
            dropped_midi_outputs_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        midi_outputs_enqueued_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool pop_midi_output(GraphMidiOutputEvent& event) noexcept {
        auto popped = midi_outputs_.try_pop();
        if (!popped.has_value()) return false;
        event = *popped;
        return true;
    }

    /// Offline-only reset. Call only when both the control producer and the
    /// realtime consumer/producer are stopped; this method drains both sides of
    /// every SPSC queue and resets diagnostic counters.
    void reset_offline() noexcept {
        while (commands_.try_pop().has_value()) {}
        while (events_.try_pop().has_value()) {}
        while (midi_outputs_.try_pop().has_value()) {}
        commands_enqueued_.store(0, std::memory_order_relaxed);
        commands_drained_.store(0, std::memory_order_relaxed);
        events_enqueued_.store(0, std::memory_order_relaxed);
        midi_outputs_enqueued_.store(0, std::memory_order_relaxed);
        dropped_commands_.store(0, std::memory_order_relaxed);
        dropped_events_.store(0, std::memory_order_relaxed);
        dropped_midi_outputs_.store(0, std::memory_order_relaxed);
    }

    GraphRuntimeQueueStats stats() const noexcept {
        return {
            commands_enqueued_.load(std::memory_order_relaxed),
            commands_drained_.load(std::memory_order_relaxed),
            events_enqueued_.load(std::memory_order_relaxed),
            midi_outputs_enqueued_.load(std::memory_order_relaxed),
            dropped_commands_.load(std::memory_order_relaxed),
            dropped_events_.load(std::memory_order_relaxed),
            dropped_midi_outputs_.load(std::memory_order_relaxed),
        };
    }

    static constexpr std::size_t command_capacity() noexcept { return CommandCapacity; }
    static constexpr std::size_t event_capacity() noexcept { return EventCapacity; }
    static constexpr std::size_t midi_output_capacity() noexcept { return MidiOutputCapacity; }

private:
    static std::uint32_t command_offset(GraphCommandTiming timing,
                                        std::uint32_t block_frames) noexcept {
        if (timing.type == GraphCommandTimingType::Immediate || block_frames == 0) {
            return 0;
        }
        return std::min(timing.frame_offset, block_frames - 1);
    }

    static void sort_timed_commands(std::span<GraphTimedCommand> commands) noexcept {
        for (std::size_t i = 1; i < commands.size(); ++i) {
            auto current = commands[i];
            auto j = i;
            while (j > 0 && comes_before(current, commands[j - 1])) {
                commands[j] = commands[j - 1];
                --j;
            }
            commands[j] = current;
        }
    }

    static bool comes_before(const GraphTimedCommand& a,
                             const GraphTimedCommand& b) noexcept {
        if (a.block_offset != b.block_offset) return a.block_offset < b.block_offset;
        return a.order < b.order;
    }

    runtime::SpscQueue<GraphCommand, CommandCapacity> commands_;
    runtime::SpscQueue<GraphEvent, EventCapacity> events_;
    runtime::SpscQueue<GraphMidiOutputEvent, MidiOutputCapacity> midi_outputs_;

    std::atomic<std::uint64_t> commands_enqueued_{0};
    std::atomic<std::uint64_t> commands_drained_{0};
    std::atomic<std::uint64_t> events_enqueued_{0};
    std::atomic<std::uint64_t> midi_outputs_enqueued_{0};
    std::atomic<std::uint64_t> dropped_commands_{0};
    std::atomic<std::uint64_t> dropped_events_{0};
    std::atomic<std::uint64_t> dropped_midi_outputs_{0};
};

} // namespace pulp::graph
