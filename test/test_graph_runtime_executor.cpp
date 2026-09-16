#include <catch2/catch_test_macros.hpp>

#include <pulp/format/graph_runtime_executor.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace {

using pulp::format::GraphRuntimeExecutor;
using pulp::format::GraphRuntimeExecutorErrorCode;
using pulp::format::GraphRuntimeCommandHandler;
using pulp::format::GraphRuntimeCommandStatus;
using pulp::format::GraphRuntimeNodeBinding;
using pulp::format::GraphRuntimeNodeProcessContext;
using pulp::format::GraphRuntimeSnapshot;
using pulp::format::ProcessBlock;
using pulp::graph::GraphCommand;
using pulp::graph::GraphCommandTiming;
using pulp::graph::GraphCommandTimingType;
using pulp::graph::GraphRuntimeConnectionSpec;
using pulp::graph::GraphRuntimeNodeKind;
using pulp::graph::GraphRuntimeNodeSpec;
using pulp::graph::GraphRuntimeQueues;
using pulp::graph::GraphTimedCommand;
using pulp::graph::NodeId;

ProcessBlock valid_block() {
    ProcessBlock block;
    block.sample_rate = 48000.0;
    block.frame_count = 64;
    block.render_speed = 1.0;
    return block;
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
    return {source, source_port, dest, dest_port, false};
}

GraphCommand command(std::uint64_t sequence_id,
                     NodeId node_id,
                     std::uint32_t offset = 0) {
    GraphCommand c;
    c.sequence_id = sequence_id;
    c.node_id = node_id;
    c.timing = {GraphCommandTimingType::BlockOffset, offset};
    c.value = static_cast<float>(sequence_id);
    return c;
}

struct VisitLog {
    std::array<NodeId, 8> nodes{};
    std::uint32_t count = 0;
    std::uint32_t command_count = 0;
    std::uint32_t accepted_commands = 0;
    std::uint32_t rejected_commands = 0;
};

bool record_visit(ProcessBlock& block,
                  const GraphRuntimeNodeProcessContext& context,
                  void* user_data) noexcept {
    auto* log = static_cast<VisitLog*>(user_data);
    if (block.frame_count != 64 || context.node == nullptr || context.plan == nullptr) {
        return false;
    }
    if (log->count >= log->nodes.size()) return false;
    log->nodes[log->count++] = context.node->id;
    log->command_count = static_cast<std::uint32_t>(context.command_results.size());
    for (const auto& decision : context.command_results) {
        if (decision.status == GraphRuntimeCommandStatus::Accepted) {
            ++log->accepted_commands;
        } else {
            ++log->rejected_commands;
        }
    }
    return true;
}

bool fail_node(ProcessBlock&,
               const GraphRuntimeNodeProcessContext&,
               void*) noexcept {
    return false;
}

GraphRuntimeCommandStatus reject_commands(ProcessBlock&,
                                          const pulp::graph::GraphRuntimePlan&,
                                          const GraphTimedCommand&,
                                          void*) noexcept {
    return GraphRuntimeCommandStatus::Rejected;
}

