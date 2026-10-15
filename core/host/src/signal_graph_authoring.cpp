// signal_graph_authoring.cpp — control-thread topology authoring and mutation.
//
// Keep node/connection edits under GraphMutationLock and preserve the exact
// invalidate_live_locked_ ordering. No audio-thread execution or publication
// policy is owned by this translation unit.

#include <pulp/format/processor.hpp>
#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pulp::host {
namespace {
state::ModulationMixMode modulation_mix_for(AutomationMix mix) {
    switch (mix) {
    case AutomationMix::Replace:
        return state::ModulationMixMode::Replace;
    case AutomationMix::Add:
        return state::ModulationMixMode::Add;
    }
    return state::ModulationMixMode::Add;
}

bool parameter_allows_modulation(const HostParamInfo& p, uint32_t param_id,
                                 state::ParamRate required_rate, bool require_modulatable = false) {
    return p.id == param_id && p.rate == required_rate && p.flags.automatable &&
           (!require_modulatable || p.flags.modulatable) && !p.flags.read_only && !p.flags.stepped;
}

bool has_input_port(const GraphNode& node, PortIndex port) {
    return node.num_input_ports > 0 && port < static_cast<PortIndex>(node.num_input_ports);
}

bool has_output_port(const GraphNode& node, PortIndex port) {
    return node.num_output_ports > 0 && port < static_cast<PortIndex>(node.num_output_ports);
}

} // namespace

