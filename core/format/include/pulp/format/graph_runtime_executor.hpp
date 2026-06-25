#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/graph/graph_runtime_buffer_assignment.hpp>
#include <pulp/graph/graph_runtime_plan.hpp>
#include <pulp/graph/graph_runtime_queue.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::format {

enum class GraphRuntimeCommandStatus : std::uint8_t {
    Accepted,
    Rejected,
};

enum class GraphRuntimeExecutorErrorCode : std::uint8_t {
    None,
    InvalidProcessBlock,
    InvalidSnapshot,
    CommandScratchTooSmall,
    MissingRequiredProcessor,
    NodeProcessorFailed,
    BufferPoolTooSmall,
    NodePortLimitExceeded,
};

struct GraphRuntimeCommandDecision {
    graph::GraphTimedCommand command;
    GraphRuntimeCommandStatus status = GraphRuntimeCommandStatus::Rejected;
};

struct GraphRuntimeNodeProcessContext {
    const graph::GraphRuntimePlan* plan = nullptr;
    const graph::GraphRuntimeNodePlan* node = nullptr;
    std::uint32_t node_index = 0;
    std::span<const GraphRuntimeCommandDecision> command_results;
    // True when this context came from process_routed(): node_inputs/node_outputs
    // are populated and a binding MUST read/write them rather than the block's
    // buses. False on the shared-block process() path, where they are empty and
    // a binding works the buses directly. A binding should assert the mode it
    // expects — the shared-block path is slated to retire once callers migrate
    // onto routing, at which point this flag and the dual mode collapse.
    bool routed = false;
    // Per-node routed I/O (valid only when `routed`). A routing binding reads
    // `node_inputs` (already gathered from upstream output slots) and writes
    // `node_outputs` (its assigned scratch slots). Channels are mono slots.
    audio::BufferView<const float> node_inputs;
    audio::BufferView<float> node_outputs;
};

using GraphRuntimeNodeProcessFn = bool (*)(
    ProcessBlock& block,
    const GraphRuntimeNodeProcessContext& context,
    void* user_data) noexcept;

using GraphRuntimeCommandApplyFn = GraphRuntimeCommandStatus (*)(
    ProcessBlock& block,
    const graph::GraphRuntimePlan& plan,
    const graph::GraphTimedCommand& command,
    void* user_data) noexcept;

struct GraphRuntimeCommandHandler {
    // Block-level side effects are applied before node processing, so this
    // handler is only invoked for block_offset == 0 commands until the executor
    // grows block-splitting/sample-accurate command application.
    GraphRuntimeCommandApplyFn apply = nullptr;
    void* user_data = nullptr;
};

struct GraphRuntimeNodeBinding {
    graph::NodeId node_id = 0;
    GraphRuntimeNodeProcessFn process = nullptr;
    void* user_data = nullptr;
    bool required = true;
};

/// Control-thread-built graph snapshot for GraphRuntimeExecutor.
///
/// A snapshot is logically immutable after a successful reset(). Do not call
/// reset() or clear() on a snapshot object while any realtime process() call may
/// still reference it; use a publish/lifetime policy above this primitive.
class GraphRuntimeSnapshot {
public:
    bool reset(graph::GraphRuntimePlan plan,
               std::span<const GraphRuntimeNodeBinding> bindings);
    void clear() noexcept;

    bool valid() const noexcept;
    const graph::GraphRuntimePlan& plan() const noexcept { return plan_; }
    std::span<const GraphRuntimeNodeBinding> bindings() const noexcept {
        return bindings_;
    }
    std::uint32_t node_count() const noexcept { return plan_.node_count(); }

    // Scratch-slot layout for the routing process() path, computed off-RT in
    // reset() from the plan. slot_count() is the number of mono slots a backing
    // GraphRuntimeBufferPool must provide.
    const graph::GraphRuntimeBufferAssignment& buffer_assignment() const noexcept {
        return assignment_;
    }
    std::uint32_t buffer_slot_count() const noexcept { return assignment_.slot_count; }

private:
    graph::GraphRuntimePlan plan_;
    std::vector<GraphRuntimeNodeBinding> bindings_;
    graph::GraphRuntimeBufferAssignment assignment_;
};

struct GraphRuntimeEventSink {
    void* user_data = nullptr;
    bool (*push_event)(void* user_data, const graph::GraphEvent& event) noexcept = nullptr;

    bool push(const graph::GraphEvent& event) const noexcept {
        return push_event ? push_event(user_data, event) : true;
    }
};

struct GraphRuntimeExecutorStats {
    std::uint64_t blocks_processed = 0;
    std::uint64_t nodes_processed = 0;
    std::uint64_t commands_drained = 0;
    std::uint64_t commands_accepted = 0;
    std::uint64_t commands_rejected = 0;
    std::uint64_t events_dropped = 0;
    std::uint64_t invalid_blocks = 0;
    std::uint64_t invalid_snapshots = 0;
    std::uint64_t command_scratch_failures = 0;
    std::uint64_t node_failures = 0;
};

