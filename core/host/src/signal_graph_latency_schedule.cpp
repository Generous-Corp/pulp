#include "signal_graph_latency_schedule.hpp"

#include <pulp/host/signal_graph_execution_snapshot.hpp>

#include <algorithm>
#include <optional>
#include <vector>

namespace pulp::host::detail {
namespace {

using Result = LatencyToOutputResult;
using Status = Result::Status;

void select_unknown(Result& selected, const Result& candidate) {
    const auto rank = [](Status status) {
        if (status == Status::QueryFailed) return 2;
        if (status == Status::Unsupported) return 1;
        return 0;
    };
    const int selected_rank = rank(selected.status);
    const int candidate_rank = rank(candidate.status);
    if (candidate_rank > selected_rank ||
        (candidate_rank == selected_rank && candidate_rank != 0 &&
         candidate.offending_node < selected.offending_node)) {
        selected = candidate;
    }
}

Result unknown_result(PluginSlot::LatencyQuery query, NodeId node) {
    if (query == PluginSlot::LatencyQuery::QueryFailed) {
        return {Status::QueryFailed, 0, node};
    }
    if (query == PluginSlot::LatencyQuery::Unsupported) {
        return {Status::Unsupported, 0, node};
    }
    return {};
}

} // namespace

std::unordered_map<NodeId, LatencyToOutputResult> build_latency_schedule(
    std::span<const NodeId> processing_order,
    std::span<const LatencyScheduleNode> nodes,
    std::span<const LatencyScheduleEdge> edges) {
    std::unordered_map<NodeId, const LatencyScheduleNode*> node_by_id;
    node_by_id.reserve(nodes.size());
    for (const auto& node : nodes) node_by_id.emplace(node.id, &node);

    std::unordered_map<NodeId, Result> schedule;
    schedule.reserve(nodes.size());
    std::unordered_map<NodeId, std::vector<const LatencyScheduleEdge*>> outgoing;
    outgoing.reserve(nodes.size());
    for (const auto& edge : edges) outgoing[edge.source].push_back(&edge);
    for (auto order_it = processing_order.rbegin();
         order_it != processing_order.rend(); ++order_it) {
        const NodeId id = *order_it;
        const auto node_it = node_by_id.find(id);
        if (node_it == node_by_id.end()) continue;
        const auto& node = *node_it->second;
        if (node.type == NodeType::AudioOutput) {
            schedule[id] = {Status::Available, 0, 0};
            continue;
        }

        bool reaches_output = false;
        bool ambiguous = false;
        std::optional<std::int64_t> known_samples;
        Result selected_unknown;
        const auto outgoing_it = outgoing.find(id);
        if (outgoing_it != outgoing.end()) {
            for (const auto* edge : outgoing_it->second) {
                const auto downstream_it = schedule.find(edge->destination);
                if (downstream_it == schedule.end() ||
                    downstream_it->second.status == Status::NoOutputPath) {
                    continue;
                }
                reaches_output = true;
                const auto& downstream = downstream_it->second;
                if (downstream.status == Status::QueryFailed ||
                    downstream.status == Status::Unsupported) {
                    select_unknown(selected_unknown, downstream);
                    continue;
                }
                if (downstream.status == Status::AmbiguousOutputLatency) {
                    ambiguous = true;
                    continue;
                }
                const std::int64_t samples = node.intrinsic_samples +
                                             edge->delay_samples +
                                             downstream.samples;
                if (known_samples && *known_samples != samples) ambiguous = true;
                if (!known_samples) known_samples = samples;
            }
        }

        if (!reaches_output) {
            schedule[id] = {Status::NoOutputPath, 0, 0};
            continue;
        }
        select_unknown(selected_unknown, unknown_result(node.query, id));
        if (selected_unknown.status == Status::QueryFailed ||
            selected_unknown.status == Status::Unsupported) {
            schedule[id] = selected_unknown;
        } else if (ambiguous) {
            schedule[id] = {Status::AmbiguousOutputLatency, 0, 0};
        } else {
            schedule[id] = {Status::Available, known_samples.value_or(0), 0};
        }
    }
    return schedule;
}

} // namespace pulp::host::detail

namespace pulp::host {

SignalGraph::PreparedLatencyMetadata SignalGraph::capture_latency_metadata_(
    PluginSlot& slot) {
    const auto query = slot.latency_query();
    return {
        query == PluginSlot::LatencyQuery::Available
            ? std::max(0, slot.latency_samples())
            : 0,
        query,
    };
}

void SignalGraph::build_latency_schedule_for_(CompiledGraph& cg) {
    std::vector<detail::LatencyScheduleNode> nodes;
    nodes.reserve(cg.shapes.size());
    for (const auto& [id, shape] : cg.shapes) {
        const auto runtime_it = cg.runtime.find(id);
        const std::int64_t intrinsic = runtime_it == cg.runtime.end()
            ? 0
            : std::max<std::int64_t>(
                  0, runtime_it->second.output_latency -
                         runtime_it->second.input_latency);
        const auto query_it = cg.plugin_latency_queries.find(id);
        nodes.push_back({
            id,
            shape.type,
            intrinsic,
            query_it == cg.plugin_latency_queries.end()
                ? PluginSlot::LatencyQuery::Available
                : query_it->second,
        });
    }

    std::vector<detail::LatencyScheduleEdge> edges;
    edges.reserve(cg.connections.size());
    for (std::size_t index = 0; index < cg.connections.size(); ++index) {
        const auto& connection = cg.connections[index];
        if (connection.feedback || !connection_affects_latency(connection)) continue;
        const std::int64_t delay = index < cg.connection_delays.size()
            ? std::max(0, cg.connection_delays[index].delay_samples)
            : 0;
        edges.push_back({connection.source_node, connection.dest_node, delay});
    }
    cg.latency_schedule = detail::build_latency_schedule(cg.order, nodes, edges);
}

LatencyToOutputResult SignalGraph::latency_to_output_for_(
    const CompiledGraph& cg, NodeId id) noexcept {
    const auto result = cg.latency_schedule.find(id);
    return result == cg.latency_schedule.end()
        ? LatencyToOutputResult{}
        : result->second;
}

LatencyToOutputResult SignalGraph::latency_to_output(NodeId id) const noexcept {
    auto read_guard = live_slot_.read();
    const auto* snapshot = read_guard.get();
    return snapshot ? latency_to_output_for_(*snapshot, id)
                    : LatencyToOutputResult{};
}

LatencyToOutputResult SignalGraph::ExecutionSnapshot::latency_to_output(
    NodeId id) const noexcept {
    return snapshot_ ? SignalGraph::latency_to_output_for_(*snapshot_, id)
                     : LatencyToOutputResult{};
}

} // namespace pulp::host
