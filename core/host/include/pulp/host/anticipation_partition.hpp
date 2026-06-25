#pragma once

#include <pulp/host/anticipation_eligibility.hpp>
#include <pulp/host/graph_types.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace pulp::host {

struct GraphNode;
struct Connection;

// An edge leaving the anticipation-eligible interior: its source is a node that
// can be rendered ahead, its destination is NOT (a live/non-eligible node, or a
// live sink such as AudioOutput/MidiOutput). These are the splice points — the
// outputs the anticipative renderer pre-computes into a ring and the live graph
// reads instead of running the interior synchronously.
struct AnticipationBoundary {
    NodeId source_node = 0;   // in the eligible interior
    PortIndex source_port = 0;
    NodeId dest_node = 0;     // outside the interior
    PortIndex dest_port = 0;
};

// The renderable eligible subgraph carved out of a graph: the interior nodes the
// renderer would run ahead, and the boundary edges whose pre-rendered output the
// live graph consumes. A partition is only worth anticipating if it both has
// interior nodes and feeds something outside the interior at non-trivial cost.
struct AnticipationPartition {
    // Indices into the input `nodes` span: every eligible node EXCEPT the live
    // sinks (AudioOutput / MidiOutput), which are consumed at the real deadline
    // and must not be written ahead. (Live SOURCES never appear — 6a already
    // excludes them.)
    std::vector<std::size_t> interior_nodes;
    std::vector<AnticipationBoundary> boundary;
    // Sum of a coarse static per-node cost weight over the interior (max(in,out)
    // ports for processing nodes, 0 for I/O) — the same proxy the parallel cost
    // gate uses. A worthiness signal, not a CPU-cycle estimate.
    std::uint64_t cost_weight = 0;
    bool ok = false;

    // Worth pre-rendering only if there is interior work that something outside
    // the interior actually consumes, at non-zero cost. An interior with no
    // boundary produces nothing the live graph reads; a zero-cost interior is not
    // worth the ring + scheduler overhead.
    bool worth_anticipating() const {
        return ok && !interior_nodes.empty() && !boundary.empty() && cost_weight > 0;
    }
};

// Carve the anticipation-eligible interior + its boundary edges out of a graph,
// given 6a's eligibility classification. Pure, deterministic, allocation-bounded;
// no prepared/live graph required. Returns ok=false if `eligibility` is not ok or
// does not match `nodes`, if `nodes` contains a duplicate id, or if a connection
// names a node absent from `nodes` (the absent-id case is normally already caught
// by 6a, which leaves `eligibility.ok == false`).
//
// This identifies WHAT a future anticipative renderer would render ahead and
// WHERE its output splices into the live graph. It performs no rendering and
// changes no real-time path. Like 6a it is a static safety/structure analysis;
// the host-time clearance still applies on top before anything is rendered ahead.
AnticipationPartition build_anticipation_partition(
    std::span<const GraphNode> nodes, std::span<const Connection> connections,
    const AnticipationEligibility& eligibility);

} // namespace pulp::host
