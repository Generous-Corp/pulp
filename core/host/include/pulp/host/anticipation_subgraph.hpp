#pragma once

#include <pulp/host/anticipation_partition.hpp>
#include <pulp/host/graph_types.hpp>

#include <span>
#include <vector>

namespace pulp::host {

struct GraphNode;
struct Connection;

// A standalone, renderable graph carved out of a larger one: the anticipation
// partition's interior, plus a SINGLE synthesized AudioOutput sink whose input
// ports are the distinct boundary output ports, so the whole thing renders
// through the ordinary executor (build_executor_snapshot + process_routed) and
// its output bus carries exactly the boundary signals the live graph needs — each
// on its own channel. The interior GraphNodes are COPIED verbatim (plugin slots,
// gain, ports preserved), so the same plugin instances render here — ownership of
// those instances across the ahead/live boundary is the renderer's concern, not
// this extraction's. NOTE: this does not check interior executor-eligibility (a
// Custom or null-slot Plugin interior node would make build_executor_snapshot
// reject the sub-graph); that gate belongs to the renderer slice.
struct AnticipationSubgraph {
    std::vector<GraphNode> nodes;         // interior nodes, then the one sink
    std::vector<Connection> connections;  // internal edges, then source->sink edges

    // One entry per captured boundary port, in OUTPUT-BUS CHANNEL ORDER: the
    // output at index i carries the original interior port (source_node,
    // source_port) on output-bus channel i (the sink's input port i). `sink_node`
    // is the same synthesized AudioOutput id for every entry. The renderer maps
    // each live boundary edge to the output that carries its source's signal.
    struct Output {
        NodeId source_node = 0;
        PortIndex source_port = 0;
        NodeId sink_node = 0;
    };
    std::vector<Output> outputs;
    bool ok = false;

    bool renders_anything() const { return ok && !outputs.empty(); }
};

// Build the renderable sub-graph for a partition: copy the interior nodes, copy
// the connections internal to the interior, and for each DISTINCT boundary output
// port (source_node, source_port) synthesize a one-input AudioOutput sink fed from
// that port (fresh ids above every existing node id, so they never collide).
// Deterministic; allocation-bounded. Returns ok=false if `partition` is not ok or
// references a node absent from `nodes` (it should not, having been built from the
// same graph). The interior carries no feedback edges (6a excludes feedback
// endpoints), so every internal edge is feedforward and every boundary edge leaves
// the interior forward — the sub-graph is acyclic and self-contained.
AnticipationSubgraph build_anticipation_subgraph(
    std::span<const GraphNode> nodes, std::span<const Connection> connections,
    const AnticipationPartition& partition);

} // namespace pulp::host