GraphRuntimeCommandStatus count_applied_commands(ProcessBlock&,
                                                 const pulp::graph::GraphRuntimePlan&,
                                                 const GraphTimedCommand&,
                                                 void* user_data) noexcept {
    auto* count = static_cast<std::uint32_t*>(user_data);
    ++(*count);
    return GraphRuntimeCommandStatus::Accepted;
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

pulp::graph::GraphRuntimePlanResult dense_plan(std::span<const std::uint32_t> lanes_per_node) {
    std::vector<GraphRuntimeNodeSpec> nodes;
    std::vector<GraphRuntimeConnectionSpec> connections;
    nodes.push_back(node(1, 0, 1, GraphRuntimeNodeKind::AudioInput));
    NodeId next_id = 2;
    std::uint32_t next_param = 1;
    for (const auto lane_count : lanes_per_node) {
        const NodeId destination = next_id++;
        nodes.push_back(node(destination, 1, 0));
        for (std::uint32_t lane = 0; lane < lane_count; ++lane) {
            auto connection = connect(1, 0, destination, 0);
            connection.kind = pulp::graph::GraphRuntimeConnectionKind::Automation;
            connection.automation.param_id = next_param++;
            connection.automation.audio_rate = true;
            connections.push_back(connection);
        }
    }
    return pulp::graph::build_graph_runtime_plan(nodes, connections);
}

pulp::graph::GraphRuntimePlanResult sparse_plan(std::uint32_t lane_count) {
    const std::array nodes = {
        node(1, 0, 1, GraphRuntimeNodeKind::AudioInput),
        node(2, 1, 0),
    };
    std::vector<GraphRuntimeConnectionSpec> connections;
    connections.reserve(lane_count);
    for (std::uint32_t lane = 0; lane < lane_count; ++lane) {
        auto connection = connect(1, 0, 2, 0);
        connection.kind = pulp::graph::GraphRuntimeConnectionKind::Automation;
        connection.automation.param_id = lane + 1;
        connections.push_back(connection);
    }
    return pulp::graph::build_graph_runtime_plan(nodes, connections);
}

std::vector<GraphRuntimeNodeBinding> dense_bindings(const pulp::graph::GraphRuntimePlan& plan) {
    std::vector<GraphRuntimeNodeBinding> bindings;
    bindings.reserve(plan.nodes.size());
    for (const auto& planned : plan.nodes) {
        GraphRuntimeNodeBinding binding{planned.id, nullptr, nullptr, false};
        binding.audio_rate_modulation_delivery =
            pulp::format::AudioRateModulationDelivery::DenseViews;
        bindings.push_back(binding);
    }
    return bindings;
}

} // namespace

TEST_CASE("GraphRuntimeAutomationScratch enforces dense admission limits and reports bytes",
          "[format][graph-runtime][executor][automation][audio-rate][limits]") {
    using Scratch = pulp::format::GraphRuntimeAutomationScratch;

    auto per_node_limit = dense_plan(std::array<std::uint32_t, 1>{64});
    REQUIRE(per_node_limit.ok());
    const auto per_node_bindings = dense_bindings(per_node_limit.plan);
    Scratch scratch;
    REQUIRE(scratch.reset(per_node_limit.plan, per_node_bindings, Scratch::kMaxDenseFrames));
    const auto stats = scratch.prepared_stats();
    REQUIRE(stats.dense_lane_count == 64);
    REQUIRE(stats.dense_sample_bytes == 4'194'304);
    REQUIRE(stats.dense_descriptor_bytes ==
            64 * (sizeof(std::uint32_t) * 2 + sizeof(pulp::format::AudioRateModulationView)));
    REQUIRE(stats.dense_accumulator_bytes ==
            64 * (sizeof(float) * 2 + sizeof(std::uint8_t) * 2 + sizeof(std::uint32_t)));
    REQUIRE(stats.total_dense_bytes() == stats.dense_sample_bytes + stats.dense_descriptor_bytes +
                                             stats.dense_accumulator_bytes +
                                             stats.dense_node_index_bytes);

    auto per_node_over = dense_plan(std::array<std::uint32_t, 1>{65});
    REQUIRE(per_node_over.ok());
    REQUIRE_FALSE(scratch.reset(per_node_over.plan, 64));
    REQUIRE(scratch.prepared_stats().dense_lane_count == 0);

    auto graph_limit = dense_plan(std::array<std::uint32_t, 4>{64, 64, 64, 64});
    REQUIRE(graph_limit.ok());
    const auto graph_limit_bindings = dense_bindings(graph_limit.plan);
    REQUIRE(scratch.reset(graph_limit.plan, graph_limit_bindings, Scratch::kMaxDenseFrames));
    REQUIRE(scratch.prepared_stats().dense_lane_count == 256);
    REQUIRE(scratch.prepared_stats().dense_sample_bytes == 16'777'216);

    auto graph_over = dense_plan(std::array<std::uint32_t, 5>{64, 64, 64, 64, 1});
    REQUIRE(graph_over.ok());
    REQUIRE_FALSE(scratch.reset(graph_over.plan, 64));

    REQUIRE_FALSE(scratch.reset(per_node_limit.plan, Scratch::kMaxDenseFrames + 1));
    REQUIRE_FALSE(scratch.reset(per_node_limit.plan, std::numeric_limits<std::uint32_t>::max()));
}

