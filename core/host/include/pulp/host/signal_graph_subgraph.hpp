#pragma once

#include <pulp/host/signal_graph_node.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::host {

class SignalGraph;

// Declarative, immutable subgraph template. The bounded first slice supports
// built-in Input, Output, and Gain nodes; templates are expanded into the
// ordinary SignalGraph plan and do not introduce a second scheduler.
struct SubgraphNodeSpec {
    std::string key;
    NodeType type = NodeType::Gain;
    int channels = 1;
    float gain = 1.0f;
};

struct SubgraphEdgeSpec {
    std::string source;
    PortIndex source_port = 0;
    std::string destination;
    PortIndex destination_port = 0;
};

struct SubgraphPortSpec {
    std::string name;
    std::string node;
    PortIndex port = 0;
    bool output = false;
    PortKind kind = PortKind::Audio;
};

struct SubgraphParameterSpec {
    std::string path;
    std::string node;
    float default_value = 0.0f;
    float minimum = 0.0f;
    float maximum = 1.0f;
};

struct SubgraphTemplate {
    std::string identity;
    std::vector<SubgraphNodeSpec> nodes;
    std::vector<SubgraphEdgeSpec> edges;
    std::vector<SubgraphPortSpec> exposed_ports;
    std::vector<SubgraphParameterSpec> parameters;
};

struct SubgraphState {
    std::string template_identity;
    std::string instance_identity;
    std::uint32_t instance_index = 0;
    std::unordered_map<std::string, float> parameters;
};

struct SubgraphInstance {
    std::string template_identity;
    std::string instance_identity;
    std::uint32_t instance_index = 0;
    std::vector<NodeId> nodes;
    std::unordered_map<std::string, PortHandle> ports;
    std::unordered_map<std::string, float> parameters;
    std::unordered_map<std::string, NodeId> parameter_nodes;

    SubgraphState save_state() const;
};

struct SubgraphExpansionResult {
    GraphResult result;
    SubgraphInstance instance;

    explicit operator bool() const noexcept {
        return static_cast<bool>(result);
    }
};

// Expand one template instance into graph-owned nodes and edges. Expansion is
// immutable with respect to the template; a failed edge/port validation rolls
// back every node created by this call. Diagnostics name the subgraph boundary.
SubgraphExpansionResult instantiate_subgraph(SignalGraph& graph, const SubgraphTemplate& definition,
                                             std::string instance_identity,
                                             std::uint32_t instance_index = 0);

// Restore only declared parameter paths. Identity and node topology remain
// stable; out-of-range or foreign state is rejected at the subgraph boundary.
GraphResult restore_subgraph_state(SignalGraph& graph, SubgraphInstance& instance,
                                   const SubgraphState& state);

} // namespace pulp::host