struct GraphRuntimeExecutorResult {
    GraphRuntimeExecutorErrorCode error = GraphRuntimeExecutorErrorCode::None;
    std::uint32_t nodes_processed = 0;
    std::uint32_t commands_drained = 0;
    std::uint32_t commands_accepted = 0;
    std::uint32_t commands_rejected = 0;
    std::uint32_t events_dropped = 0;
    std::uint32_t failed_node_index = 0;

    bool ok() const noexcept {
        return error == GraphRuntimeExecutorErrorCode::None;
    }
};

/// Pre-allocated scratch buffers backing the routing process() path.
///
/// One slot is a mono buffer of `max_frames` floats — the same "slot" the
/// snapshot's GraphRuntimeBufferAssignment indexes. The caller sizes the pool
/// off-RT from a snapshot's buffer_slot_count() and the maximum block size, then
/// reuses it across realtime blocks. The executor never resizes it.
///
/// Scope: this is the SHARED inter-node routing scratch for one serial walk.
/// Per-worker scratch for a parallel executor is a separate concern and must
/// not be overloaded onto this shared pool.
class GraphRuntimeBufferPool {
public:
    // Off-RT: (re)allocate storage for `slot_count` mono slots of `max_frames`.
    // Zero-fills, so the first block (and any feedback edge) reads silence.
    // Returns false on allocation failure or zero max_frames with slots.
    bool reset(std::uint32_t slot_count, std::uint32_t max_frames);

    // Off-RT: as above, plus per-connection plug-in-delay-compensation rings.
    // `connection_delay_samples` is the snapshot assignment's per-connection
    // delay (0 = no ring). Each delayed connection gets a contiguous ring of
    // (delay + max_frames) floats and a persisted write position, zero-filled so
    // the leading delay reads silence. The 2-arg reset is equivalent to passing
    // an all-zero / empty span. Returns false on allocation failure.
    bool reset(std::uint32_t slot_count, std::uint32_t max_frames,
               std::span<const std::uint32_t> connection_delay_samples);
    void clear() noexcept;

    std::uint32_t slot_count() const noexcept { return slot_count_; }
    std::uint32_t max_frames() const noexcept { return max_frames_; }

    // RT-safe: mono buffer for slot `index`, or nullptr if out of range.
    float* slot_data(std::uint32_t index) noexcept {
        if (index >= slot_count_) return nullptr;
        return storage_.data() + static_cast<std::size_t>(index) * max_frames_;
    }
    const float* slot_data(std::uint32_t index) const noexcept {
        if (index >= slot_count_) return nullptr;
        return storage_.data() + static_cast<std::size_t>(index) * max_frames_;
    }

    // A per-connection plug-in-delay-compensation ring: a contiguous float
    // buffer of `size` (= delay + max_frames) with a persisted write position the
    // executor advances by the block's frame count. `data == nullptr` when the
    // connection needs no delay.
    struct DelayRing {
        float* data = nullptr;
        std::uint32_t size = 0;
        std::uint32_t delay = 0;
        int* write_pos = nullptr;
    };

    // RT-safe: the delay ring for connection `index`, or an empty ring (data ==
    // nullptr) when the connection has no delay or the pool carries no rings.
    DelayRing delay_ring(std::uint32_t index) noexcept {
        if (index >= ring_.size()) return {};
        RingSlot& r = ring_[index];
        if (r.size == 0) return {};
        return DelayRing{ring_storage_.data() + r.offset, r.size, r.delay, &r.write_pos};
    }

    // True when the pool can back `snapshot`'s routing for a block of `frames`:
    // enough mono slots, a large-enough max_frames, and — if the snapshot needs
    // PDC — a delay-ring layout matching its connection count (so a pool sized
    // without rings falls back to the legacy walk rather than routing without
    // delay compensation and diverging). This validates ring COUNT, not each
    // ring's size; the invariant that the rings were sized from THIS snapshot's
    // per-connection delays is held by construction — reset() the pool from the
    // same buffer_assignment that built the snapshot (both travel as one
    // CompiledGraph). Pairing a pool with a same-connection-count-but-different-
    // delays snapshot is not a supported call sequence.
    bool fits(const GraphRuntimeSnapshot& snapshot, std::uint32_t frames) const noexcept {
        if (slot_count_ < snapshot.buffer_slot_count() || frames > max_frames_) {
            return false;
        }
        const auto& assignment = snapshot.buffer_assignment();
        if (assignment.has_delay &&
            ring_.size() != snapshot.plan().connections.size()) {
            return false;
        }
        return true;
    }

private:
    // Per-connection delay-ring layout (parallel to the plan's connections; empty
    // when the pool was reset without delays). size == 0 means no ring.
    struct RingSlot {
        std::uint32_t offset = 0;  // into ring_storage_
        std::uint32_t size = 0;    // delay + max_frames; 0 = no ring
        std::uint32_t delay = 0;
        int write_pos = 0;         // persisted RT state, advanced per block
    };

    std::vector<float> storage_;
    std::uint32_t slot_count_ = 0;
    std::uint32_t max_frames_ = 0;
    std::vector<float> ring_storage_;
    std::vector<RingSlot> ring_;
};

