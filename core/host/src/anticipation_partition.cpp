#include <pulp/host/anticipation_partition.hpp>

#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <unordered_map>

namespace pulp::host {
namespace {

// Coarse static per-node cost weight, mirroring the parallel executor's gate:
// the per-channel work a processing node does, 0 for pure I/O / sinks.
std::uint64_t node_cost_weight(const GraphNode& n) {
    switch (n.type) {
        case NodeType::Plugin:
        case NodeType::Gain:
        case NodeType::Custom:
            return static_cast<std::uint64_t>(
                std::max(n.num_input_ports, n.num_output_ports));
        case NodeType::AudioInput:
        case NodeType::AudioOutput:
        case NodeType::MidiInput:
        case NodeType::MidiOutput:
            return 0;
    }
    return 0;
}

// A live sink: eligible but consumed at the real deadline, so never rendered
// ahead even though nothing downstream excludes it. LANDMINE: a node defaults to
// interior (anticipatable) work, so any NEW NodeType that represents a live
// endpoint MUST be added here (and a new live SOURCE must be seeded in 6a) — the
// unsafe direction for a safety classifier is to silently treat it as interior.
bool is_live_sink(NodeType t) {
    return t == NodeType::AudioOutput || t == NodeType::MidiOutput;
}

} // namespace

AnticipationPartition build_anticipation_partition(
    std::span<const GraphNode> nodes, std::span<const Connection> connections,
    const AnticipationEligibility& eligibility) {
    AnticipationPartition result;
    if (!eligibility.ok || eligibility.node_exclusion.size() != nodes.size()) {
        return result;  // ok = false
    }

    // The renderable interior: eligible, minus the live sinks.
    std::unordered_map<NodeId, std::size_t> index_of;
    index_of.reserve(nodes.size());
    std::vector<bool> in_interior(nodes.size(), false);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        // Duplicate ids would silently collapse a later node onto an earlier one's
        // classification when the boundary loop resolves connection endpoints —
        // fabricating or missing a splice point. Reject as malformed; a safety
        // classifier must be correct by construction, not by an unstated unique-id
        // assumption on a raw span.
        if (!index_of.emplace(nodes[i].id, i).second) {
            return AnticipationPartition{};  // ok = false
        }
        if (eligibility.passes_static_exclusions(i) && !is_live_sink(nodes[i].type)) {
            in_interior[i] = true;
            result.interior_nodes.push_back(i);
            result.cost_weight += node_cost_weight(nodes[i]);
        }
    }

    // Boundary: every edge whose source is interior and whose destination is not.
    // (6a excludes all feedback endpoints, so no interior node is a feedback
    // endpoint — every edge leaving the interior is a feedforward splice point.)
    for (const auto& c : connections) {
        const auto si = index_of.find(c.source_node);
        const auto di = index_of.find(c.dest_node);
        if (si == index_of.end() || di == index_of.end()) {
            return AnticipationPartition{};  // malformed: dangling node id
        }
        if (in_interior[si->second] && !in_interior[di->second]) {
            result.boundary.push_back(
                {c.source_node, c.source_port, c.dest_node, c.dest_port});
        }
    }

    result.ok = true;
    return result;
}

} // namespace pulp::host
