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
        graph.set_node_gain(id, spec.gain);
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
    std::unordered_set<std::string> keys;
    for (const auto& spec : definition.nodes) {
        if (spec.key.empty() || !valid_node_type(spec.type) || spec.channels <= 0 ||
            !keys.insert(spec.key).second) {
            out.result = boundary(GraphResult::Code::UnknownNode,
                                  "template node keys must be unique built-in nodes");
            return out;
        }
        const auto id = add_node(graph, spec, out.instance.instance_identity + "." + spec.key);
        if (id == 0) {
            out.result = boundary(GraphResult::Code::UnknownNode, "unable to add template node");
            return out;
        }
        out.instance.nodes.push_back(id);
    }
    std::unordered_map<std::string, NodeId> ids;
    for (std::size_t i = 0; i < definition.nodes.size(); ++i)
        ids.emplace(definition.nodes[i].key, out.instance.nodes[i]);
    auto rollback = [&] {
        for (auto it = out.instance.nodes.rbegin(); it != out.instance.nodes.rend(); ++it)
            graph.remove_node(*it);
        out.instance.nodes.clear();
        out.instance.ports.clear();
    };
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
        out.instance.ports.emplace(port.name, std::move(exposed));
    }
    for (const auto& parameter : definition.parameters) {
        const auto node = ids.find(parameter.node);
        if (node == ids.end() || parameter.path.empty() || parameter.minimum > parameter.maximum ||
            !std::isfinite(parameter.default_value) ||
            parameter.default_value < parameter.minimum ||
            parameter.default_value > parameter.maximum) {
            rollback();
            out.result =
                boundary(GraphResult::Code::UnknownPort, "invalid exposed parameter declaration");
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
    }
    return out;
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
    for (const auto& [path, value] : state.parameters) {
        const auto it = instance.parameters.find(path);
        if (it == instance.parameters.end() || !std::isfinite(value)) {
            return boundary(GraphResult::Code::UnknownPort, "state contains an unknown parameter");
        }
        const auto node = instance.parameter_nodes.find(path);
        if (node == instance.parameter_nodes.end() || !graph.set_node_gain(node->second, value)) {
            return boundary(GraphResult::Code::UnknownPort,
                            "state parameter target is unavailable");
        }
        it->second = value;
    }
    return {GraphResult::Code::Ok};
}
} // namespace pulp::host
