#include <pulp/host/anticipation_subgraph.hpp>

#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pulp::host {

// port_key packs (NodeId, PortIndex) losslessly into 64 bits. Both are 32-bit, so
// the node occupies the high half and the port the low half with no aliasing; the
// static_assert guards the day someone widens either type (which would silently
// collapse two distinct boundary ports onto one captured output).
static_assert(sizeof(NodeId) <= 4 && sizeof(PortIndex) <= 4,
              "anticipation_subgraph::port_key assumes 32-bit NodeId/PortIndex");

AnticipationSubgraph build_anticipation_subgraph(
    std::span<const GraphNode> nodes, std::span<const Connection> connections,
    const AnticipationPartition& partition) {
    AnticipationSubgraph result;
    if (!partition.ok) return result;  // ok = false

    // Interior id set + a fresh-id seed above every existing node id.
    std::unordered_set<NodeId> interior;
    interior.reserve(partition.interior_nodes.size());
    NodeId max_id = 0;
    for (const auto& n : nodes) max_id = std::max(max_id, n.id);
    for (const auto idx : partition.interior_nodes) {
        if (idx >= nodes.size()) return AnticipationSubgraph{};  // malformed
        interior.insert(nodes[idx].id);
        result.nodes.push_back(nodes[idx]);
    }

    // Internal edges: both endpoints in the interior. (No feedback edges can be
    // internal — 6a excludes both endpoints of every feedback edge from the
    // interior — so the copied sub-graph is acyclic.)
    for (const auto& c : connections) {
        if (interior.count(c.source_node) && interior.count(c.dest_node)) {
            result.connections.push_back(c);
        }
    }

    // Distinct boundary output ports (source_node, source_port), in encounter
    // order: several live consumers of the same interior port share one captured
    // output. Each becomes one channel of a SINGLE synthesized AudioOutput sink.
    std::unordered_map<std::uint64_t, bool> seen_port;
    seen_port.reserve(partition.boundary.size());
    auto port_key = [](NodeId n, PortIndex p) -> std::uint64_t {
        return (static_cast<std::uint64_t>(n) << 32) ^ static_cast<std::uint32_t>(p);
    };
    std::vector<std::pair<NodeId, PortIndex>> ports;
    for (const auto& b : partition.boundary) {
        if (seen_port.emplace(port_key(b.source_node, b.source_port), true).second) {
            ports.push_back({b.source_node, b.source_port});
        }
    }
    if (ports.empty()) {
        result.ok = true;
        return result;  // interior with no boundary: nothing to capture
    }

    // ONE AudioOutput sink with one input port per captured boundary port. The
    // executor maps an AudioOutput node's input port p to output-bus channel p
    // (accumulating), so wiring boundary port i into the sink's dest_port i lands
    // each captured signal on its OWN channel — distinct, never summed together.
    // `outputs[i]` therefore corresponds to output-bus channel i.
    if (max_id == std::numeric_limits<NodeId>::max()) {
        return AnticipationSubgraph{};  // no room for a fresh id without wrapping
    }
    const NodeId sink_id = max_id + 1;
    GraphNode sink;
    sink.id = sink_id;
    sink.type = NodeType::AudioOutput;
    sink.num_input_ports = static_cast<int>(ports.size());
    sink.num_output_ports = 0;
    result.nodes.push_back(std::move(sink));

    for (std::size_t i = 0; i < ports.size(); ++i) {
        Connection cap;
        cap.source_node = ports[i].first;
        cap.source_port = ports[i].second;
        cap.dest_node = sink_id;
        cap.dest_port = static_cast<PortIndex>(i);
        result.connections.push_back(cap);
        result.outputs.push_back({ports[i].first, ports[i].second, sink_id});
    }

    result.ok = true;
    return result;
}

} // namespace pulp::host