TEST_CASE("GraphRuntimeAutomationScratch separates legacy queue admission from dense views",
          "[format][graph-runtime][executor][automation][audio-rate][limits]") {
    using Scratch = pulp::format::GraphRuntimeAutomationScratch;
    auto one_lane = dense_plan(std::array<std::uint32_t, 1>{1});
    REQUIRE(one_lane.ok());

    Scratch scratch;
    REQUIRE(scratch.reset(one_lane.plan, 1024));
    REQUIRE_FALSE(scratch.reset(one_lane.plan, 1025));

    const auto bindings = dense_bindings(one_lane.plan);
    REQUIRE(scratch.reset(one_lane.plan, bindings, 4096));
    REQUIRE(scratch.prepared_stats().dense_sample_bytes == 4096 * sizeof(float));

    auto sparse_limit = sparse_plan(512);
    REQUIRE(sparse_limit.ok());
    const auto sparse_limit_bindings = dense_bindings(sparse_limit.plan);
    REQUIRE(scratch.reset(sparse_limit.plan, sparse_limit_bindings, 64));
    auto sparse_over = sparse_plan(513);
    REQUIRE(sparse_over.ok());
    const auto sparse_over_bindings = dense_bindings(sparse_over.plan);
    REQUIRE_FALSE(scratch.reset(sparse_over.plan, sparse_over_bindings, 64));

    REQUIRE_FALSE(scratch.reset(one_lane.plan,
                                std::span<const GraphRuntimeNodeBinding>(bindings).first(1), 64));
}

TEST_CASE("GraphRuntimeExecutor visits snapshot nodes in plan order",
          "[format][graph-runtime][executor]") {
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

    GraphRuntimeExecutor executor;
    auto block = valid_block();
    const auto result = executor.process(block, snapshot);

    REQUIRE(result.ok());
    REQUIRE(result.nodes_processed == 3);
    REQUIRE(log.count == 3);
    REQUIRE(log.nodes[0] == 10);
    REQUIRE(log.nodes[1] == 20);
    REQUIRE(log.nodes[2] == 30);
    REQUIRE(executor.stats().blocks_processed == 1);
    REQUIRE(executor.stats().nodes_processed == 3);
}

TEST_CASE("GraphRuntimeExecutor drains commands and publishes command events",
          "[format][graph-runtime][executor][queue]") {
    const std::array nodes = {
        node(10, 0, 0),
    };
    VisitLog log;
    const std::array bindings = {
        GraphRuntimeNodeBinding{10, record_visit, &log, true},
    };
    auto snapshot = make_snapshot(nodes, {}, bindings);

    GraphRuntimeQueues<4, 4, 4> queues;
    REQUIRE(queues.enqueue_command(command(1, 10, 9)));
    REQUIRE(queues.enqueue_command(command(2, 99, 3)));

    GraphRuntimeExecutor executor;
    auto block = valid_block();
    const auto result = executor.process(block, snapshot, queues);

    REQUIRE(result.ok());
    REQUIRE(result.commands_drained == 2);
    REQUIRE(result.commands_accepted == 1);
    REQUIRE(result.commands_rejected == 1);
    REQUIRE(log.command_count == 2);

    pulp::graph::GraphEvent event;
    REQUIRE(queues.pop_event(event));
    REQUIRE(event.sequence_id == 2);
    REQUIRE(event.type == pulp::graph::GraphEventType::CommandRejected);
    REQUIRE(event.block_offset == 3);
    REQUIRE(queues.pop_event(event));
    REQUIRE(event.sequence_id == 1);
    REQUIRE(event.type == pulp::graph::GraphEventType::CommandAccepted);
    REQUIRE(event.block_offset == 9);
    REQUIRE_FALSE(queues.pop_event(event));

    const auto stats = executor.stats();
    REQUIRE(stats.commands_drained == 2);
    REQUIRE(stats.commands_accepted == 1);
    REQUIRE(stats.commands_rejected == 1);
}

