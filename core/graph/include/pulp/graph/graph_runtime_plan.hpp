#pragma once

#include <pulp/graph/graph_types.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pulp::graph {

enum class GraphRuntimeNodeKind : std::uint8_t {
    AudioInput,
    AudioOutput,
    Processor,
    Utility,
    MidiInput,
    MidiOutput,
    Custom,
};

struct GraphRuntimeLimits {
    std::uint32_t max_nodes = 512;
    std::uint32_t max_connections = 2048;
    std::uint32_t max_ports_per_node = 64;
    std::uint32_t max_total_ports = 4096;
};

struct GraphRuntimeNodeSpec {
    NodeId id = 0;
    GraphRuntimeNodeKind kind = GraphRuntimeNodeKind::Processor;
    std::uint32_t input_ports = 0;
    std::uint32_t output_ports = 0;
    std::uint32_t event_input_ports = 0;
    std::uint32_t event_output_ports = 0;
    // When true, the buffer assignment gives this node's output region dedicated
    // scratch slots that are never recycled by another node, so their contents
    // persist across blocks. Required for parity with a per-node persistent
    // output buffer when the node may NOT fully overwrite its outputs each block
    // (e.g. a hosted plugin that leaves a channel or trailing samples untouched);
    // slot reuse would otherwise hand it a different node's stale data.
    // Meaningful only for nodes with output_ports > 0; a no-op otherwise.
    bool persistent_output = false;
    // Latency this node ADDS to its own signal path, in samples (e.g. a hosted
    // plugin's reported processing latency). The buffer assignment propagates it
    // through the topology to derive per-connection delay compensation so that
    // fan-in paths of differing latency time-align. Zero for nodes that add no
    // latency (audio I/O, gain).
    std::uint32_t latency_samples = 0;
};

// Parameter automation carried on a connection: the source audio output drives
// the destination plugin's parameter `param_id` instead of an audio/event port.
// `audio_rate` selects dense per-sample modulation; otherwise sparse two-point
// (sample 0 + sample N-1). All values the realtime automation gather needs —
// the source→param range map, the per-source slew time, the mix mode, and the
// resolved parameter bounds — are carried here so the gather never calls into
// the plugin on the audio thread. Meaningful only when `GraphRuntimeConnection*
// ::automation` is true.
struct GraphRuntimeAutomationSpec {
    std::uint32_t param_id = 0;
    float range_lo = 0.0f;        // plain-parameter domain the source maps into
    float range_hi = 1.0f;
    float smoothing_ms = 0.0f;    // per-source linear slew time (sparse only)
    bool mix_add = false;         // false = Replace, true = Add (then clamp)
    bool audio_rate = false;      // true = dense per-sample, false = sparse 2-point
    float bounds_lo = 0.0f;       // resolved parameter bounds (Add-mix clamp)
    float bounds_hi = 1.0f;
};

struct GraphRuntimeConnectionSpec {
    NodeId source_node = 0;
    PortIndex source_port = 0;
    NodeId dest_node = 0;
    PortIndex dest_port = 0;
    bool feedback = false;
    bool event = false;
    // True if this connection drives a parameter rather than an audio/event
    // port; `automation` then carries the mapping. Automation connections still
    // order the graph (source before dest) but carry no audio/MIDI payload.
    bool is_automation = false;
    GraphRuntimeAutomationSpec automation;
};

struct GraphRuntimeNodePlan {
    NodeId id = 0;
    GraphRuntimeNodeKind kind = GraphRuntimeNodeKind::Processor;
    std::uint32_t input_ports = 0;
    std::uint32_t output_ports = 0;
    std::uint32_t event_input_ports = 0;
    std::uint32_t event_output_ports = 0;
    // Carried from GraphRuntimeNodeSpec::persistent_output; see that field.
    bool persistent_output = false;
    // Carried from GraphRuntimeNodeSpec::latency_samples; see that field.
    std::uint32_t latency_samples = 0;
    std::uint32_t first_inbound_connection = 0;
    std::uint32_t inbound_connection_count = 0;
    std::uint32_t first_outbound_connection = 0;
    std::uint32_t outbound_connection_count = 0;
};

struct GraphRuntimeConnectionPlan {
    std::uint32_t source_index = 0;
    PortIndex source_port = 0;
    std::uint32_t dest_index = 0;
    PortIndex dest_port = 0;
    bool feedback = false;
    bool event = false;
    // Carried from GraphRuntimeConnectionSpec; see those fields.
    bool is_automation = false;
    GraphRuntimeAutomationSpec automation;
};

struct GraphRuntimePlan {
    std::vector<GraphRuntimeNodePlan> nodes;
    std::vector<GraphRuntimeConnectionPlan> connections;
    std::vector<std::uint32_t> inbound_connection_indices;
    std::vector<std::uint32_t> outbound_connection_indices;
    // Dense node indices into `nodes`, not NodeIds.
    std::vector<std::uint32_t> processing_order_indices;

    void clear();
    std::uint32_t node_count() const noexcept;
    std::uint32_t connection_count() const noexcept;
};

enum class GraphRuntimePlanErrorCode : std::uint8_t {
    None,
    InvalidLimits,
    TooManyNodes,
    TooManyConnections,
    TooManyPorts,
    InvalidNodeId,
    DuplicateNodeId,
    InvalidNodePortCount,
    UnknownSourceNode,
    UnknownDestinationNode,
    SourcePortOutOfRange,
    DestinationPortOutOfRange,
    CycleDetected,
    AllocationFailed,
};

struct GraphRuntimePlanError {
    GraphRuntimePlanErrorCode code = GraphRuntimePlanErrorCode::None;
    std::uint32_t index = 0;
    NodeId node_id = 0;
    std::string message;
};

struct GraphRuntimePlanResult {
    GraphRuntimePlan plan;
    GraphRuntimePlanError error;

    bool ok() const noexcept {
        return error.code == GraphRuntimePlanErrorCode::None;
    }
};

GraphRuntimePlanResult build_graph_runtime_plan(
    std::span<const GraphRuntimeNodeSpec> nodes,
    std::span<const GraphRuntimeConnectionSpec> connections,
    GraphRuntimeLimits limits = {});

} // namespace pulp::graph
