#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/host/subgraph_persistence.hpp>

#include "graph_load_refusal.hpp"
#include "subgraph_custom_member.hpp"
#include "subgraph_persistence_test_access.hpp"

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_set>

namespace pulp::host {
namespace {

// Armed by SubgraphPersistenceTestAccess: the countdown names the rebind call
// (1 = the next one) that throws std::bad_alloc.
std::atomic<std::size_t> g_fail_rebind_countdown{0};

// Every semantic field of a Connection, so an internal edge that changed role
// (feedback, MIDI, sidechain, modulation, spectral, Control) or modulation
// target is a different edge, not the same edge with other flags.
using EdgeKey = std::tuple<NodeId, PortIndex, NodeId, PortIndex, bool, bool, bool, bool, bool, bool,
                           bool, std::uint32_t, float, float, float, int>;

EdgeKey edge_key(const Connection& edge, NodeId source, NodeId destination) {
    const bool modulation = edge.automation || edge.audio_rate_modulation;
    return {source,
            edge.source_port,
            destination,
            edge.dest_port,
            edge.midi,
            edge.feedback,
            edge.automation,
            edge.audio_rate_modulation,
            edge.sidechain,
            edge.spectral,
            edge.control_signal,
            modulation ? edge.automation_param_id : 0u,
            modulation ? edge.automation_range_lo : 0.0f,
            modulation ? edge.automation_range_hi : 0.0f,
            modulation ? edge.automation_smoothing_ms : 0.0f,
            modulation ? static_cast<int>(edge.automation_mix) : 0};
}

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

// Internal edges of `members`, with each endpoint mapped through `rename`
// (identity when empty).
std::multiset<EdgeKey> internal_edges(const SignalGraph& graph,
                                      const std::unordered_set<NodeId>& members,
                                      const std::unordered_map<NodeId, NodeId>& rename = {}) {
    const auto mapped = [&](NodeId node) {
        const auto found = rename.find(node);
        return found == rename.end() ? node : found->second;
    };
    std::multiset<EdgeKey> edges;
    for (const auto& edge : graph.connections()) {
        if (members.contains(edge.source_node) && members.contains(edge.dest_node))
            edges.insert(edge_key(edge, mapped(edge.source_node), mapped(edge.dest_node)));
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

bool same_ports(const std::vector<PortHandle>& want, const std::vector<PortHandle>& have,
                bool match_names) {
    return std::equal(want.begin(), want.end(), have.begin(), have.end(),
                      [&](const PortHandle& a, const PortHandle& b) {
                          return a.index == b.index && a.kind == b.kind && a.output == b.output &&
                                 (!match_names || a.name == b.name);
                      });
}

// A Custom member is resolved (its exact type and version are registered with
// the port shape the node has) and its opaque state is state that type
// accepts. The writer and the restore both apply it, so a document that
// passes the writer also passes the restore.
bool custom_member_loadable(const SignalGraph& graph, NodeId id) {
    const auto* node = graph.node(id);
    if (!node || node->type != NodeType::Custom)
        return false;
    const auto* type = graph.custom_node_type(node->custom_type_id, node->custom_type_version);
    return type && type->num_input_ports == node->num_input_ports &&
           type->num_output_ports == node->num_output_ports &&
           detail::custom_member_state_accepted(*type, graph.custom_node_state(id));
}

// A restored member matches its template node structurally: type, port
// shape and kinds, for a Custom node its exact type, version and port names,
// and for a Processor the graph type it was restored as and the descriptor it
// was built from. The member's position in the record is its stable template
// member key, so its display name is not compared when `match_names` is
// false.
bool member_matches(const SignalGraph& scratch, NodeId want_id, const SignalGraph& graph,
                    NodeId have_id, bool match_names) {
    const auto* want = scratch.node(want_id);
    const auto* have = graph.node(have_id);
    if (!want || !have || want->type != have->type ||
        want->num_input_ports != have->num_input_ports ||
        want->num_output_ports != have->num_output_ports)
        return false;
    const bool custom = want->type == NodeType::Custom;
    if (!same_ports(want->input_ports, have->input_ports, custom) ||
        !same_ports(want->output_ports, have->output_ports, custom))
        return false;
    if (custom && (want->custom_type_id != have->custom_type_id ||
                   want->custom_type_version != have->custom_type_version))
        return false;
    if (match_names && want->name != have->name)
        return false;
    const auto want_processor = scratch.processor_node_instance(want_id);
    const auto have_processor = graph.processor_node_instance(have_id);
    if (static_cast<bool>(want_processor) != static_cast<bool>(have_processor))
        return false;
    if (want_processor) {
        const auto a = want_processor->descriptor();
        const auto b = have_processor->descriptor();
        if (a.bundle_id != b.bundle_id || a.version != b.version ||
            a.manufacturer != b.manufacturer ||
            want_processor->graph_type_id() != have_processor->graph_type_id() ||
            want_processor->graph_type_version() != have_processor->graph_type_version())
            return false;
    }
    return true;
}

// Rebuild one instance against a fresh expansion of its template. The scratch
// graph is the authority for structure, ports, bounds, and Processor targets;
// the record contributes only identities and declared parameter values.
// Members of a v7 document are bound by authored ID and record position, so a
// renamed member still restores; a v6 member identity is derived from its
// name, so v6 records keep the name comparison.
bool rebind_instance(SignalGraph& graph, const SubgraphTemplate& definition,
                     const choc::value::ValueView& record,
                     const std::unordered_map<std::string, NodeId>& live_by_identity,
                     bool match_names, SubgraphInstance& out, Refusal& refusal) {
    const auto instance_identity = std::string(record["instance_identity"].getString());
    const auto instance_index = static_cast<std::uint32_t>(record["instance_index"].getInt64());
    const auto mismatch = [&](const std::string& rule) {
        refusal.error = "subgraph_instance_mismatch: " + instance_identity + ": " + rule;
        return false;
    };
    if (const auto armed = g_fail_rebind_countdown.load(std::memory_order_acquire);
        armed != 0 && g_fail_rebind_countdown.fetch_sub(1, std::memory_order_acq_rel) == 1)
        throw std::bad_alloc();

    // The scratch expansion resolves Custom members against the same
    // registry the document was loaded with.
    SignalGraph scratch;
    if (!GraphSerializer::copy_custom_node_types(graph, scratch))
        return mismatch("the graph's custom node types could not be staged for the template");
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
        if (!member_matches(scratch, expected.nodes[i], graph, found->second, match_names))
            return mismatch("member " + std::to_string(i) +
                            " does not match the template node structure or nested key");
        live_of.emplace(expected.nodes[i], found->second);
        live_members.insert(found->second);
    }
    // Structure first, then state: a Custom member's opaque state must be
    // state its registered type accepts, or the instance would only fail at
    // its first prepare().
    for (std::uint32_t i = 0; i < members.size(); ++i) {
        const auto live = live_of.at(expected.nodes[i]);
        if (graph.node(live)->type == NodeType::Custom && !custom_member_loadable(graph, live)) {
            refusal.error = "subgraph_state_rejected: " + instance_identity + ": Custom member " +
                            std::to_string(i) + " state was refused by its registered type";
            return false;
        }
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
    if (internal_edges(scratch, scratch_members, live_of) != internal_edges(graph, live_members))
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
        for (const auto node : instance.nodes) {
            if (!identities.contains(node) || !claimed.insert(node).second)
                return {};
            // Restore refuses an unresolved Custom member or state its type
            // refuses, so writing one would produce a document that cannot
            // load.
            const auto* value = graph.node(node);
            if (value && value->type == NodeType::Custom && !custom_member_loadable(graph, node))
                return {};
        }
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

namespace {

// Load `json` into `graph` and rebind every record. Returns the restored
// instances ordered by identity, or nullopt with `refusal` set. Does not clean
// up `graph`; the caller decides whether it was the candidate or the live
// graph.
std::optional<std::vector<SubgraphInstance>>
load_and_rebind(SignalGraph& graph, const std::string& json, const choc::value::ValueView& records,
                const std::unordered_map<std::string, const SubgraphTemplate*>& by_identity,
                bool match_names, const GraphLoadOptions& options,
                GraphSerializer::LoadResult& load, Refusal& refusal) {
    load = GraphSerializer::from_saved_document(graph, json, options);
    if (!load.ok) {
        refusal.error = load.error;
        return std::nullopt;
    }
    // Members name the document's identities: authored IDs in v7, the
    // structural identities in v6.
    std::map<std::string, SubgraphInstance> restored;
    for (std::uint32_t i = 0; i < records.size(); ++i) {
        const auto record = records[i];
        const auto& definition =
            *by_identity.at(std::string(record["template_identity"].getString()));
        SubgraphInstance instance;
        if (!rebind_instance(graph, definition, record, load.identity_node_ids, match_names,
                             instance, refusal))
            return std::nullopt;
        restored.emplace(instance.instance_identity, std::move(instance));
    }
    std::vector<SubgraphInstance> ordered;
    ordered.reserve(restored.size());
    for (auto& [identity, instance] : restored)
        ordered.push_back(std::move(instance));
    return ordered;
}

} // namespace

SubgraphDocumentLoad from_saved_subgraph_document(SignalGraph& graph, const std::string& json,
                                                  std::span<const SubgraphTemplate> templates,
                                                  const GraphLoadOptions& options) {
    SubgraphDocumentLoad out;
    // Every refusal goes through refuse(). Until the live load starts the
    // caller's graph is untouched and stays so; from then on it is cleared,
    // and the instances and every map the load returned are emptied.
    bool live_load_started = false;
    const auto refuse = [&](std::string error) {
        detail::refuse_load(out.load, live_load_started ? &graph : nullptr, std::move(error));
        out.subgraphs.clear();
        return std::move(out);
    };
    try {
        std::unordered_map<std::string, const SubgraphTemplate*> by_identity;
        for (const auto& definition : templates) {
            if (definition.identity.empty() ||
                !by_identity.emplace(definition.identity, &definition).second)
                return refuse("subgraph_template_duplicate: template identities must be unique "
                              "and non-empty");
        }

        const auto inspection = GraphSerializer::inspect_document(json);
        if (!inspection.ok())
            return refuse(std::string(graph_document_refusal_name(inspection.refusal)) + ": " +
                          inspection.detail);

        const auto root = choc::json::parse(json);
        if (!root.hasObjectMember("subgraphs")) {
            live_load_started = true;
            out.load = GraphSerializer::from_saved_document(graph, json, options);
            return out;
        }
        const auto& records = root["subgraphs"];
        for (std::uint32_t i = 0; i < records.size(); ++i) {
            const auto identity = std::string(records[i]["template_identity"].getString());
            if (!by_identity.contains(identity))
                return refuse("subgraph_template_unknown: no template named " + identity);
        }
        const bool match_names =
            inspection.document.profile() != std::string_view("pulp.graph.saved-identity.v7");

        // The candidate: the whole document is loaded and every record
        // rebound on a scratch graph that carries the caller's custom node
        // types. Any refusal here returns before the caller's graph is
        // touched. Processor factories and restore registries therefore run
        // for the candidate and again for the live graph.
        {
            SignalGraph candidate;
            if (!GraphSerializer::copy_custom_node_types(graph, candidate))
                return refuse("subgraph_instance_mismatch: the graph's custom node types could "
                              "not be staged for a candidate restore");
            GraphSerializer::LoadResult candidate_load;
            Refusal refusal;
            if (!load_and_rebind(candidate, json, records, by_identity, match_names, options,
                                 candidate_load, refusal))
                return refuse(std::move(refusal.error));
        }

        // The live load repeats what the candidate proved. It can still fail
        // when a factory or restore callback behaves differently the second
        // time; that refusal clears the graph like any refused load.
        live_load_started = true;
        Refusal refusal;
        auto restored = load_and_rebind(graph, json, records, by_identity, match_names, options,
                                        out.load, refusal);
        if (!restored)
            return refuse(std::move(refusal.error));
        out.subgraphs = std::move(*restored);
        return out;
    } catch (const std::exception& error) {
        return refuse(std::string("subgraph_instance_mismatch: ") + error.what());
    }
}

void SubgraphPersistenceTestAccess::fail_rebind_call(std::size_t call) noexcept {
    g_fail_rebind_countdown.store(call, std::memory_order_release);
}

} // namespace pulp::host
