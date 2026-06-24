#include <pulp/format/graph_runtime_executor.hpp>

#include <algorithm>
#include <array>
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

bool GraphRuntimeBufferPool::reset(std::uint32_t slot_count, std::uint32_t max_frames) {
    if (slot_count > 0 && max_frames == 0) return false;
    try {
        storage_.assign(static_cast<std::size_t>(slot_count) * max_frames, 0.0f);
    } catch (...) {
        clear();
        return false;
    }
    slot_count_ = slot_count;
    max_frames_ = max_frames;
    return true;
}

void GraphRuntimeBufferPool::clear() noexcept {
    storage_.clear();
    slot_count_ = 0;
    max_frames_ = 0;
}

bool GraphRuntimeSnapshot::reset(
    graph::GraphRuntimePlan plan,
    std::span<const GraphRuntimeNodeBinding> bindings) {
    clear();
    if (bindings.size() != plan.nodes.size()) return false;
    if (plan.processing_order_indices.size() != plan.nodes.size()) return false;
    for (std::uint32_t i = 0; i < plan.nodes.size(); ++i) {
        if (bindings[i].node_id != plan.nodes[i].id) return false;
    }
    for (const auto node_index : plan.processing_order_indices) {
        if (node_index >= plan.nodes.size()) return false;
    }

    try {
        auto assignment = graph::build_graph_runtime_buffer_assignment(plan);
        if (!assignment.ok) {
            clear();
            return false;
        }
        plan_ = std::move(plan);
        bindings_.assign(bindings.begin(), bindings.end());
        assignment_ = std::move(assignment);
    } catch (...) {
        clear();
        return false;
    }
    return true;
}

void GraphRuntimeSnapshot::clear() noexcept {
    plan_.clear();
    bindings_.clear();
    assignment_ = {};
}

bool GraphRuntimeSnapshot::valid() const noexcept {
    if (bindings_.size() != plan_.nodes.size()) return false;
    if (plan_.processing_order_indices.size() != plan_.nodes.size()) return false;
    if (assignment_.nodes.size() != plan_.nodes.size()) return false;
    for (std::uint32_t i = 0; i < plan_.nodes.size(); ++i) {
        if (bindings_[i].node_id != plan_.nodes[i].id) return false;
    }
    for (const auto node_index : plan_.processing_order_indices) {
        if (node_index >= plan_.nodes.size()) return false;
    }
    return true;
}

