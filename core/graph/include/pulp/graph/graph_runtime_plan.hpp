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
};

struct GraphRuntimeConnectionSpec {
    NodeId source_node = 0;
    PortIndex source_port = 0;
    NodeId dest_node = 0;
    PortIndex dest_port = 0;
    bool feedback = false;
    bool event = false;
};

struct GraphRuntimeNodePlan {
    NodeId id = 0;
    GraphRuntimeNodeKind kind = GraphRuntimeNodeKind::Processor;
    std::uint32_t input_ports = 0;
    std::uint32_t output_ports = 0;
    std::uint32_t event_input_ports = 0;
    std::uint32_t event_output_ports = 0;
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

