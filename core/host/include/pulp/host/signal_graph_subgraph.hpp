#pragma once

#include <pulp/host/signal_graph_node.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
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

struct SubgraphTemplate;

// A child is another immutable template expanded as part of its parent. The
// child key is the stable namespace used for its exposed ports and parameter
// paths (for example, `filter.gain`). A shared pointer keeps the declaration
// value-owned while allowing recursive template types without a second graph
// runtime or a pointer into a temporary object.
struct SubgraphChildSpec {
    std::string key;
    std::shared_ptr<const SubgraphTemplate> definition;
    std::uint32_t instance_index = 0;
};

struct SubgraphTemplate {
    std::string identity;
    std::vector<SubgraphNodeSpec> nodes;
    std::vector<SubgraphEdgeSpec> edges;
    std::vector<SubgraphPortSpec> exposed_ports;
    std::vector<SubgraphParameterSpec> parameters;
    std::vector<SubgraphChildSpec> children;
};

struct SubgraphState {
    std::string template_identity;
    std::string instance_identity;
    std::uint32_t instance_index = 0;
    std::unordered_map<std::string, float> parameters;
    std::unordered_map<std::string, std::string> child_template_identities;
    std::unordered_map<std::string, std::uint32_t> child_instance_indices;
};

struct SubgraphParameterBounds {
    float minimum = 0.0f;
    float maximum = 1.0f;
};

struct SubgraphInstance {
    std::string template_identity;
    std::string instance_identity;
    std::uint32_t instance_index = 0;
    std::vector<NodeId> nodes;
    std::unordered_map<std::string, PortHandle> ports;
    std::unordered_map<std::string, float> parameters;
    std::unordered_map<std::string, NodeId> parameter_nodes;
    std::unordered_map<std::string, SubgraphParameterBounds> parameter_bounds;
    std::unordered_map<std::string, std::string> child_template_identities;
    std::unordered_map<std::string, std::uint32_t> child_instance_indices;

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
// immutable with respect to the template. Child templates are recursively
// flattened into the same ordinary SignalGraph plan; their exposed names and
// parameter paths are namespaced by the child key. Any failure (node, edge,
// port, child, or parameter declaration) rolls back every node and clears
// every instance entry created by this call, so a failed result holds no
// NodeIds. Exposed port names and parameter paths must be unique, parameter
// bounds must be finite, and each parameter must target a distinct gain node.
// Diagnostics name the subgraph boundary.
SubgraphExpansionResult instantiate_subgraph(SignalGraph& graph, const SubgraphTemplate& definition,
                                             std::string instance_identity,
                                             std::uint32_t instance_index = 0);

// Connect two exposed ports without reaching into the instance's node handles.
// The source must be an exposed output and the destination an exposed input of
// the same kind. Validation happens before graph mutation; graph-level cycle,
// duplicate, and stale-handle failures are returned with a subgraph boundary
// diagnostic.
GraphResult connect_subgraph_ports(SignalGraph& graph, const SubgraphInstance& source,
                                   std::string_view source_port,
                                   const SubgraphInstance& destination,
                                   std::string_view destination_port);

// Restore only declared parameter paths. Identity and node topology remain
// stable. Every entry is validated (known path, finite value within the
// declared [minimum, maximum], live gain target) before any is applied, so a
// rejected restore leaves the instance and graph unchanged.
GraphResult restore_subgraph_state(SignalGraph& graph, SubgraphInstance& instance,
                                   const SubgraphState& state);

} // namespace pulp::host
