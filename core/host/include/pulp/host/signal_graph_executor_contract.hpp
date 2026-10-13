#pragma once

// Small, behavior-only contracts shared by SignalGraph's routed executor,
// legacy reference walk, and host-facing admission code.  The full
// signal_graph_executor_routing.hpp header remains the compatibility surface
// for callers that need binding storage or snapshot construction.

#include <pulp/graph/graph_runtime_plan.hpp>
#include <pulp/host/graph_types.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace pulp::host {

struct Connection;
struct GraphNode;

// The single, authoritative classification of a host Connection into a runtime
// lane. MIDI takes precedence over automation, and automation takes precedence
// over plain audio. Feedback is orthogonal; sidechain and dense audio-rate
// modulation remain explicit markers on the result.
struct ConnectionClass {
    graph::GraphRuntimeConnectionKind kind = graph::GraphRuntimeConnectionKind::Audio;
    bool feedback = false;
    bool audio_rate = false;
    bool sidechain = false;
};

ConnectionClass classify(const Connection& c);

// True iff this connection carries latency-aligned audio for PDC: feed-forward
// audio or dense audio-rate modulation. Feedback, MIDI, and sparse automation
// do not participate in the latency domain.
bool connection_affects_latency(const Connection& c);

enum class ExecutorTopologyValidationCode : std::uint8_t {
    Accepted,
    TopologyIneligible,
    NodeLimitExceeded,
    ConnectionLimitExceeded,
    PerNodePortLimitExceeded,
    TotalPortLimitExceeded,
    PlanRejected,
};

// Admission result for the exact routed-executor plan shape derived from a
// SignalGraph candidate. Capacity failures report actual/limit; plan failures
// additionally identify the rejected spec index and node.
struct ExecutorTopologyValidation {
    ExecutorTopologyValidationCode code = ExecutorTopologyValidationCode::Accepted;
    std::uint64_t actual = 0;
    std::uint64_t limit = 0;
    std::size_t index = 0;
    NodeId node = 0;

    constexpr explicit operator bool() const noexcept {
        return code == ExecutorTopologyValidationCode::Accepted;
    }
};

// Validates the same node/connection projection consumed by
// build_executor_snapshot(), without preparing or publishing a graph.
ExecutorTopologyValidation
validate_signal_graph_executor_topology(std::span<const GraphNode> nodes,
                                        std::span<const Connection> connections,
                                        graph::GraphRuntimeLimits limits = {});

} // namespace pulp::host
