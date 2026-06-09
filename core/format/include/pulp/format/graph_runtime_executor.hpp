#pragma once

#include <pulp/format/process_block.hpp>
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

private:
    graph::GraphRuntimePlan plan_;
    std::vector<GraphRuntimeNodeBinding> bindings_;
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

class GraphRuntimeExecutor {
public:
    static constexpr std::size_t kMaxInlineCommandCapacity = 256;

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

    GraphRuntimeExecutorStats stats() const noexcept;
    void reset_stats() noexcept;

private:
    GraphRuntimeExecutorResult fail_invalid_block() noexcept;
    GraphRuntimeExecutorResult fail_invalid_snapshot() noexcept;
    GraphRuntimeExecutorResult fail_command_scratch_too_small() noexcept;

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