TEST_CASE("GraphRuntimeExecutor reports handler-rejected command decisions",
          "[format][graph-runtime][executor][queue]") {
    const std::array nodes = {
        node(10, 0, 0),
    };
    VisitLog log;
    const std::array bindings = {
        GraphRuntimeNodeBinding{10, record_visit, &log, true},
    };
    auto snapshot = make_snapshot(nodes, {}, bindings);

    GraphRuntimeQueues<2, 2, 2> queues;
    REQUIRE(queues.enqueue_command(command(1, 10)));

    GraphRuntimeExecutor executor;
    auto block = valid_block();
    const auto result = executor.process(
        block, snapshot, queues, GraphRuntimeCommandHandler{reject_commands, nullptr});

    REQUIRE(result.ok());
    REQUIRE(result.commands_drained == 1);
    REQUIRE(result.commands_accepted == 0);
    REQUIRE(result.commands_rejected == 1);
    REQUIRE(log.command_count == 1);
    REQUIRE(log.accepted_commands == 0);
    REQUIRE(log.rejected_commands == 1);

    pulp::graph::GraphEvent event;
    REQUIRE(queues.pop_event(event));
    REQUIRE(event.sequence_id == 1);
    REQUIRE(event.type == pulp::graph::GraphEventType::CommandRejected);
}

TEST_CASE("GraphRuntimeExecutor does not apply nonzero block-offset commands at block start",
          "[format][graph-runtime][executor][queue]") {
    const std::array nodes = {
        node(10, 0, 0),
    };
    VisitLog log;
    const std::array bindings = {
        GraphRuntimeNodeBinding{10, record_visit, &log, true},
    };
    auto snapshot = make_snapshot(nodes, {}, bindings);

    GraphRuntimeQueues<2, 2, 2> queues;
    REQUIRE(queues.enqueue_command(command(1, 10, 7)));

    std::uint32_t applied = 0;
    GraphRuntimeExecutor executor;
    auto block = valid_block();
    const auto result = executor.process(
        block,
        snapshot,
        queues,
        GraphRuntimeCommandHandler{count_applied_commands, &applied});

    REQUIRE(result.ok());
    REQUIRE(result.commands_drained == 1);
    REQUIRE(result.commands_accepted == 0);
    REQUIRE(result.commands_rejected == 1);
    REQUIRE(applied == 0);

    pulp::graph::GraphEvent event;
    REQUIRE(queues.pop_event(event));
    REQUIRE(event.sequence_id == 1);
    REQUIRE(event.type == pulp::graph::GraphEventType::CommandRejected);
    REQUIRE(event.block_offset == 7);
}

TEST_CASE("GraphRuntimeExecutor leaves queued commands intact for invalid blocks",
          "[format][graph-runtime][executor][queue]") {
    const std::array nodes = {
        node(10, 0, 0),
    };
    VisitLog log;
    const std::array bindings = {
        GraphRuntimeNodeBinding{10, record_visit, &log, true},
    };
    auto snapshot = make_snapshot(nodes, {}, bindings);

    GraphRuntimeQueues<2, 2, 2> queues;
    REQUIRE(queues.enqueue_command(command(1, 10)));

    GraphRuntimeExecutor executor;
    auto invalid = valid_block();
    invalid.frame_count = 0;
    const auto invalid_result = executor.process(invalid, snapshot, queues);
    REQUIRE_FALSE(invalid_result.ok());
    REQUIRE(invalid_result.error == GraphRuntimeExecutorErrorCode::InvalidProcessBlock);
    REQUIRE(executor.stats().invalid_blocks == 1);

    auto block = valid_block();
    const auto valid_result = executor.process(block, snapshot, queues);
    REQUIRE(valid_result.ok());
    REQUIRE(valid_result.commands_drained == 1);
}

