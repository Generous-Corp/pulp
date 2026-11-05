#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/host/signal_graph_subgraph.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace pulp::host {
namespace {
GraphResult boundary(GraphResult::Code code, std::string rule, std::string fix = {}) {
    return {code, 0, 0, "subgraph boundary: " + std::move(rule), std::move(fix)};
}

bool valid_node_type(NodeType type) {
    return type == NodeType::AudioInput || type == NodeType::AudioOutput || type == NodeType::Gain;
}

NodeId add_node(SignalGraph& graph, const SubgraphNodeSpec& spec, const std::string& name) {
    switch (spec.type) {
    case NodeType::AudioInput:
        return graph.add_input_node(spec.channels, name);
    case NodeType::AudioOutput:
        return graph.add_output_node(spec.channels, name);
    case NodeType::Gain: {
        const auto id = graph.add_gain_node(name);
        if (id != 0 && !graph.set_node_gain(id, spec.gain)) {
            graph.remove_node(id);
            return 0;
        }
        return id;
    }
    default:
        return 0;
    }
}
} // namespace

SubgraphState SubgraphInstance::save_state() const {
    return {template_identity, instance_identity, instance_index, parameters};
}

SubgraphExpansionResult instantiate_subgraph(SignalGraph& graph, const SubgraphTemplate& definition,
                                             std::string instance_identity,
                                             std::uint32_t instance_index) {
    SubgraphExpansionResult out;
    out.result = {GraphResult::Code::Ok};
    out.instance.template_identity = definition.identity;
    out.instance.instance_identity = std::move(instance_identity);
    out.instance.instance_index = instance_index;
    if (definition.identity.empty() || out.instance.instance_identity.empty()) {
        out.result = boundary(GraphResult::Code::UnknownNode,
                              "template and instance identities must be non-empty");
        return out;
    }
    // Reject invalid parameter targets before allocating nodes or changing topology.
    for (const auto& parameter : definition.parameters) {
        const auto target =
            std::find_if(definition.nodes.begin(), definition.nodes.end(),
                         [&](const SubgraphNodeSpec& spec) { return spec.key == parameter.node; });
        if (target == definition.nodes.end() || target->type != NodeType::Gain) {
            out.result =
                boundary(GraphResult::Code::UnknownPort, "parameter target is not a gain node");
            return out;
        }
    }
    auto rollback = [&] {
        for (auto it = out.instance.nodes.rbegin(); it != out.instance.nodes.rend(); ++it)
            graph.remove_node(*it);
        out.instance.nodes.clear();
        out.instance.ports.clear();
        out.instance.parameters.clear();
        out.instance.parameter_nodes.clear();
        out.instance.parameter_bounds.clear();
    };
    std::unordered_set<std::string> keys;
    for (const auto& spec : definition.nodes) {
        if (spec.key.empty() || !valid_node_type(spec.type) || spec.channels <= 0 ||
            !std::isfinite(spec.gain) || !keys.insert(spec.key).second) {
            rollback();
            out.result = boundary(GraphResult::Code::UnknownNode,
                                  "template node keys must be unique built-in nodes");
            return out;
        }
        const auto id = add_node(graph, spec, out.instance.instance_identity + "." + spec.key);
        if (id == 0) {
            rollback();
            out.result = boundary(GraphResult::Code::UnknownNode, "unable to add template node");
            return out;
        }
        out.instance.nodes.push_back(id);
    }
    std::unordered_map<std::string, NodeId> ids;
    for (std::size_t i = 0; i < definition.nodes.size(); ++i)
        ids.emplace(definition.nodes[i].key, out.instance.nodes[i]);
    for (const auto& edge : definition.edges) {
        const auto src = ids.find(edge.source), dst = ids.find(edge.destination);
        if (src == ids.end() || dst == ids.end()) {
            rollback();
            out.result = boundary(GraphResult::Code::UnknownNode,
                                  "edge references an unknown template node");
            return out;
        }
        const auto result =
            graph.connect(src->second, edge.source_port, dst->second, edge.destination_port);
        if (!result) {
            rollback();
            out.result = boundary(GraphResult::Code::IllegalCycle, "template edge rejected",
                                  "fix the edge before expanding");
            return out;
        }
    }
    for (const auto& port : definition.exposed_ports) {
        const auto node = ids.find(port.node);
        if (node == ids.end()) {
            rollback();
            out.result =
                boundary(GraphResult::Code::UnknownPort, "exposed port references an unknown node");
            return out;
        }
        const auto* graph_node = graph.node(node->second);
        if (!graph_node) {
            rollback();
            out.result = boundary(GraphResult::Code::UnknownNode, "expanded node disappeared");
            return out;
        }
        const auto& available = port.output ? graph_node->output_ports : graph_node->input_ports;
        const auto found =
            std::find_if(available.begin(), available.end(), [&](const PortHandle& handle) {
                return handle.index == port.port && handle.kind == port.kind;
            });
        if (found == available.end()) {
            rollback();
            out.result =
                boundary(GraphResult::Code::UnknownPort, "exposed port is not present on the node");
            return out;
        }
        auto exposed = *found;
        exposed.name = port.name;
        if (!out.instance.ports.emplace(port.name, std::move(exposed)).second) {
            rollback();
            out.result = boundary(GraphResult::Code::UnknownPort, "duplicate exposed port name");
            return out;
        }
    }
    std::unordered_set<NodeId> bound_nodes;
    for (const auto& parameter : definition.parameters) {
        const auto node = ids.find(parameter.node);
        if (node == ids.end() || parameter.path.empty() || !std::isfinite(parameter.minimum) ||
            !std::isfinite(parameter.maximum) || parameter.minimum > parameter.maximum ||
            !std::isfinite(parameter.default_value) ||
            parameter.default_value < parameter.minimum ||
            parameter.default_value > parameter.maximum) {
            rollback();
            out.result =
                boundary(GraphResult::Code::UnknownPort, "invalid exposed parameter declaration");
            return out;
        }
        if (out.instance.parameters.count(parameter.path) != 0) {
            rollback();
            out.result = boundary(GraphResult::Code::UnknownPort, "duplicate parameter path");
            return out;
        }
        if (!bound_nodes.insert(node->second).second) {
            rollback();
            out.result = boundary(GraphResult::Code::UnknownPort,
                                  "node is already bound to another parameter");
            return out;
        }
        if (!graph.set_node_gain(node->second, parameter.default_value)) {
            rollback();
            out.result =
                boundary(GraphResult::Code::UnknownPort, "parameter target is not a gain node");
            return out;
        }
        out.instance.parameters.emplace(parameter.path, parameter.default_value);
        out.instance.parameter_nodes.emplace(parameter.path, node->second);
        out.instance.parameter_bounds.emplace(
            parameter.path, SubgraphParameterBounds{parameter.minimum, parameter.maximum});
    }
    return out;
}