bool GraphRuntimeExecutor::drain_commands(
    ProcessBlock& block,
    const graph::GraphRuntimePlan& plan,
    std::span<const graph::GraphTimedCommand> commands,
    std::span<GraphRuntimeCommandDecision> command_results,
    GraphRuntimeCommandHandler command_handler,
    GraphRuntimeEventSink event_sink,
    GraphRuntimeExecutorResult& result) noexcept {
    if (command_results.size() < commands.size()) return false;

    result.commands_drained = static_cast<std::uint32_t>(commands.size());
    commands_drained_.fetch_add(result.commands_drained, std::memory_order_relaxed);

    for (const auto& command : commands) {
        auto status = GraphRuntimeCommandStatus::Accepted;
        if (command_requires_node(command.command.type) &&
            !contains_node(plan, command.command.node_id)) {
            status = GraphRuntimeCommandStatus::Rejected;
        } else if (command_handler.apply && command.block_offset != 0) {
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

    const auto& plan = snapshot.plan();
    const auto bindings = snapshot.bindings();

    GraphRuntimeExecutorResult result;
    if (!drain_commands(block, plan, commands, command_results, command_handler,
                        event_sink, result)) {
        return fail_command_scratch_too_small();
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

GraphRuntimeExecutorResult GraphRuntimeExecutor::process_routed(
    ProcessBlock& block,
    const GraphRuntimeSnapshot& snapshot,
    GraphRuntimeBufferPool& pool,
    std::span<const graph::GraphTimedCommand> commands,
    std::span<GraphRuntimeCommandDecision> command_results,
    GraphRuntimeCommandHandler command_handler,
    GraphRuntimeEventSink event_sink) noexcept {
    if (!block.validate()) return fail_invalid_block();
    if (!snapshot.valid()) return fail_invalid_snapshot();

    const auto& plan = snapshot.plan();
    const auto bindings = snapshot.bindings();
    const auto& assignment = snapshot.buffer_assignment();
    const auto frames = block.frame_count;

    if (assignment.has_feedback) {
        // Feedback needs previous-block slot capture (a later phase); the serial
        // gather would otherwise read silence on the feedback edge and silently
        // diverge from the host graph. Fail loudly instead.
        GraphRuntimeExecutorResult fail;
        fail.error = GraphRuntimeExecutorErrorCode::UnsupportedFeedbackEdge;
        return fail;
    }
    if (!pool.fits(snapshot, frames)) {
        GraphRuntimeExecutorResult fail;
        fail.error = GraphRuntimeExecutorErrorCode::BufferPoolTooSmall;
        return fail;
    }

    GraphRuntimeExecutorResult result;
    if (!drain_commands(block, plan, commands, command_results, command_handler,
                        event_sink, result)) {
        return fail_command_scratch_too_small();
    }

    const BusBuffer* in_bus =
        block.buses ? block.buses->first(BusDirection::Input, BusRole::Main) : nullptr;
    BusBuffer* out_bus =
        block.buses ? block.buses->first(BusDirection::Output, BusRole::Main) : nullptr;

    for (const auto node_index : plan.processing_order_indices) {
        if (node_index >= bindings.size()) return fail_invalid_snapshot();

        const auto& node = plan.nodes[node_index];
        const auto& slots = assignment.nodes[node_index];

        if (node.input_ports > kMaxRoutedPortsPerNode ||
            node.output_ports > kMaxRoutedPortsPerNode) {
            result.error = GraphRuntimeExecutorErrorCode::NodePortLimitExceeded;
            result.failed_node_index = node_index;
            node_failures_.fetch_add(1, std::memory_order_relaxed);
            return result;
        }

        // Gather: zero each input slot (unconnected ports stay silent), then
        // sum every non-feedback audio connection from its upstream output slot.
        for (std::uint32_t p = 0; p < node.input_ports; ++p) {
            if (float* dst = pool.slot_data(slots.input_base + p)) {
                std::fill_n(dst, frames, 0.0f);
            }
        }
        for (std::uint32_t c = 0; c < node.inbound_connection_count; ++c) {
            const auto conn_index =
                plan.inbound_connection_indices[node.first_inbound_connection + c];
            const auto& conn = plan.connections[conn_index];
            if (conn.feedback || conn.event) continue;
            const auto& src_slots = assignment.nodes[conn.source_index];
            const float* src = pool.slot_data(src_slots.output_base + conn.source_port);
            float* dst = pool.slot_data(slots.input_base + conn.dest_port);
            if (src == nullptr || dst == nullptr) continue;
            for (std::uint32_t f = 0; f < frames; ++f) dst[f] += src[f];
        }

        switch (node.kind) {
            case graph::GraphRuntimeNodeKind::AudioInput: {
                for (std::uint32_t p = 0; p < node.output_ports; ++p) {
                    float* dst = pool.slot_data(slots.output_base + p);
                    if (dst == nullptr) continue;
                    if (in_bus != nullptr && p < in_bus->input.num_channels()) {
                        std::copy_n(in_bus->input.channel_ptr(p), frames, dst);
                    } else {
                        std::fill_n(dst, frames, 0.0f);
                    }
                }
                break;
            }
            case graph::GraphRuntimeNodeKind::AudioOutput: {
                for (std::uint32_t p = 0; p < node.input_ports; ++p) {
                    if (out_bus == nullptr || p >= out_bus->output.num_channels()) continue;
                    const float* src = pool.slot_data(slots.input_base + p);
                    if (src == nullptr) continue;
                    std::copy_n(src, frames, out_bus->output.channel_ptr(p));
                }
                break;
            }
            default: {
                const auto& binding = bindings[node_index];
                if (!binding.process) {
                    if (binding.required) {
                        result.error =
                            GraphRuntimeExecutorErrorCode::MissingRequiredProcessor;
                        result.failed_node_index = node_index;
                        node_failures_.fetch_add(1, std::memory_order_relaxed);
                        return result;
                    }
                    break;  // optional no-op; its output slots stay silent
                }

                std::array<const float*, kMaxRoutedPortsPerNode> in_ptrs{};
                std::array<float*, kMaxRoutedPortsPerNode> out_ptrs{};
                for (std::uint32_t p = 0; p < node.input_ports; ++p) {
                    in_ptrs[p] = pool.slot_data(slots.input_base + p);
                }
                for (std::uint32_t p = 0; p < node.output_ports; ++p) {
                    out_ptrs[p] = pool.slot_data(slots.output_base + p);
                }

                GraphRuntimeNodeProcessContext context;
                context.plan = &plan;
                context.node = &node;
                context.node_index = node_index;
                context.command_results = command_results.first(commands.size());
                context.node_inputs = audio::BufferView<const float>(
                    in_ptrs.data(), node.input_ports, frames);
                context.node_outputs = audio::BufferView<float>(
                    out_ptrs.data(), node.output_ports, frames);
                if (!binding.process(block, context, binding.user_data)) {
                    result.error = GraphRuntimeExecutorErrorCode::NodeProcessorFailed;
                    result.failed_node_index = node_index;
                    node_failures_.fetch_add(1, std::memory_order_relaxed);
                    return result;
                }
                break;
            }
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
