#pragma once

#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/signal_graph.hpp>

#include <span>
#include <unordered_map>

namespace pulp::host::detail {

struct LatencyScheduleNode {
    NodeId id = 0;
    NodeType type = NodeType::Gain;
    std::int64_t intrinsic_samples = 0;
    PluginSlot::LatencyQuery query = PluginSlot::LatencyQuery::Available;
};

struct LatencyScheduleEdge {
    NodeId source = 0;
    NodeId destination = 0;
    std::int64_t delay_samples = 0;
};

struct LatencyBoundarySchedules {
    std::unordered_map<NodeId, LatencyToOutputResult> input;
    std::unordered_map<NodeId, LatencyToOutputResult> output;
};

LatencyBoundarySchedules build_latency_schedule(
    std::span<const NodeId> processing_order,
    std::span<const LatencyScheduleNode> nodes,
    std::span<const LatencyScheduleEdge> edges);

} // namespace pulp::host::detail