GraphResult connect_subgraph_ports(SignalGraph& graph, const SubgraphInstance& source,
                                   std::string_view source_port,
                                   const SubgraphInstance& destination,
                                   std::string_view destination_port) {
    const auto source_it = source.ports.find(std::string(source_port));
    if (source_it == source.ports.end())
        return boundary(GraphResult::Code::UnknownPort, "source exposed port is not declared",
                        "use a declared output port");
    const auto destination_it = destination.ports.find(std::string(destination_port));
    if (destination_it == destination.ports.end())
        return boundary(GraphResult::Code::UnknownPort, "destination exposed port is not declared",
                        "use a declared input port");
    const auto& source_handle = source_it->second;
    const auto& destination_handle = destination_it->second;
    if (!source_handle.output)
        return boundary(GraphResult::Code::UnknownPort, "source exposed port is not an output",
                        "choose an output port");
    if (destination_handle.output)
        return boundary(GraphResult::Code::UnknownPort, "destination exposed port is not an input",
                        "choose an input port");
    if (source_handle.kind != destination_handle.kind)
        return boundary(GraphResult::Code::KindMismatch, "exposed port kinds do not match",
                        "connect ports with the same kind");

    // Exposed names are instance-local aliases. Resolve each handle back to
    // the graph-owned port name before calling the graph API, whose lookup is
    // intentionally based on the node's canonical port names.
    const auto resolve = [&](const PortHandle& exposed) -> PortHandle {
        const auto* node = graph.node(exposed.node);
        if (!node)
            return {};
        const auto& ports = exposed.output ? node->output_ports : node->input_ports;
        for (const auto& current : ports) {
            if (current.index != exposed.index || current.kind != exposed.kind)
                continue;
            if (exposed.owner &&
                (current.owner != exposed.owner || current.incarnation != exposed.incarnation))
                return {};
            return current;
        }
        return {};
    };
    const auto canonical_source = resolve(source_handle);
    const auto canonical_destination = resolve(destination_handle);
    if (canonical_source.node == 0 || canonical_destination.node == 0)
        return boundary(GraphResult::Code::UnknownPort,
                        "exposed port handle is stale or unavailable",
                        "obtain a live instance port");

    const auto result = graph.connect(canonical_source, canonical_destination);
    if (result)
        return result;
    return boundary(result.code, "exposed port connection rejected: " + result.rule,
                    result.suggested_fix);
}

GraphResult restore_subgraph_state(SignalGraph& graph, SubgraphInstance& instance,
                                   const SubgraphState& state) {
    if (state.template_identity != instance.template_identity ||
        state.instance_identity != instance.instance_identity ||
        state.instance_index != instance.instance_index) {
        return boundary(GraphResult::Code::UnknownNode,
                        "state identity does not match the instance");
    }
    if (state.parameters.size() != instance.parameters.size()) {
        return boundary(GraphResult::Code::UnknownPort,
                        "state parameter set does not match the template");
    }
    // Validate every entry before mutating anything so a bad entry cannot
    // leave the instance half-restored.
    for (const auto& [path, value] : state.parameters) {
        const auto known = instance.parameters.find(path);
        const auto bounds = instance.parameter_bounds.find(path);
        const auto node = instance.parameter_nodes.find(path);
        if (known == instance.parameters.end() || bounds == instance.parameter_bounds.end() ||
            node == instance.parameter_nodes.end()) {
            return boundary(GraphResult::Code::UnknownPort, "state contains an unknown parameter");
        }
        if (!std::isfinite(value) || value < bounds->second.minimum ||
            value > bounds->second.maximum) {
            return boundary(GraphResult::Code::UnknownPort,
                            "state parameter value is non-finite or out of range");
        }
        const auto* target = graph.node(node->second);
        if (!target || target->type != NodeType::Gain) {
            return boundary(GraphResult::Code::UnknownPort,
                            "state parameter target is unavailable");
        }
    }
    for (const auto& [path, value] : state.parameters) {
        graph.set_node_gain(instance.parameter_nodes.at(path), value);
        instance.parameters.at(path) = value;
    }
    return {GraphResult::Code::Ok};
}
} // namespace pulp::host
