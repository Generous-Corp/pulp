#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/host/signal_graph_subgraph.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>
#include <utility>

namespace pulp::host {
namespace {

constexpr std::size_t kMaxNestedDepth = 8;

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

bool validate_template(const SubgraphTemplate& definition, std::size_t depth,
                       std::unordered_set<const SubgraphTemplate*>& active) {
    if (depth > kMaxNestedDepth || definition.identity.empty() ||
        !active.insert(&definition).second)
        return false;

    std::unordered_set<std::string> node_keys;
    for (const auto& node : definition.nodes) {
        if (node.key.empty() || !valid_node_type(node.type) || node.channels <= 0 ||
            !std::isfinite(node.gain) || !node_keys.insert(node.key).second) {
            active.erase(&definition);
            return false;
        }
    }

    std::unordered_set<std::string> parameter_paths;
    std::unordered_set<std::string> parameter_nodes;
    for (const auto& parameter : definition.parameters) {
        const auto target =
            std::find_if(definition.nodes.begin(), definition.nodes.end(),
                         [&](const SubgraphNodeSpec& spec) { return spec.key == parameter.node; });
        if (target == definition.nodes.end() || target->type != NodeType::Gain ||
            parameter.path.empty() || !std::isfinite(parameter.minimum) ||
            !std::isfinite(parameter.maximum) || parameter.minimum > parameter.maximum ||
            !std::isfinite(parameter.default_value) ||
            parameter.default_value < parameter.minimum ||
            parameter.default_value > parameter.maximum ||
            !parameter_paths.insert(parameter.path).second ||
            !parameter_nodes.insert(parameter.node).second) {
            active.erase(&definition);
            return false;
        }
    }

    std::unordered_set<std::string> child_keys;
    for (const auto& child : definition.children) {
        if (child.key.empty() || !child.definition || !child_keys.insert(child.key).second ||
            !validate_template(*child.definition, depth + 1, active)) {
            active.erase(&definition);
            return false;
        }
    }
    active.erase(&definition);
    return true;
}

struct ExpansionContext {
    SignalGraph& graph;
    SubgraphInstance& instance;
    std::unordered_map<std::string, NodeId> nodes;
    // All child exposed ports are retained by their stable namespaced key so a
    // parent can re-expose them or connect them to one of its own built-ins.
    std::unordered_map<std::string, PortHandle> exposed;
};

std::string scoped(const std::string& prefix, std::string_view key) {
    return prefix.empty() ? std::string(key) : prefix + "." + std::string(key);
}

PortHandle node_port(SignalGraph& graph, NodeId node, bool output, PortIndex index) {
    const auto* value = graph.node(node);
    if (!value)
        return {};
    const auto& ports = output ? value->output_ports : value->input_ports;
    const auto found = std::find_if(ports.begin(), ports.end(),
                                    [&](const PortHandle& port) { return port.index == index; });
    return found == ports.end() ? PortHandle{} : *found;
}

PortHandle declared_port(ExpansionContext& context, const std::string& prefix,
                         const SubgraphPortSpec& spec) {
    const auto node_key = scoped(prefix, spec.node);
    if (const auto node = context.nodes.find(node_key); node != context.nodes.end())
        return node_port(context.graph, node->second, spec.output, spec.port);

    const auto exposed = context.exposed.find(node_key);
    if (exposed == context.exposed.end())
        return {};
    if (exposed->second.output != spec.output || exposed->second.index != spec.port ||
        exposed->second.kind != spec.kind)
        return {};
    return exposed->second;
}

PortHandle edge_port(ExpansionContext& context, const std::string& prefix, std::string_view key,
                     PortIndex index, bool output) {
    const auto node_key = scoped(prefix, key);
    if (const auto node = context.nodes.find(node_key); node != context.nodes.end())
        return node_port(context.graph, node->second, output, index);
    const auto exposed = context.exposed.find(node_key);
    if (exposed == context.exposed.end() || exposed->second.output != output ||
        exposed->second.index != index)
        return {};
    return exposed->second;
}

bool expand_template(ExpansionContext& context, const SubgraphTemplate& definition,
                     const std::string& prefix, std::size_t depth) {
    for (const auto& spec : definition.nodes) {
        const auto key = scoped(prefix, spec.key);
        const auto id =
            add_node(context.graph, spec, context.instance.instance_identity + "." + key);
        if (id == 0)
            return false;
        context.instance.nodes.push_back(id);
        context.nodes.emplace(key, id);
    }

    for (const auto& child : definition.children) {
        const auto child_prefix = scoped(prefix, child.key);
        if (!expand_template(context, *child.definition, child_prefix, depth + 1))
            return false;
        context.instance.child_template_identities.emplace(child_prefix,
                                                           child.definition->identity);
        context.instance.child_instance_indices.emplace(child_prefix, child.instance_index);
    }

    // Child ports become internal endpoints under `child.key.port`. A parent
    // can promote one of these endpoints via its own exposed_ports declaration.
    for (const auto& port : definition.exposed_ports) {
        const auto handle = declared_port(context, prefix, port);
        if (handle.node == 0 || handle.output != port.output || handle.kind != port.kind)
            return false;
        auto exposed = handle;
        exposed.name = port.name;
        const auto key = scoped(prefix, port.name);
        if (!context.exposed.emplace(key, exposed).second)
            return false;
        if (prefix.empty() && !context.instance.ports.emplace(port.name, exposed).second)
            return false;
    }

    // Edges may target local node keys (the original API) or a namespaced
    // child endpoint such as `filter.output`.
    for (const auto& edge : definition.edges) {
        const auto source = edge_port(context, prefix, edge.source, edge.source_port, true);
        const auto destination =
            edge_port(context, prefix, edge.destination, edge.destination_port, false);
        if (source.node == 0 || destination.node == 0 ||
            !context.graph.connect(source, destination))
            return false;
    }
    return depth <= kMaxNestedDepth;
}

void clear_instance(SubgraphInstance& instance) {
    instance.nodes.clear();
    instance.ports.clear();
    instance.parameters.clear();
    instance.parameter_nodes.clear();
    instance.parameter_bounds.clear();
    instance.child_template_identities.clear();
    instance.child_instance_indices.clear();
}

bool bind_parameters(ExpansionContext& context, const SubgraphTemplate& definition,
                     const std::string& prefix) {
    for (const auto& parameter : definition.parameters) {
        const auto node = context.nodes.find(scoped(prefix, parameter.node));
        const auto path = scoped(prefix, parameter.path);
        if (node == context.nodes.end() || context.instance.parameters.count(path) != 0 ||
            !context.instance.parameter_nodes.emplace(path, node->second).second ||
            !context.instance.parameter_bounds
                 .emplace(path, SubgraphParameterBounds{parameter.minimum, parameter.maximum})
                 .second ||
            !context.instance.parameters.emplace(path, parameter.default_value).second ||
            !context.graph.set_node_gain(node->second, parameter.default_value))
            return false;
    }
    for (const auto& child : definition.children) {
        if (!bind_parameters(context, *child.definition, scoped(prefix, child.key)))
            return false;
    }
    return true;
}

} // namespace

SubgraphState SubgraphInstance::save_state() const {
    return {template_identity, instance_identity,         instance_index,
            parameters,        child_template_identities, child_instance_indices};
}

SubgraphExpansionResult instantiate_subgraph(SignalGraph& graph, const SubgraphTemplate& definition,
                                             std::string instance_identity,
                                             std::uint32_t instance_index) {
    SubgraphExpansionResult out;
    out.result = {GraphResult::Code::Ok};
    out.instance.template_identity = definition.identity;
    out.instance.instance_identity = std::move(instance_identity);
    out.instance.instance_index = instance_index;
    if (out.instance.template_identity.empty() || out.instance.instance_identity.empty()) {
        out.result = boundary(GraphResult::Code::UnknownNode,
                              "template and instance identities must be non-empty");
        return out;
    }
    // Preserve the original first-slice diagnostic and, more importantly,
    // reject this failure before allocating a node or consuming a graph ID.
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
    std::unordered_set<const SubgraphTemplate*> active;
    if (!validate_template(definition, 0, active)) {
        out.result = boundary(GraphResult::Code::UnknownNode,
                              "template contains an invalid or cyclic child declaration");
        return out;
    }

    ExpansionContext context{graph, out.instance};
    auto rollback = [&] {
        for (auto it = out.instance.nodes.rbegin(); it != out.instance.nodes.rend(); ++it)
            graph.remove_node(*it);
        clear_instance(out.instance);
    };
    if (!expand_template(context, definition, {}, 0) || !bind_parameters(context, definition, {})) {
        rollback();
        out.result = boundary(GraphResult::Code::UnknownPort,
                              "nested template expansion or binding was rejected",
                              "fix the child declaration or exposed endpoint");
        return out;
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
        state.instance_index != instance.instance_index ||
        state.child_template_identities != instance.child_template_identities ||
        state.child_instance_indices != instance.child_instance_indices) {
        return boundary(GraphResult::Code::UnknownNode,
                        "state identity does not match the instance");
    }
    if (state.parameters.size() != instance.parameters.size())
        return boundary(GraphResult::Code::UnknownPort,
                        "state parameter set does not match the template");
    for (const auto& [path, value] : state.parameters) {
        const auto known = instance.parameters.find(path);
        const auto bounds = instance.parameter_bounds.find(path);
        const auto node = instance.parameter_nodes.find(path);
        if (known == instance.parameters.end() || bounds == instance.parameter_bounds.end() ||
            node == instance.parameter_nodes.end())
            return boundary(GraphResult::Code::UnknownPort, "state contains an unknown parameter");
        if (!std::isfinite(value) || value < bounds->second.minimum ||
            value > bounds->second.maximum)
            return boundary(GraphResult::Code::UnknownPort,
                            "state parameter value is non-finite or out of range");
        const auto* target = graph.node(node->second);
        if (!target || target->type != NodeType::Gain)
            return boundary(GraphResult::Code::UnknownPort,
                            "state parameter target is unavailable");
    }
    for (const auto& [path, value] : state.parameters) {
        graph.set_node_gain(instance.parameter_nodes.at(path), value);
        instance.parameters.at(path) = value;
    }
    return {GraphResult::Code::Ok};
}

} // namespace pulp::host