TEST_CASE("GraphRuntimeSnapshot validates binding node identity",
          "[format][graph-runtime][executor]") {
    const std::array nodes = {
        node(10, 0, 0),
    };
    auto plan = pulp::graph::build_graph_runtime_plan(nodes, {});
    REQUIRE(plan.ok());

    const std::array bindings = {
        GraphRuntimeNodeBinding{99, record_visit, nullptr, true},
    };
    GraphRuntimeSnapshot snapshot;
    REQUIRE_FALSE(snapshot.reset(std::move(plan.plan), bindings));
    REQUIRE(snapshot.valid());
    REQUIRE(snapshot.node_count() == 0);
}

TEST_CASE("GraphRuntimeSnapshot clears stale state after failed reset",
          "[format][graph-runtime][executor]") {
    const std::array nodes = {
        node(10, 0, 0),
    };
    const std::array valid_bindings = {
        GraphRuntimeNodeBinding{10, record_visit, nullptr, true},
    };
    auto snapshot = make_snapshot(nodes, {}, valid_bindings);
    REQUIRE(snapshot.node_count() == 1);

    auto plan = pulp::graph::build_graph_runtime_plan(nodes, {});
    REQUIRE(plan.ok());
    const std::array invalid_bindings = {
        GraphRuntimeNodeBinding{99, record_visit, nullptr, true},
    };

    REQUIRE_FALSE(snapshot.reset(std::move(plan.plan), invalid_bindings));
    REQUIRE(snapshot.valid());
    REQUIRE(snapshot.node_count() == 0);
    REQUIRE(snapshot.bindings().empty());
}

TEST_CASE("GraphRuntimeExecutor distinguishes optional and required missing processors",
          "[format][graph-runtime][executor]") {
    const std::array nodes = {
        node(10, 0, 1, GraphRuntimeNodeKind::AudioInput),
        node(20, 1, 0),
    };
    const std::array connections = {
        connect(10, 0, 20, 0),
    };
    VisitLog log;
    const std::array bindings = {
        GraphRuntimeNodeBinding{10, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{20, record_visit, &log, true},
    };
    auto snapshot = make_snapshot(nodes, connections, bindings);

    GraphRuntimeExecutor executor;
    auto block = valid_block();
    auto result = executor.process(block, snapshot);
    REQUIRE(result.ok());
    REQUIRE(result.nodes_processed == 1);
    REQUIRE(log.count == 1);
    REQUIRE(log.nodes[0] == 20);

    const std::array failing_bindings = {
        GraphRuntimeNodeBinding{10, nullptr, nullptr, true},
        GraphRuntimeNodeBinding{20, record_visit, &log, true},
    };
    auto failing_snapshot = make_snapshot(nodes, connections, failing_bindings);
    result = executor.process(block, failing_snapshot);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error == GraphRuntimeExecutorErrorCode::MissingRequiredProcessor);
    REQUIRE(result.failed_node_index == 0);
    REQUIRE(executor.stats().node_failures == 1);
}

TEST_CASE("GraphRuntimeExecutor reports node processor failures",
          "[format][graph-runtime][executor]") {
    const std::array nodes = {
        node(10, 0, 0),
    };
    const std::array bindings = {
        GraphRuntimeNodeBinding{10, fail_node, nullptr, true},
    };
    auto snapshot = make_snapshot(nodes, {}, bindings);

    GraphRuntimeExecutor executor;
    auto block = valid_block();
    const auto result = executor.process(block, snapshot);

    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error == GraphRuntimeExecutorErrorCode::NodeProcessorFailed);
    REQUIRE(result.failed_node_index == 0);
    REQUIRE(executor.stats().node_failures == 1);
}