class GraphRuntimeExecutor {
public:
    static constexpr std::size_t kMaxInlineCommandCapacity = 256;
    // Upper bound on ports per node for the routing path's on-stack channel
    // pointer arrays. Matches GraphRuntimeLimits::max_ports_per_node.
    static constexpr std::uint32_t kMaxRoutedPortsPerNode = 64;

    GraphRuntimeExecutor() = default;

    GraphRuntimeExecutor(const GraphRuntimeExecutor&) = delete;
    GraphRuntimeExecutor& operator=(const GraphRuntimeExecutor&) = delete;

    GraphRuntimeExecutorResult process(
        ProcessBlock& block,
        const GraphRuntimeSnapshot& snapshot,
        std::span<const graph::GraphTimedCommand> commands = {},
        std::span<GraphRuntimeCommandDecision> command_results = {},
        GraphRuntimeCommandHandler command_handler = {},
        GraphRuntimeEventSink event_sink = {}) noexcept;

    template<std::size_t CommandCapacity,
             std::size_t EventCapacity,
             std::size_t MidiOutputCapacity>
    GraphRuntimeExecutorResult process(
        ProcessBlock& block,
        const GraphRuntimeSnapshot& snapshot,
        graph::GraphRuntimeQueues<CommandCapacity, EventCapacity, MidiOutputCapacity>& queues,
        GraphRuntimeCommandHandler command_handler = {}) noexcept {
        static_assert(CommandCapacity <= kMaxInlineCommandCapacity,
                      "Use a smaller queue capacity or an explicit scratch path");
        if (!block.validate()) return fail_invalid_block();
        if (!snapshot.valid()) return fail_invalid_snapshot();

        std::array<graph::GraphTimedCommand, CommandCapacity> commands{};
        std::array<GraphRuntimeCommandDecision, CommandCapacity> command_results{};
        const auto command_count = queues.drain_commands_for_block(commands, block.frame_count);
        GraphRuntimeEventSink sink{
            &queues,
            [](void* user_data, const graph::GraphEvent& event) noexcept {
                auto* typed = static_cast<
                    graph::GraphRuntimeQueues<CommandCapacity, EventCapacity, MidiOutputCapacity>*>(
                        user_data);
                return typed->push_event_from_realtime(event);
            },
        };
        return process(block, snapshot,
                       std::span<const graph::GraphTimedCommand>(
                                 commands.data(), command_count),
                       std::span<GraphRuntimeCommandDecision>(
                           command_results.data(), command_count),
                       command_handler,
                       sink);
    }

    // Routing process() path: routes inter-node audio through `pool` per the
    // snapshot's buffer_assignment(). AudioInput nodes read the block's main
    // input bus into their output slots; AudioOutput nodes write their gathered
    // input slots to the main output bus; all other nodes have their
    // node_inputs gathered (summed) from upstream output slots and their
    // node_outputs pointed at their scratch slots before the binding runs.
    // Allocation-free: `pool` must already fit() the snapshot for this block.
    //
    // Feedback connections are honored as a one-block delay: gather adds each
    // feedback edge's previous-block slot, and after the walk the source's
    // output is captured into that slot for the next block (matching
    // host::SignalGraph's feedback_prev). The pool's zero-init gives the first
    // block silent feedback.
    GraphRuntimeExecutorResult process_routed(
        ProcessBlock& block,
        const GraphRuntimeSnapshot& snapshot,
        GraphRuntimeBufferPool& pool,
        std::span<const graph::GraphTimedCommand> commands = {},
        std::span<GraphRuntimeCommandDecision> command_results = {},
        GraphRuntimeCommandHandler command_handler = {},
        GraphRuntimeEventSink event_sink = {}) noexcept;

    GraphRuntimeExecutorStats stats() const noexcept;
    void reset_stats() noexcept;

private:
    GraphRuntimeExecutorResult fail_invalid_block() noexcept;
    GraphRuntimeExecutorResult fail_invalid_snapshot() noexcept;
    GraphRuntimeExecutorResult fail_command_scratch_too_small() noexcept;

    // Shared command-drain used by both process() paths; fills command_results
    // and updates result + stats. Returns false if command_results is too small.
    bool drain_commands(
        ProcessBlock& block,
        const graph::GraphRuntimePlan& plan,
        std::span<const graph::GraphTimedCommand> commands,
        std::span<GraphRuntimeCommandDecision> command_results,
        GraphRuntimeCommandHandler command_handler,
        GraphRuntimeEventSink event_sink,
        GraphRuntimeExecutorResult& result) noexcept;

    std::atomic<std::uint64_t> blocks_processed_{0};
    std::atomic<std::uint64_t> nodes_processed_{0};
    std::atomic<std::uint64_t> commands_drained_{0};
    std::atomic<std::uint64_t> commands_accepted_{0};
    std::atomic<std::uint64_t> commands_rejected_{0};
    std::atomic<std::uint64_t> events_dropped_{0};
    std::atomic<std::uint64_t> invalid_blocks_{0};
    std::atomic<std::uint64_t> invalid_snapshots_{0};
    std::atomic<std::uint64_t> command_scratch_failures_{0};
    std::atomic<std::uint64_t> node_failures_{0};
};

} // namespace pulp::format
