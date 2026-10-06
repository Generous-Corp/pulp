#include <pulp/graph/graph_runtime_plan.hpp>

#include <algorithm>
#include <cassert>
#include <functional>
#include <limits>
#include <queue>
#include <tuple>
#include <utility>

namespace pulp::graph {
namespace {

GraphRuntimePlanResult fail(GraphRuntimePlanErrorCode code,
                            std::uint32_t index,
                            NodeId node_id,
                            std::string message) {
    GraphRuntimePlanResult result;
    result.error = {code, index, node_id, std::move(message)};
    return result;
}

bool limits_valid(const GraphRuntimeLimits& limits) {
    return limits.max_nodes > 0 &&
           limits.max_connections > 0 &&
           limits.max_ports_per_node > 0 &&
           limits.max_total_ports > 0;
}

struct NodeLookupEntry {
    NodeId id = 0;
    std::uint32_t index = 0;
};

std::uint32_t find_node_index(std::span<const NodeLookupEntry> lookup,
                              NodeId id) {
    const auto it = std::lower_bound(
        lookup.begin(), lookup.end(), id,
        [](const NodeLookupEntry& entry, NodeId wanted) {
            return entry.id < wanted;
        });
    if (it == lookup.end() || it->id != id) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return it->index;
}

} // namespace

void GraphRuntimePlan::clear() {
    nodes.clear();
    connections.clear();
    inbound_connection_indices.clear();
    outbound_connection_indices.clear();
    processing_order_indices.clear();
}

std::uint32_t GraphRuntimePlan::node_count() const noexcept {
    return static_cast<std::uint32_t>(nodes.size());
}

std::uint32_t GraphRuntimePlan::connection_count() const noexcept {
    return static_cast<std::uint32_t>(connections.size());
}

GraphRuntimePlanResult build_graph_runtime_plan(
    std::span<const GraphRuntimeNodeSpec> node_specs,
    std::span<const GraphRuntimeConnectionSpec> connection_specs,
    GraphRuntimeLimits limits) {
    if (!limits_valid(limits)) {
        return fail(GraphRuntimePlanErrorCode::InvalidLimits, 0, 0,
                    "graph runtime limits must all be positive");
    }
    if (node_specs.size() > limits.max_nodes) {
        return fail(GraphRuntimePlanErrorCode::TooManyNodes,
                    static_cast<std::uint32_t>(node_specs.size()), 0,
                    "graph has more nodes than the configured runtime limit");
    }
    if (connection_specs.size() > limits.max_connections) {
        return fail(GraphRuntimePlanErrorCode::TooManyConnections,
                    static_cast<std::uint32_t>(connection_specs.size()), 0,
                    "graph has more connections than the configured runtime limit");
    }

    GraphRuntimePlanResult result;
    try {
        result.plan.nodes.reserve(node_specs.size());
        result.plan.connections.reserve(connection_specs.size());
        result.plan.inbound_connection_indices.resize(connection_specs.size());
        result.plan.outbound_connection_indices.resize(connection_specs.size());
        result.plan.processing_order_indices.reserve(node_specs.size());

        std::vector<NodeLookupEntry> lookup;
        lookup.reserve(node_specs.size());

        std::uint64_t total_ports = 0;
        for (std::uint32_t i = 0; i < node_specs.size(); ++i) {
            const auto& spec = node_specs[i];
            if (spec.id == 0) {
                return fail(GraphRuntimePlanErrorCode::InvalidNodeId, i, spec.id,
                            "graph node id 0 is reserved");
            }
            if (spec.input_ports > limits.max_ports_per_node ||
                spec.output_ports > limits.max_ports_per_node ||
                spec.event_input_ports > limits.max_ports_per_node ||
                spec.event_output_ports > limits.max_ports_per_node) {
                return fail(GraphRuntimePlanErrorCode::InvalidNodePortCount, i, spec.id,
                            "graph node exceeds the per-node port limit");
            }
            total_ports += spec.input_ports;
            total_ports += spec.output_ports;
            total_ports += spec.event_input_ports;
            total_ports += spec.event_output_ports;
            if (total_ports > limits.max_total_ports) {
                return fail(GraphRuntimePlanErrorCode::TooManyPorts, i, spec.id,
                            "graph exceeds the total port limit");
            }

            lookup.push_back({spec.id, i});
            result.plan.nodes.push_back({
                spec.id,
                spec.kind,
                spec.input_ports,
                spec.output_ports,
                spec.event_input_ports,
                spec.event_output_ports,
                spec.persistent_output,
                spec.latency_samples,
                0,
                0,
                0,
                0,
            });
        }

        std::sort(lookup.begin(), lookup.end(), [](const auto& a, const auto& b) {
            return a.id < b.id;
        });
        for (std::uint32_t i = 1; i < lookup.size(); ++i) {
            if (lookup[i].id == lookup[i - 1].id) {
                return fail(GraphRuntimePlanErrorCode::DuplicateNodeId,
                            std::max(lookup[i - 1].index, lookup[i].index),
                            lookup[i].id,
                            "graph contains duplicate node ids");
            }
        }

        std::vector<std::uint32_t> inbound_counts(node_specs.size(), 0);
        std::vector<std::uint32_t> outbound_counts(node_specs.size(), 0);

        for (std::uint32_t i = 0; i < connection_specs.size(); ++i) {
            const auto& spec = connection_specs[i];
            const auto source_index = find_node_index(lookup, spec.source_node);
            if (source_index == std::numeric_limits<std::uint32_t>::max()) {
                return fail(GraphRuntimePlanErrorCode::UnknownSourceNode, i,
                            spec.source_node,
                            "graph connection references an unknown source node");
            }
            const auto dest_index = find_node_index(lookup, spec.dest_node);
            if (dest_index == std::numeric_limits<std::uint32_t>::max()) {
                return fail(GraphRuntimePlanErrorCode::UnknownDestinationNode, i,
                            spec.dest_node,
                            "graph connection references an unknown destination node");
            }

            const auto& source = result.plan.nodes[source_index];
            const auto source_ports = is_event(spec) ? source.event_output_ports
                                                     : source.output_ports;
            if (spec.source_port >= source_ports) {
                return fail(GraphRuntimePlanErrorCode::SourcePortOutOfRange, i,
                            spec.source_node,
                            is_event(spec)
                                ? "graph event connection source port is out of range"
                                : "graph connection source port is out of range");
            }
            // An automation connection targets a destination PARAMETER, not an
            // audio/event input port (its dest_port is a conventional 0), so the
            // input-port range check does not apply. Its source IS an audio
            // output port, so the source check above still holds.
            if (!is_automation_conn(spec)) {
                const auto& dest = result.plan.nodes[dest_index];
                const auto dest_ports = is_event(spec) ? dest.event_input_ports
                                                       : dest.input_ports;
                if (spec.dest_port >= dest_ports) {
                    return fail(GraphRuntimePlanErrorCode::DestinationPortOutOfRange, i,
                                spec.dest_node,
                                is_event(spec)
                                    ? "graph event connection destination port is out of range"
                                    : "graph connection destination port is out of range");
                }
            }

            // Lane invariants (debug-only; no effect on release output). An
            // automation connection is never a feedback back-edge — automation
            // edges contribute to topological order, so a back-edge here would be
            // un-orderable and is rejected upstream. The automation payload is
            // only meaningful when kind == Automation.
            assert(!(is_automation_conn(spec) && spec.feedback) &&
                   "automation connections are never feedback back-edges");
            result.plan.connections.push_back({
                source_index,
                spec.source_port,
                dest_index,
                spec.dest_port,
                spec.feedback,
                spec.kind,
                spec.automation,
                spec.sidechain,
            });
            ++outbound_counts[source_index];
            ++inbound_counts[dest_index];
        }

        std::uint32_t inbound_offset = 0;
        std::uint32_t outbound_offset = 0;
        for (std::uint32_t i = 0; i < result.plan.nodes.size(); ++i) {
            auto& node = result.plan.nodes[i];
            node.first_inbound_connection = inbound_offset;
            node.inbound_connection_count = inbound_counts[i];
            inbound_offset += inbound_counts[i];

            node.first_outbound_connection = outbound_offset;
            node.outbound_connection_count = outbound_counts[i];
            outbound_offset += outbound_counts[i];
        }

        std::vector<std::uint32_t> inbound_cursor(result.plan.nodes.size(), 0);
        std::vector<std::uint32_t> outbound_cursor(result.plan.nodes.size(), 0);
        for (std::uint32_t i = 0; i < result.plan.connections.size(); ++i) {
            const auto& connection = result.plan.connections[i];
            auto& source = result.plan.nodes[connection.source_index];
            const auto outbound_index = source.first_outbound_connection +
                outbound_cursor[connection.source_index]++;
            result.plan.outbound_connection_indices[outbound_index] = i;

            auto& dest = result.plan.nodes[connection.dest_index];
            const auto inbound_index = dest.first_inbound_connection +
                inbound_cursor[connection.dest_index]++;
            result.plan.inbound_connection_indices[inbound_index] = i;
        }

        // Ordinary main-bus audio fan-in is an arithmetic reduction. Its
        // floating-point result depends on edge order, so canonicalize those
        // adjacency entries by stable endpoint identity. Keep every other
        // authored lane in place: MIDI/event, automation, feedback, and
        // sidechain edges carry ordering semantics of their own. One comparator
        // is shared by inbound and outbound slices so both views agree on the
        // same connection identity ordering.
        const auto connection_less = [&](std::uint32_t lhs, std::uint32_t rhs) {
            const auto& left = result.plan.connections[lhs];
            const auto& right = result.plan.connections[rhs];
            const auto left_key = std::tuple{
                result.plan.nodes[left.source_index].id,
                left.source_port,
                result.plan.nodes[left.dest_index].id,
                left.dest_port,
            };
            const auto right_key = std::tuple{
                result.plan.nodes[right.source_index].id,
                right.source_port,
                result.plan.nodes[right.dest_index].id,
                right.dest_port,
            };
            if (left_key != right_key) return left_key < right_key;
            return lhs < rhs;
        };
        const auto ordinary_audio = [&](std::uint32_t connection_index) {
            const auto& connection = result.plan.connections[connection_index];
            return carries_audio(connection) && !connection.feedback && !connection.sidechain;
        };
        const auto canonicalize_slice = [&](std::uint32_t first,
                                            std::uint32_t count,
                                            std::vector<std::uint32_t>& adjacency) {
            std::vector<std::uint32_t> ordinary;
            ordinary.reserve(count);
            for (std::uint32_t offset = 0; offset < count; ++offset) {
                const auto connection_index = adjacency[first + offset];
                if (ordinary_audio(connection_index)) ordinary.push_back(connection_index);
            }
            std::sort(ordinary.begin(), ordinary.end(), connection_less);
            std::size_t ordinary_index = 0;
            for (std::uint32_t offset = 0; offset < count; ++offset) {
                if (ordinary_audio(adjacency[first + offset]))
                    adjacency[first + offset] = ordinary[ordinary_index++];
            }
        };
        for (const auto& node : result.plan.nodes) {
            canonicalize_slice(node.first_inbound_connection, node.inbound_connection_count,
                               result.plan.inbound_connection_indices);
            canonicalize_slice(node.first_outbound_connection, node.outbound_connection_count,
                               result.plan.outbound_connection_indices);
        }

        std::vector<std::uint32_t> indegree(result.plan.nodes.size(), 0);
        for (const auto& connection : result.plan.connections) {
            if (!connection.feedback) ++indegree[connection.dest_index];
        }

        // Node specifications retain authored dense order because the
        // connection arrays use those indices.  Ready-node selection is a
        // separate runtime concern: use the stable NodeId as its key so
        // equivalent plans do not inherit input-vector order or hash-table
        // iteration order.  Keep the dense index as a tie-breaker for
        // defensive robustness even though duplicate NodeIds were rejected
        // above.
        using ReadyEntry = std::pair<NodeId, std::uint32_t>;
        std::priority_queue<ReadyEntry, std::vector<ReadyEntry>, std::greater<ReadyEntry>> ready;
        for (std::uint32_t i = 0; i < indegree.size(); ++i) {
            if (indegree[i] == 0)
                ready.push({result.plan.nodes[i].id, i});
        }

        while (!ready.empty()) {
            const auto node_index = ready.top().second;
            ready.pop();
            result.plan.processing_order_indices.push_back(node_index);

            const auto& node = result.plan.nodes[node_index];
            for (std::uint32_t i = 0; i < node.outbound_connection_count; ++i) {
                const auto connection_index = result.plan.outbound_connection_indices[
                    node.first_outbound_connection + i];
                const auto& connection = result.plan.connections[connection_index];
                if (connection.feedback) continue;
                auto& dest_indegree = indegree[connection.dest_index];
                if (dest_indegree == 0) continue;
                --dest_indegree;
                if (dest_indegree == 0) {
                    ready.push(
                        {result.plan.nodes[connection.dest_index].id, connection.dest_index});
                }
            }
        }

        if (result.plan.processing_order_indices.size() != result.plan.nodes.size()) {
            result.plan.clear();
            return fail(GraphRuntimePlanErrorCode::CycleDetected, 0, 0,
                        "graph contains a cycle without a feedback edge");
        }
    } catch (...) {
        return fail(GraphRuntimePlanErrorCode::AllocationFailed, 0, 0,
                    "graph runtime plan allocation failed");
    }

    return result;
}

} // namespace pulp::graph