// All add_*/connect/remove mutators below take graph_mutation_mutex_ for the
// duration of the nodes_/connections_ mutation + invalidate_live_locked_(). The lock is
// safe to hold across invalidate_live_locked_() because that drives only the
// non-blocking Slot::reclaim_if_quiescent() (never the blocking reader-drain), so
// it cannot invert lock order with the Slot reader-pin. See the
// graph_mutation_mutex_ contract in signal_graph.hpp.
NodeId SignalGraph::add_input_node(int channels, const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::AudioInput;
    node.name = name;
    node.num_output_ports = channels;
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_output_node(int channels, const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::AudioOutput;
    node.name = name;
    node.num_input_ports = channels;
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_plugin_node(const PluginInfo& info) {
    GraphMutationLock mutation_lock(*this);
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Plugin;
    node.name = info.name;
    node.num_input_ports = info.num_inputs;
    node.num_output_ports = info.num_outputs;
    node.plugin = std::shared_ptr<PluginSlot>(PluginSlot::load(info));
    node.plugin_info = info; // preserve identity even if load failed
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_unresolved_plugin_node(const PluginInfo& info, int num_inputs,
                                               int num_outputs, const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Plugin;
    node.name = name;
    node.num_input_ports = num_inputs;
    node.num_output_ports = num_outputs;
    node.plugin_info = info;
    node.plugin_info.num_inputs = num_inputs;
    node.plugin_info.num_outputs = num_outputs;
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_plugin_node(std::unique_ptr<PluginSlot> slot, int num_inputs,
                                    int num_outputs, const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Plugin;
    node.name = name;
    node.num_input_ports = num_inputs;
    node.num_output_ports = num_outputs;
    node.plugin = std::shared_ptr<PluginSlot>(std::move(slot));
    if (node.plugin) {
        node.plugin_info = node.plugin->info();
    } else {
        node.plugin_info.name = name;
        node.plugin_info.num_inputs = num_inputs;
        node.plugin_info.num_outputs = num_outputs;
    }
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_processor_node(std::shared_ptr<format::ProcessorNodeInstance> instance,
                                       const std::string& name) {
    if (!instance)
        return 0;
    const auto& descriptor = instance->descriptor();
    const int inputs = descriptor.default_input_channels();
    const int outputs = descriptor.default_output_channels();
    if (inputs < 0 || outputs < 0)
        return 0;

    GraphMutationLock mutation_lock(*this);
    if (std::any_of(processor_nodes_.begin(), processor_nodes_.end(), [&](const auto& entry) {
            return entry.second && entry.second->instance == instance;
        })) {
        return 0;
    }
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Plugin;
    node.name = name.empty() ? descriptor.name : name;
    node.num_input_ports = inputs;
    node.num_output_ports = outputs;
    node.plugin_info.name = node.name;
    node.plugin_info.manufacturer = descriptor.manufacturer;
    node.plugin_info.version = descriptor.version;
    node.plugin_info.num_inputs = inputs;
    node.plugin_info.num_outputs = outputs;
    const NodeId id = node.id;
    try {
        processor_nodes_.emplace(id, std::make_shared<ProcessorNodeLifetime>(std::move(instance)));
        nodes_.push_back(std::move(node));
    } catch (...) {
        processor_nodes_.erase(id);
        return 0;
    }
    invalidate_live_locked_();
    return id;
}

NodeId SignalGraph::add_processor_node(std::unique_ptr<format::Processor> processor,
                                       const std::string& name) {
    return add_processor_node(format::ProcessorNodeInstance::create(std::move(processor)), name);
}

bool SignalGraph::is_processor_node(NodeId id) const {
    GraphMutationLock mutation_lock(*this);
    return processor_nodes_.contains(id);
}

NodeId SignalGraph::add_gain_node(const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Gain;
    node.name = name;
    node.num_input_ports = 2;
    node.num_output_ports = 2;
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_midi_input_node(const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::MidiInput;
    node.name = name;
    node.num_output_ports = 1;
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_midi_output_node(const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::MidiOutput;
    node.name = name;
    node.num_input_ports = 1;
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

// add_custom_node overloads only RESOLVE a registered type (read
// custom_node_types_) and delegate to the public add_unresolved_custom_node leaf,
// which takes graph_mutation_mutex_ once for the nodes_ mutation. They do NOT
// take the lock themselves — delegating through another locked public method
// would self-deadlock this non-recursive mutex. Type registration is expected to
// precede topology building (register_custom_node_type holds the same mutex), so
// the custom_node_types_ read here is consistent with that ordering.
NodeId SignalGraph::add_custom_node(std::string_view type_id, const std::string& name) {
    const auto* type = custom_node_type(type_id);
    if (!type)
        return 0;
    return add_custom_node(type_id, type->version, name);
}

NodeId SignalGraph::add_custom_node(std::string_view type_id, int version,
                                    const std::string& name) {
    const auto* type = custom_node_type(type_id, version);
    if (!type)
        return 0;
    return add_unresolved_custom_node(type->type_id, type->version, type->num_input_ports,
                                      type->num_output_ports,
                                      name.empty() ? type->default_name : name);
}

NodeId SignalGraph::add_unresolved_custom_node(std::string_view type_id, int version,
                                               int num_inputs, int num_outputs,
                                               const std::string& name) {
    CustomNodeType type;
    type.type_id = std::string(type_id);
    type.version = version;
    type.num_input_ports = num_inputs;
    type.num_output_ports = num_outputs;
    type.default_name = name;
    if (!type.is_valid_registration())
        return 0;

    // The single locked leaf for the add_custom_node family — serializes the
    // nodes_ push against a concurrent prepare()/mutator.
    GraphMutationLock mutation_lock(*this);
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Custom;
    node.name = name.empty() ? type.type_id : name;
    node.num_input_ports = num_inputs;
    node.num_output_ports = num_outputs;
    node.custom_type_id = std::move(type.type_id);
    node.custom_type_version = version;
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

bool SignalGraph::remove_node(NodeId id) {
    // Serialize the nodes_ erase against a concurrent compile_()/node() scan on a
    // host thread (see add_gain_node for the lock-ordering rationale).
    GraphMutationLock mutation_lock(*this);
    if (sample_region_for_node_locked_(id) != 0)
        return false;
    auto it =
        std::find_if(nodes_.begin(), nodes_.end(), [id](const GraphNode& n) { return n.id == id; });
    if (it == nodes_.end())
        return false;
    for (std::size_t i = connections_.size(); i-- > 0;) {
        const auto& c = connections_[i];
        if (c.source_node == id || c.dest_node == id) {
            erase_connection_at_locked_(i);
        }
    }
    nodes_.erase(it);
    processor_nodes_.erase(id);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect(NodeId source, PortIndex source_port, NodeId dest, PortIndex dest_port) {
    GraphMutationLock mutation_lock(*this);
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(source) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    const GraphNode* src_n = node(source);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n)
        return false;
    if (!has_output_port(*src_n, source_port))
        return false;
    if (!has_input_port(*dst_n, dest_port))
        return false;
    if (would_create_cycle(source, dest))
        return false;
    Connection conn{source, source_port, dest, dest_port};
    for (auto& c : connections_)
        if (c == conn)
            return false;
    append_connection_locked_(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect_midi(NodeId source, NodeId dest) {
    GraphMutationLock mutation_lock(*this);
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(source) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    if (!node(source) || !node(dest))
        return false;
    if (would_create_cycle(source, dest))
        return false;
    Connection conn{source, 0, dest, 0, false, true};
    for (auto& c : connections_)
        if (c == conn && c.midi == conn.midi)
            return false;
    append_connection_locked_(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect_sidechain(NodeId source, PortIndex source_port, NodeId dest,
                                    PortIndex dest_sidechain_port) {
    // Sidechain connections only make sense to Plugin nodes; everything
    // else (Gain, Custom, AudioOutput, ...) has no notion of a sidechain
    // bus. We reject other destinations early so callers fail loudly
    // instead of silently routing into a regular audio port.
    GraphMutationLock mutation_lock(*this);
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(source) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    const GraphNode* src_n = node(source);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n)
        return false;
    if (dst_n->type != NodeType::Plugin)
        return false;
    if (!has_output_port(*src_n, source_port))
        return false;
    if (!has_input_port(*dst_n, dest_sidechain_port))
        return false;
    if (would_create_cycle(source, dest))
        return false;

    Connection conn{};
    conn.source_node = source;
    conn.source_port = source_port;
    conn.dest_node = dest;
    conn.dest_port = dest_sidechain_port;
    conn.sidechain = true;
    for (auto& c : connections_)
        if (c == conn)
            return false;
    append_connection_locked_(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect_automation(NodeId src, PortIndex src_audio_port, NodeId dest,
                                     uint32_t dest_param_id, float range_lo, float range_hi,
                                     float smoothing_ms, AutomationMix mix) {
    GraphMutationLock mutation_lock(*this);
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(src) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    const GraphNode* src_n = node(src);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n)
        return false;
    if (dst_n->type != NodeType::Plugin || (!dst_n->plugin && !processor_nodes_.contains(dest)))
        return false;
    if (!has_output_port(*src_n, src_audio_port))
        return false;

    // Reject automation edges that would introduce a cycle. Automation
    // edges contribute to topological order (the source must be processed
    // before the dest), so a back-edge here would make the graph
    // un-orderable. Use the same has_path_locked_ check as connect().
    if (would_create_cycle(src, dest))
        return false;

    // Parameter must exist, be automatable, and not read-only.
    bool ok_param = false;
    for (const auto& pi : cached_or_live_params_locked_(*dst_n)) {
        if (pi.id != dest_param_id)
            continue;
        if (!pi.flags.automatable || pi.flags.read_only)
            return false;
        ok_param = true;
        break;
    }
    if (!ok_param)
        return false;

    // Second Replace edge to same (dest, param) is rejected.
    if (mix == AutomationMix::Replace) {
        for (const auto& c : connections_) {
            if (c.automation && c.dest_node == dest && c.automation_param_id == dest_param_id &&
                c.automation_mix == AutomationMix::Replace) {
                return false;
            }
        }
    }

    Connection conn{};
    conn.source_node = src;
    conn.source_port = src_audio_port;
    conn.dest_node = dest;
    conn.dest_port = 0;
    conn.automation = true;
    conn.automation_param_id = dest_param_id;
    conn.automation_range_lo = range_lo;
    conn.automation_range_hi = range_hi;
    conn.automation_smoothing_ms = std::max(0.0f, smoothing_ms);
    conn.automation_mix = mix;
    append_connection_locked_(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect_audio_rate_modulation(NodeId src, PortIndex src_audio_port, NodeId dest,
                                                uint32_t dest_param_id, float range_lo,
                                                float range_hi, float smoothing_ms,
                                                AutomationMix mix) {
    // Holds graph_mutation_mutex_ across audio_rate_modulation_lane() below, which
    // is a lock-free helper that assumes the caller holds it (it scans nodes_).
    GraphMutationLock mutation_lock(*this);
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(src) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    const GraphNode* src_n = node(src);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n)
        return false;
    if (dst_n->type != NodeType::Plugin || (!dst_n->plugin && !processor_nodes_.contains(dest)))
        return false;
    if (!has_output_port(*src_n, src_audio_port))
        return false;
    if (would_create_cycle(src, dest))
        return false;

    bool ok_param = false;
    for (const auto& pi : cached_or_live_params_locked_(*dst_n)) {
        if (pi.id != dest_param_id)
            continue;
        if (!parameter_allows_modulation(pi, dest_param_id, state::ParamRate::AudioRate, true)) {
            return false;
        }
        ok_param = true;
        break;
    }
    if (!ok_param)
        return false;

    if (mix == AutomationMix::Replace) {
        for (const auto& c : connections_) {
            if (c.audio_rate_modulation && c.dest_node == dest &&
                c.automation_param_id == dest_param_id &&
                c.automation_mix == AutomationMix::Replace) {
                return false;
            }
        }
    }

    Connection conn{};
    conn.source_node = src;
    conn.source_port = src_audio_port;
    conn.dest_node = dest;
    conn.dest_port = 0;
    conn.audio_rate_modulation = true;
    conn.automation_param_id = dest_param_id;
    conn.automation_range_lo = range_lo;
    conn.automation_range_hi = range_hi;
    conn.automation_smoothing_ms = std::max(0.0f, smoothing_ms);
    conn.automation_mix = mix;
    state::ModulationLane lane;
    // Mutex already held: call the lock-free core directly (the public
    // audio_rate_modulation_lane would re-lock and self-deadlock).
    if (!audio_rate_modulation_lane_locked_(conn, lane))
        return false;
    append_connection_locked_(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::audio_rate_modulation_lane(const Connection& connection,
                                             state::ModulationLane& lane) const {
    // Public entry: scans nodes_ via node(), so serialize against mutators.
    GraphMutationLock mutation_lock(*this);
    return audio_rate_modulation_lane_locked_(connection, lane);
}

bool SignalGraph::audio_rate_modulation_lane_locked_(const Connection& connection,
                                                     state::ModulationLane& lane) const {
    assert_graph_mutation_locked_();
    if (!connection.audio_rate_modulation || connection.automation) {
        return false;
    }

    const GraphNode* src_n = node(connection.source_node);
    const GraphNode* dst_n = node(connection.dest_node);
    if (!src_n || !dst_n || dst_n->type != NodeType::Plugin ||
        (!dst_n->plugin && !processor_nodes_.contains(connection.dest_node))) {
        return false;
    }
    if (!has_output_port(*src_n, connection.source_port)) {
        return false;
    }

    for (const auto& pi : cached_or_live_params_locked_(*dst_n)) {
        if (pi.id != connection.automation_param_id)
            continue;

        lane = state::ModulationLane{
            .source =
                {
                    .id = static_cast<state::ModulationSourceId>(connection.source_node),
                    .scope = state::ModulationScope::GraphNode,
                    .rate = state::ModulationRate::Audio,
                },
            .target =
                {
                    .param_id = pi.id,
                    .scope = state::ModulationScope::GraphNode,
                    .param_rate = pi.rate,
                    .modulatable =
                        pi.flags.modulatable && pi.flags.automatable && !pi.flags.stepped,
                    .writable = !pi.flags.read_only,
                },
            .mix = modulation_mix_for(connection.automation_mix),
            .depth = std::abs(connection.automation_range_hi - connection.automation_range_lo),
        };
        return state::validate_modulation_lane(lane).accepted;
    }

    return false;
}

bool SignalGraph::connect_feedback(NodeId source, PortIndex source_port, NodeId dest,
                                   PortIndex dest_port) {
    GraphMutationLock mutation_lock(*this);
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(source) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    const GraphNode* src_n = node(source);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n)
        return false;
    if (!has_output_port(*src_n, source_port))
        return false;
    if (!has_input_port(*dst_n, dest_port))
        return false;
    Connection conn{source, source_port, dest, dest_port, true};
    for (auto& c : connections_)
        if (c == conn)
            return false;
    append_connection_locked_(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::disconnect(NodeId source, PortIndex source_port, NodeId dest,
                             PortIndex dest_port) {
    GraphMutationLock mutation_lock(*this);
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(source) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    Connection target{source, source_port, dest, dest_port};
    auto it = std::find(connections_.begin(), connections_.end(), target);
    if (it == connections_.end())
        return false;
    erase_connection_at_locked_(static_cast<std::size_t>(std::distance(connections_.begin(), it)));
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::disconnect_modulation(NodeId source, PortIndex source_port, NodeId dest,
                                        uint32_t dest_param_id, bool audio_rate) {
    GraphMutationLock mutation_lock(*this);
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(source) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    const auto it =
        std::find_if(connections_.begin(), connections_.end(), [&](const Connection& connection) {
            return connection.source_node == source && connection.source_port == source_port &&
                   connection.dest_node == dest &&
                   connection.automation_param_id == dest_param_id &&
                   connection.automation == !audio_rate &&
                   connection.audio_rate_modulation == audio_rate;
        });
    if (it == connections_.end())
        return false;
    erase_connection_at_locked_(static_cast<std::size_t>(std::distance(connections_.begin(), it)));
    invalidate_live_locked_();
    return true;
}

// Does not lock: scans nodes_; the caller MUST hold graph_mutation_mutex_
// (every internal mutator/reader below does). Public direct callers
// (e.g. nodes()/node() external users) own their serialization per the accessor
// contract documented on graph_mutation_mutex_.
const GraphNode* SignalGraph::node(NodeId id) const {
    for (auto& n : nodes_)
        if (n.id == id)
            return &n;
    return nullptr;
}

GraphNode* SignalGraph::node_mut_locked_(NodeId id) {
    assert_graph_mutation_locked_();
    return const_cast<GraphNode*>(node(id));
}

void SignalGraph::append_connection_locked_(Connection connection) {
    assert_graph_mutation_locked_();
    const auto new_size = connections_.size() + 1;
    // Reserve both arrays before changing either size. If allocation fails the
    // graph remains unchanged instead of leaving identity metadata misaligned.
    connections_.reserve(new_size);
    connection_identities_.reserve(new_size);
    connections_.push_back(std::move(connection));
    connection_identities_.push_back(next_connection_identity_);
    ++next_connection_identity_;
    assert(connections_.size() == connection_identities_.size());
}

void SignalGraph::erase_connection_at_locked_(std::size_t index) {
    assert_graph_mutation_locked_();
    assert(index < connections_.size());
    assert(connections_.size() == connection_identities_.size());
    connections_.erase(connections_.begin() + static_cast<std::ptrdiff_t>(index));
    connection_identities_.erase(connection_identities_.begin() +
                                 static_cast<std::ptrdiff_t>(index));
}

bool SignalGraph::has_path_locked_(NodeId from, NodeId to) const {
    std::unordered_set<NodeId> visited;
    std::queue<NodeId> queue;
    queue.push(from);
    while (!queue.empty()) {
        auto current = queue.front();
        queue.pop();
        if (current == to)
            return true;
        if (visited.count(current))
            continue;
        visited.insert(current);
        for (auto& c : connections_) {
            if (c.feedback)
                continue;
            if (c.source_node == current)
                queue.push(c.dest_node);
        }
    }
    return false;
}

bool SignalGraph::would_create_cycle(NodeId source, NodeId dest) const {
    return has_path_locked_(dest, source);
}

namespace {
std::vector<NodeId> processing_order_for(const std::vector<GraphNode>& nodes,
                                         const std::vector<Connection>& connections) {
    std::unordered_map<NodeId, int> in_degree;
    std::unordered_map<NodeId, std::size_t> authoring_index;
    authoring_index.reserve(nodes.size());
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto& n = nodes[index];
        in_degree[n.id] = 0;
        authoring_index[n.id] = index;
    }
    for (const auto& c : connections) {
        if (c.feedback)
            continue;
        in_degree[c.dest_node]++;
    }
    // A graph can have multiple ready nodes at every fan-out/fan-in boundary.
    // Do not let unordered_map iteration or connection insertion order choose
    // the runtime plan. Authoring position is the semantic tie-break for
    // independent nodes; NodeId is an identity token and may outlive a node's
    // position after removals or future ID allocation changes.
    using ReadyNode = std::pair<std::size_t, NodeId>;
    std::priority_queue<ReadyNode, std::vector<ReadyNode>, std::greater<ReadyNode>> queue;
    for (const auto& [id, deg] : in_degree)
        if (deg == 0)
            queue.push({authoring_index.at(id), id});
    std::vector<NodeId> order;
    while (!queue.empty()) {
        const auto current = queue.top().second;
        queue.pop();
        order.push_back(current);
        for (const auto& c : connections) {
            if (c.feedback)
                continue;
            // Automation edges DO contribute to topological order — the
            // source must be processed before the dest so its output
            // buffer is valid when we sample it for param events.
            if (c.source_node == current) {
                if (--in_degree[c.dest_node] == 0)
                    queue.push({authoring_index.at(c.dest_node), c.dest_node});
            }
        }
    }
    return order;
}
} // namespace

std::vector<NodeId> SignalGraph::processing_order() const {
    return processing_order_for(nodes_, connections_);
}

} // namespace pulp::host
