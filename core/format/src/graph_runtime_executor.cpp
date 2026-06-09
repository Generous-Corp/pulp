#include <pulp/format/graph_runtime_executor.hpp>

#include <algorithm>
#include <utility>

namespace pulp::format {
namespace {

bool command_requires_node(graph::GraphCommandType type) noexcept {
    switch (type) {
        case graph::GraphCommandType::SetNodeGain:
        case graph::GraphCommandType::SetNodeParameter:
        case graph::GraphCommandType::SetNodeBypass:
        case graph::GraphCommandType::ResetNode:
        case graph::GraphCommandType::InjectMidi:
            return true;
        case graph::GraphCommandType::TransportJump:
        case graph::GraphCommandType::ActivateSnapshot:
        case graph::GraphCommandType::DeactivateSnapshot:
            return false;
    }
    return true;
}

bool contains_node(const graph::GraphRuntimePlan& plan,
                   graph::NodeId node_id) noexcept {
    return std::any_of(plan.nodes.begin(), plan.nodes.end(),
                       [node_id](const graph::GraphRuntimeNodePlan& node) {
                           return node.id == node_id;
                       });
}

graph::GraphEvent command_event(const graph::GraphTimedCommand& command,
                                graph::GraphEventType type) noexcept {
    graph::GraphEvent event;
    event.sequence_id = command.command.sequence_id;
    event.type = type;
    event.node_id = command.command.node_id;
    event.target_id = command.command.target_id;
    event.block_offset = command.block_offset;
    event.value = command.command.value;
    event.bool_value = command.command.bool_value;
    return event;
}

} // namespace

bool GraphRuntimeSnapshot::reset(
    graph::GraphRuntimePlan plan,
    std::span<const GraphRuntimeNodeBinding> bindings) {
    if (bindings.size() != plan.nodes.size()) return false;
    if (plan.processing_order_indices.size() != plan.nodes.size()) return false;
    for (std::uint32_t i = 0; i < plan.nodes.size(); ++i) {
        if (bindings[i].node_id != plan.nodes[i].id) return false;
    }
    for (const auto node_index : plan.processing_order_indices) {
        if (node_index >= plan.nodes.size()) return false;
    }

    try {
        plan_ = std::move(plan);
        bindings_.assign(bindings.begin(), bindings.end());
    } catch (...) {
        clear();
        return false;
    }
    return true;
}

void GraphRuntimeSnapshot::clear() noexcept {
    plan_.clear();
    bindings_.clear();
}

bool GraphRuntimeSnapshot::valid() const noexcept {
    if (bindings_.size() != plan_.nodes.size()) return false;
    if (plan_.processing_order_indices.size() != plan_.nodes.size()) return false;
    for (std::uint32_t i = 0; i < plan_.nodes.size(); ++i) {
        if (bindings_[i].node_id != plan_.nodes[i].id) return false;
    }
    for (const auto node_index : plan_.processing_order_indices) {
        if (node_index >= plan_.nodes.size()) return false;
    }
    return true;
}

GraphRuntimeExecutorResult GraphRuntimeExecutor::process(
    ProcessBlock& block,
    const GraphRuntimeSnapshot& snapshot,
    std::span<const graph::GraphTimedCommand> commands,
    std::span<GraphRuntimeCommandDecision> command_results,
    GraphRuntimeCommandHandler command_handler,
    GraphRuntimeEventSink event_sink) noexcept {
    if (!block.validate()) return fail_invalid_block();
    if (!snapshot.valid()) return fail_invalid_snapshot();
    if (command_results.size() < commands.size()) return fail_command_scratch_too_small();

    const auto& plan = snapshot.plan();
    const auto bindings = snapshot.bindings();

    GraphRuntimeExecutorResult result;
    result.commands_drained = static_cast<std::uint32_t>(commands.size());
    commands_drained_.fetch_add(result.commands_drained, std::memory_order_relaxed);

    for (const auto& command : commands) {
        auto status = GraphRuntimeCommandStatus::Accepted;
        if (command_requires_node(command.command.type) &&
            !contains_node(plan, command.command.node_id)) {
            status = GraphRuntimeCommandStatus::Rejected;
        } else if (command_handler.apply) {
            status = command_handler.apply(block, plan, command, command_handler.user_data);
        }

        const auto accepted = status == GraphRuntimeCommandStatus::Accepted;
        if (accepted) {
            ++result.commands_accepted;
            commands_accepted_.fetch_add(1, std::memory_order_relaxed);
        } else {
            ++result.commands_rejected;
            commands_rejected_.fetch_add(1, std::memory_order_relaxed);
        }

        command_results[result.commands_accepted + result.commands_rejected - 1] = {
            command,
            status,
        };

        const auto event_type = accepted ? graph::GraphEventType::CommandAccepted
                                         : graph::GraphEventType::CommandRejected;
        if (!event_sink.push(command_event(command, event_type))) {
            ++result.events_dropped;
            events_dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    for (const auto node_index : plan.processing_order_indices) {
        if (node_index >= bindings.size()) return fail_invalid_snapshot();

        const auto& binding = bindings[node_index];
        if (!binding.process) {
            if (binding.required) {
                result.error = GraphRuntimeExecutorErrorCode::MissingRequiredProcessor;
                result.failed_node_index = node_index;
                node_failures_.fetch_add(1, std::memory_order_relaxed);
                return result;
            }
            continue;
        }

        GraphRuntimeNodeProcessContext context;
        context.plan = &plan;
        context.node = &plan.nodes[node_index];
        context.node_index = node_index;
        context.command_results = command_results.first(commands.size());
        if (!binding.process(block, context, binding.user_data)) {
            result.error = GraphRuntimeExecutorErrorCode::NodeProcessorFailed;
            result.failed_node_index = node_index;
            node_failures_.fetch_add(1, std::memory_order_relaxed);
            return result;
        }

        ++result.nodes_processed;
        nodes_processed_.fetch_add(1, std::memory_order_relaxed);
    }

    blocks_processed_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

GraphRuntimeExecutorStats GraphRuntimeExecutor::stats() const noexcept {
    return {
        blocks_processed_.load(std::memory_order_relaxed),
        nodes_processed_.load(std::memory_order_relaxed),
        commands_drained_.load(std::memory_order_relaxed),
        commands_accepted_.load(std::memory_order_relaxed),
        commands_rejected_.load(std::memory_order_relaxed),
        events_dropped_.load(std::memory_order_relaxed),
        invalid_blocks_.load(std::memory_order_relaxed),
        invalid_snapshots_.load(std::memory_order_relaxed),
        command_scratch_failures_.load(std::memory_order_relaxed),
        node_failures_.load(std::memory_order_relaxed),
    };
}

void GraphRuntimeExecutor::reset_stats() noexcept {
    blocks_processed_.store(0, std::memory_order_relaxed);
    nodes_processed_.store(0, std::memory_order_relaxed);
    commands_drained_.store(0, std::memory_order_relaxed);
    commands_accepted_.store(0, std::memory_order_relaxed);
    commands_rejected_.store(0, std::memory_order_relaxed);
    events_dropped_.store(0, std::memory_order_relaxed);
    invalid_blocks_.store(0, std::memory_order_relaxed);
    invalid_snapshots_.store(0, std::memory_order_relaxed);
    command_scratch_failures_.store(0, std::memory_order_relaxed);
    node_failures_.store(0, std::memory_order_relaxed);
}

GraphRuntimeExecutorResult GraphRuntimeExecutor::fail_invalid_block() noexcept {
    invalid_blocks_.fetch_add(1, std::memory_order_relaxed);
    GraphRuntimeExecutorResult result;
    result.error = GraphRuntimeExecutorErrorCode::InvalidProcessBlock;
    return result;
}

GraphRuntimeExecutorResult GraphRuntimeExecutor::fail_invalid_snapshot() noexcept {
    invalid_snapshots_.fetch_add(1, std::memory_order_relaxed);
    GraphRuntimeExecutorResult result;
    result.error = GraphRuntimeExecutorErrorCode::InvalidSnapshot;
    return result;
}

GraphRuntimeExecutorResult GraphRuntimeExecutor::fail_command_scratch_too_small() noexcept {
    command_scratch_failures_.fetch_add(1, std::memory_order_relaxed);
    GraphRuntimeExecutorResult result;
    result.error = GraphRuntimeExecutorErrorCode::CommandScratchTooSmall;
    return result;
}

} // namespace pulp::format
