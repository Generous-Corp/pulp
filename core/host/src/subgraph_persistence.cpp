#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/host/subgraph_persistence.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_set>

namespace pulp::host {
namespace {

using EdgeKey = std::tuple<NodeId, PortIndex, NodeId, PortIndex, bool, bool>;

template <typename V> double json_number(const V& value) {
    if (value.isInt32() || value.isInt64())
        return static_cast<double>(value.getInt64());
    return value.getFloat64();
}

PortHandle live_port(const SignalGraph& graph, NodeId node, const PortHandle& like) {
    const auto* value = graph.node(node);
    if (!value)
        return {};
    const auto& ports = like.output ? value->output_ports : value->input_ports;
    for (const auto& port : ports) {
        if (port.index == like.index && port.kind == like.kind) {
            auto bound = port;
            bound.name = like.name;
            return bound;
        }
    }
    return {};
}

std::multiset<EdgeKey> internal_edges(const SignalGraph& graph,
                                      const std::unordered_set<NodeId>& members) {
    std::multiset<EdgeKey> edges;
    for (const auto& edge : graph.connections()) {
        if (members.contains(edge.source_node) && members.contains(edge.dest_node))
            edges.emplace(edge.source_node, edge.source_port, edge.dest_node, edge.dest_port,
                          edge.midi, edge.feedback);
    }
    return edges;
}

struct Refusal {
    std::string error;
};

// The value a declared parameter has in the live graph: the target Gain
// node's gain, or the target Processor parameter. The instance's own
// `parameters` map is only a cache of the last restore and goes stale as
// soon as anything edits the node or the Processor's StateStore.
std::optional<float> live_parameter_value(const SignalGraph& graph,
                                          const SubgraphInstance& instance,
                                          const std::string& path) {
    const auto node = instance.parameter_nodes.find(path);
    if (node == instance.parameter_nodes.end())
        return std::nullopt;
    const auto* target = graph.node(node->second);
    if (!target)
        return std::nullopt;
    if (const auto id = instance.processor_parameter_ids.find(path);
        id != instance.processor_parameter_ids.end()) {
        const auto processor = graph.processor_node_instance(node->second);
        if (!processor || !processor->parameter(id->second))
            return std::nullopt;
        return processor->state_store().get_value(id->second);
    }
    if (target->type != NodeType::Gain)
        return std::nullopt;
    return target->gain;
}

// The instance's handles still name the nodes it was expanded or restored
// into: every exposed port resolves on a member node with the same owner
// and incarnation, and every parameter target is a member.
bool instance_handles_live(const SignalGraph& graph, const SubgraphInstance& instance) {
    const std::unordered_set<NodeId> members(instance.nodes.begin(), instance.nodes.end());
    for (const auto& [name, port] : instance.ports) {
        const auto* node = graph.node(port.node);
        if (!node || !members.contains(port.node))
            return false;
        const auto& ports = port.output ? node->output_ports : node->input_ports;
        const bool found = std::any_of(ports.begin(), ports.end(), [&](const PortHandle& live) {
            return live.index == port.index && live.kind == port.kind && live.owner == port.owner &&
                   live.incarnation == port.incarnation;
        });
        if (!found)
            return false;
    }
    for (const auto& [path, node] : instance.parameter_nodes)
        if (!members.contains(node))
            return false;
    return true;
}

// Rebuild one instance against a fresh expansion of its template. The scratch
// graph is the authority for structure, ports, bounds, and Processor targets;
// the record contributes only identities and declared parameter values.
bool rebind_instance(SignalGraph& graph, const SubgraphTemplate& definition,
                     const choc::value::ValueView& record,
                     const std::unordered_map<std::string, NodeId>& live_by_identity,
                     SubgraphInstance& out, Refusal& refusal) {
    const auto instance_identity = std::string(record["instance_identity"].getString());
    const auto instance_index = static_cast<std::uint32_t>(record["instance_index"].getInt64());
    const auto mismatch = [&](const std::string& rule) {
        refusal.error = "subgraph_instance_mismatch: " + instance_identity + ": " + rule;
        return false;
    };

    SignalGraph scratch;
    auto expansion = instantiate_subgraph(scratch, definition, instance_identity, instance_index);
    if (!expansion)
        return mismatch("template expansion was rejected: " + expansion.result.rule);
    const auto& expected = expansion.instance;

    const auto& members = record["members"];
    if (members.size() != expected.nodes.size())
        return mismatch("member count does not match the template expansion");

    std::unordered_map<NodeId, NodeId> live_of;
    std::unordered_set<NodeId> live_members;
    for (std::uint32_t i = 0; i < members.size(); ++i) {
        const auto found = live_by_identity.find(std::string(members[i].getString()));
        if (found == live_by_identity.end())
            return mismatch("member identity was not restored");
        const auto* want = scratch.node(expected.nodes[i]);
        const auto* have = graph.node(found->second);
        if (!want || !have || want->type != have->type || want->name != have->name)
            return mismatch("member " + std::to_string(i) +
                            " does not match the template node type or nested key");
        live_of.emplace(expected.nodes[i], found->second);
        live_members.insert(found->second);
    }

    std::unordered_map<std::string, std::uint32_t> child_indices;
    std::unordered_map<std::string, std::string> child_templates;
    const auto& children = record["children"];
    for (std::uint32_t c = 0; c < children.size(); ++c) {
        const auto child = children.getObjectMemberAt(c);
        child_templates.emplace(child.name,
                                std::string(child.value["template_identity"].getString()));
        child_indices.emplace(child.name,
                              static_cast<std::uint32_t>(child.value["instance_index"].getInt64()));
    }
    if (child_templates != expected.child_template_identities ||
        child_indices != expected.child_instance_indices)
        return mismatch("nested child identities do not match the template");

    std::unordered_set<NodeId> scratch_members(expected.nodes.begin(), expected.nodes.end());
    std::multiset<EdgeKey> wanted;
    for (const auto& [source, sport, dest, dport, midi, feedback] :
         internal_edges(scratch, scratch_members))
        wanted.emplace(live_of.at(source), sport, live_of.at(dest), dport, midi, feedback);
    if (wanted != internal_edges(graph, live_members))
        return mismatch("internal edges do not match the template");

    out = expected;
    out.nodes.clear();
    for (const auto id : expected.nodes)
        out.nodes.push_back(live_of.at(id));
    for (auto& [name, port] : out.ports) {
        port = live_port(graph, live_of.at(port.node), port);
        if (port.node == 0)
            return mismatch("exposed port " + name + " is unavailable on the restored node");
    }
    for (auto& [path, node] : out.parameter_nodes)
        node = live_of.at(node);

    SubgraphState state;
    state.template_identity = std::string(record["template_identity"].getString());
    state.instance_identity = instance_identity;
    state.instance_index = instance_index;
    state.child_template_identities = std::move(child_templates);
    state.child_instance_indices = std::move(child_indices);
    const auto& parameters = record["parameters"];
    for (std::uint32_t p = 0; p < parameters.size(); ++p) {
        const auto parameter = parameters.getObjectMemberAt(p);
        state.parameters.emplace(parameter.name, static_cast<float>(json_number(parameter.value)));
    }
    if (const auto restored = restore_subgraph_state(graph, out, state); !restored) {
        refusal.error = "subgraph_state_rejected: " + instance_identity + ": " + restored.rule;
        return false;
    }
    return true;
}

} // namespace

std::string to_saved_subgraph_document(
    const SignalGraph& graph, std::span<const SubgraphInstance> subgraphs,
    const std::unordered_map<NodeId, std::pair<float, float>>& editor_layout) {
    const auto identities = GraphSerializer::saved_node_identities(graph);
    std::map<std::string, const SubgraphInstance*> ordered;
    std::unordered_set<NodeId> claimed;
    for (const auto& instance : subgraphs) {
        if (instance.template_identity.empty() || instance.instance_identity.empty() ||
            instance.nodes.empty() ||
            !ordered.emplace(instance.instance_identity, &instance).second)
            return {};
        for (const auto node : instance.nodes)
            if (!identities.contains(node) || !claimed.insert(node).second)
                return {};
        if (!instance_handles_live(graph, instance))
            return {};
    }

    const auto document = GraphSerializer::to_saved_document(graph, editor_layout);
    if (document.empty())
        return {};
    try {
        auto root = choc::json::parse(document);
        auto records = choc::value::createEmptyArray();
        for (const auto& [identity, instance] : ordered) {
            auto record = choc::value::createObject("Subgraph");
            record.addMember("template_identity", instance->template_identity);
            record.addMember("instance_identity", instance->instance_identity);
            record.addMember("instance_index", static_cast<std::int64_t>(instance->instance_index));
            auto members = choc::value::createEmptyArray();
            for (const auto node : instance->nodes)
                members.addArrayElement(identities.at(node));
            record.addMember("members", members);
            auto parameters = choc::value::createObject("Parameters");
            for (const auto& [path, cached] : std::map<std::string, float>(
                     instance->parameters.begin(), instance->parameters.end())) {
                (void)cached;
                const auto value = live_parameter_value(graph, *instance, path);
                const auto bounds = instance->parameter_bounds.find(path);
                // Restore refuses a non-finite or out-of-bounds value, so
                // writing one would produce a document that cannot load.
                if (!value || !std::isfinite(*value) ||
                    bounds == instance->parameter_bounds.end() || *value < bounds->second.minimum ||
                    *value > bounds->second.maximum)
                    return {};
                parameters.addMember(path, static_cast<double>(*value));
            }
            record.addMember("parameters", parameters);
            auto children = choc::value::createObject("Children");
            for (const auto& [key, child_template] :
                 std::map<std::string, std::string>(instance->child_template_identities.begin(),
                                                    instance->child_template_identities.end())) {
                const auto index = instance->child_instance_indices.find(key);
                if (index == instance->child_instance_indices.end())
                    return {};
                auto child = choc::value::createObject("Child");
                child.addMember("template_identity", child_template);
                child.addMember("instance_index", static_cast<std::int64_t>(index->second));
                children.addMember(key, child);
            }
            if (instance->child_instance_indices.size() !=
                instance->child_template_identities.size())
                return {};
            record.addMember("children", children);
            records.addArrayElement(record);
        }
        root.addMember("subgraphs", records);
        return choc::json::toString(root, true);
    } catch (...) {
        return {};
    }
}

SubgraphDocumentLoad from_saved_subgraph_document(SignalGraph& graph, const std::string& json,
                                                  std::span<const SubgraphTemplate> templates,
                                                  const GraphLoadOptions& options) {
    SubgraphDocumentLoad out;
    std::unordered_map<std::string, const SubgraphTemplate*> by_identity;
    for (const auto& definition : templates) {
        if (definition.identity.empty() ||
            !by_identity.emplace(definition.identity, &definition).second) {
            out.load.error = "subgraph_template_duplicate: template identities must be unique "
                             "and non-empty";
            return out;
        }
    }

    const auto inspection = GraphSerializer::inspect_document(json);
    if (!inspection.ok()) {
        out.load.error =
            std::string(graph_document_refusal_name(inspection.refusal)) + ": " + inspection.detail;
        return out;
    }

    try {
        const auto root = choc::json::parse(json);
        const bool has_records = root.hasObjectMember("subgraphs");
        if (has_records) {
            const auto& records = root["subgraphs"];
            for (std::uint32_t i = 0; i < records.size(); ++i) {
                const auto identity = std::string(records[i]["template_identity"].getString());
                if (!by_identity.contains(identity)) {
                    out.load.error = "subgraph_template_unknown: no template named " + identity;
                    return out;
                }
            }
        }

        out.load = GraphSerializer::from_saved_document(graph, json, options);
        if (!out.load.ok || !has_records)
            return out;

        std::unordered_map<std::string, NodeId> live_by_identity;
        for (const auto& [node, identity] : GraphSerializer::saved_node_identities(graph))
            live_by_identity.emplace(identity, node);

        const auto& records = root["subgraphs"];
        std::map<std::string, SubgraphInstance> restored;
        for (std::uint32_t i = 0; i < records.size(); ++i) {
            const auto record = records[i];
            const auto& definition =
                *by_identity.at(std::string(record["template_identity"].getString()));
            SubgraphInstance instance;
            Refusal refusal;
            if (!rebind_instance(graph, definition, record, live_by_identity, instance, refusal)) {
                graph.clear();
                out.load.ok = false;
                out.load.error = std::move(refusal.error);
                return out;
            }
            restored.emplace(instance.instance_identity, std::move(instance));
        }
        for (auto& [identity, instance] : restored)
            out.subgraphs.push_back(std::move(instance));
        return out;
    } catch (const std::exception& error) {
        graph.clear();
        out.subgraphs.clear();
        out.load.ok = false;
        out.load.error = std::string("subgraph_instance_mismatch: ") + error.what();
        return out;
    }
}

} // namespace pulp::host
