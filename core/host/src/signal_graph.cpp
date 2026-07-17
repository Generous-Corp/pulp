// Signal Graph implementation.
//
// Audio-thread reads happen against a CompiledGraph snapshot published via an
// atomic raw pointer. UI-thread topology mutations (add_*, connect*,
// disconnect, remove_node) invalidate the snapshot; prepare() rebuilds and
// publishes a fresh snapshot. Control-thread shared_ptr owners keep retired
// snapshots alive until active process readers drain, avoiding libstdc++'s
// lock-taking atomic shared_ptr path in process().
// See signal_graph.hpp for the mutation protocol details.

#include <pulp/host/signal_graph.hpp>
#include <pulp/host/anticipation_eligibility.hpp>
#include <pulp/host/anticipation_partition.hpp>
#include <pulp/host/anticipation_subgraph.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/runtime/log.hpp>
#include "signal_graph_internal.hpp"
#include <algorithm>
#include <array>
#include <queue>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <unordered_map>
#include <cstring>
#include <cassert>
#include <thread>
#include <utility>

namespace pulp::host {
namespace {

bool parameter_allows_modulation(const HostParamInfo& p,
                                 uint32_t param_id,
                                 state::ParamRate required_rate,
                                 bool require_modulatable = false) {
    return p.id == param_id
        && p.rate == required_rate
        && p.flags.automatable
        && (!require_modulatable || p.flags.modulatable)
        && !p.flags.read_only
        && !p.flags.stepped;
}

bool has_input_port(const GraphNode& node, PortIndex port) {
    return node.num_input_ports > 0
        && port < static_cast<PortIndex>(node.num_input_ports);
}

bool has_output_port(const GraphNode& node, PortIndex port) {
    return node.num_output_ports > 0
        && port < static_cast<PortIndex>(node.num_output_ports);
}

std::size_t saturating_add(std::size_t a, std::size_t b) {
    const auto max = std::numeric_limits<std::size_t>::max();
    return b > max - a ? max : a + b;
}

std::size_t saturating_mul(std::size_t a, std::size_t b) {
    const auto max = std::numeric_limits<std::size_t>::max();
    if (a == 0 || b == 0) return 0;
    return a > max / b ? max : a * b;
}

std::uint64_t saturating_add_u64(std::uint64_t a, std::uint64_t b) {
    const auto max = std::numeric_limits<std::uint64_t>::max();
    return b > max - a ? max : a + b;
}

std::uint64_t saturating_mul_u64(std::uint64_t a, std::uint64_t b) {
    const auto max = std::numeric_limits<std::uint64_t>::max();
    if (a == 0 || b == 0) return 0;
    return a > max / b ? max : a * b;
}

state::ModulationMixMode modulation_mix_for(AutomationMix mix) {
    switch (mix) {
        case AutomationMix::Replace: return state::ModulationMixMode::Replace;
        case AutomationMix::Add: return state::ModulationMixMode::Add;
    }
    return state::ModulationMixMode::Add;
}

template <typename T, typename GetId>
T* find_by_id(std::vector<T>& entries, uint32_t id, GetId get_id) {
    auto it = std::find_if(entries.begin(), entries.end(),
                           [&](const T& entry) { return get_id(entry) == id; });
    return it == entries.end() ? nullptr : &*it;
}

bool is_valid_custom_node_type(const CustomNodeType& type) {
    return !type.type_id.empty()
        && type.version > 0
        && type.num_input_ports >= 0
        && type.num_output_ports >= 0;
}

bool custom_type_matches_node_shape(const CustomNodeType& type,
                                    const GraphNode& node) {
    return type.num_input_ports == node.num_input_ports
        && type.num_output_ports == node.num_output_ports;
}

std::string custom_node_key(std::string_view type_id, int version) {
    std::string key(type_id);
    key.push_back('\x1f');
    key += std::to_string(version);
    return key;
}

// Make a per-node opaque instance owned via shared_ptr, with the type's destroy
// callback as the deleter (RAII). Returns nullptr for stateless types (no
// `create`).
std::shared_ptr<void> make_custom_instance(const CustomNodeType& type) {
    if (!type.create) return nullptr;
    void* raw = type.create();
    if (raw == nullptr) return nullptr;
    auto destroy = type.destroy;
    return std::shared_ptr<void>(raw, [destroy](void* p) {
        if (destroy && p) destroy(p);
    });
}

} // namespace

SignalGraph::MidiBlockSnapshot::MidiBlockSnapshot() {
    prepare_midi_block_storage(events, ump);
}

SignalGraph::MidiBlockSnapshot::MidiBlockSnapshot(
    const MidiBlockSnapshot& other)
    : MidiBlockSnapshot() {
    *this = other;
}

SignalGraph::MidiBlockSnapshot&
SignalGraph::MidiBlockSnapshot::operator=(const MidiBlockSnapshot& other) {
    if (this == &other) return *this;
    set_from_midi(other.events, other.sequence, other.incomplete);
    return *this;
}

bool SignalGraph::MidiBlockSnapshot::set_from_midi(
    const midi::MidiBuffer& src,
    uint64_t new_sequence,
    bool source_incomplete) {
    clear_midi_block(events);
    sequence = new_sequence;
    const bool copied_all = copy_midi_block(src, events);
    incomplete = source_incomplete || !copied_all;
    return !incomplete;
}

bool SignalGraph::MidiBlockSnapshot::copy_to_midi(
    midi::MidiBuffer& dst) const {
    const bool copied_all = copy_midi_block(events, dst);
    return copied_all && !incomplete;
}

bool SignalGraph::ParameterBlockSnapshot::set_from_queue(
    const state::ParameterEventQueue& src,
    std::uint64_t new_sequence) noexcept {
    size = 0;
    sequence = new_sequence;
    source_incomplete = src.overflowed();
    for (const auto& event : src) {
        if (size == events.size()) {
            source_incomplete = true;
            break;
        }
        events[size++] = event;
    }
    return !source_incomplete;
}

bool SignalGraph::ParameterBlockSnapshot::append_to(
    state::ParameterEventQueue& dst) const noexcept {
    bool copied_all = !source_incomplete;
    for (std::size_t i = 0; i < size; ++i) {
        copied_all = dst.push(events[i]) && copied_all;
    }
    return copied_all;
}

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
    node.plugin_info = info;  // preserve identity even if load failed
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_unresolved_plugin_node(const PluginInfo& info,
                                               int num_inputs,
                                               int num_outputs,
                                               const std::string& name) {
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

NodeId SignalGraph::add_plugin_node(std::unique_ptr<PluginSlot> slot,
                                    int num_inputs, int num_outputs,
                                    const std::string& name) {
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

bool SignalGraph::register_custom_node_type(CustomNodeType type) {
    if (!is_valid_custom_node_type(type)) return false;
    // Scans nodes_ and mutates custom_node_types_; serialize against a concurrent
    // prepare()/mutator. custom_node_type() reads custom_node_types_ lock-free and
    // assumes the caller holds this mutex (true for every internal caller below).
    GraphMutationLock mutation_lock(*this);
    cancel_swap_edit_locked_();
    const bool affects_existing_nodes = std::any_of(
        nodes_.begin(), nodes_.end(), [&](const GraphNode& node) {
            return node.type == NodeType::Custom
                && node.custom_type_id == type.type_id
                && node.custom_type_version == type.version;
        });
    if (type.default_name.empty()) type.default_name = type.type_id;
    const auto key = custom_node_key(type.type_id, type.version);
    custom_node_types_[key] = std::move(type);
    // M6 (2.2b): any registry change bumps the generation so a reinit-free swap
    // compiled against an older generation is rejected (a re-register rebinds
    // callbacks to instances the old factory produced).
    ++custom_registry_generation_;
    if (affects_existing_nodes) invalidate_live_locked_();
    return true;
}

const CustomNodeType* SignalGraph::custom_node_type(std::string_view type_id) const {
    const CustomNodeType* latest = nullptr;
    const std::string wanted(type_id);
    for (const auto& [_, type] : custom_node_types_) {
        if (type.type_id != wanted) continue;
        if (!latest || type.version > latest->version) latest = &type;
    }
    return latest;
}

const CustomNodeType* SignalGraph::custom_node_type(std::string_view type_id,
                                                    int version) const {
    auto it = custom_node_types_.find(custom_node_key(type_id, version));
    if (it == custom_node_types_.end()) return nullptr;
    return &it->second;
}

// add_custom_node overloads only RESOLVE a registered type (read
// custom_node_types_) and delegate to the public add_unresolved_custom_node leaf,
// which takes graph_mutation_mutex_ once for the nodes_ mutation. They do NOT
// take the lock themselves — delegating through another locked public method
// would self-deadlock this non-recursive mutex. Type registration is expected to
// precede topology building (register_custom_node_type holds the same mutex), so
// the custom_node_types_ read here is consistent with that ordering.
NodeId SignalGraph::add_custom_node(std::string_view type_id,
                                    const std::string& name) {
    const auto* type = custom_node_type(type_id);
    if (!type) return 0;
    return add_custom_node(type_id, type->version, name);
}

NodeId SignalGraph::add_custom_node(std::string_view type_id,
                                    int version,
                                    const std::string& name) {
    const auto* type = custom_node_type(type_id, version);
    if (!type) return 0;
    return add_unresolved_custom_node(
        type->type_id,
        type->version,
        type->num_input_ports,
        type->num_output_ports,
        name.empty() ? type->default_name : name);
}

NodeId SignalGraph::add_unresolved_custom_node(std::string_view type_id,
                                               int version,
                                               int num_inputs,
                                               int num_outputs,
                                               const std::string& name) {
    CustomNodeType type;
    type.type_id = std::string(type_id);
    type.version = version;
    type.num_input_ports = num_inputs;
    type.num_output_ports = num_outputs;
    type.default_name = name;
    if (!is_valid_custom_node_type(type)) return 0;

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
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
        [id](const GraphNode& n) { return n.id == id; });
    if (it == nodes_.end()) return false;
    connections_.erase(
        std::remove_if(connections_.begin(), connections_.end(),
            [id](const Connection& c) {
                return c.source_node == id || c.dest_node == id;
            }),
        connections_.end());
    nodes_.erase(it);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect(NodeId source, PortIndex source_port,
                          NodeId dest, PortIndex dest_port) {
    GraphMutationLock mutation_lock(*this);
    const GraphNode* src_n = node(source);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n) return false;
    if (!has_output_port(*src_n, source_port)) return false;
    if (!has_input_port(*dst_n, dest_port)) return false;
    if (would_create_cycle(source, dest)) return false;
    Connection conn{source, source_port, dest, dest_port};
    for (auto& c : connections_) if (c == conn) return false;
    connections_.push_back(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect_midi(NodeId source, NodeId dest) {
    GraphMutationLock mutation_lock(*this);
    if (!node(source) || !node(dest)) return false;
    if (would_create_cycle(source, dest)) return false;
    Connection conn{source, 0, dest, 0, false, true};
    for (auto& c : connections_) if (c == conn && c.midi == conn.midi) return false;
    connections_.push_back(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect_sidechain(NodeId source, PortIndex source_port,
                                    NodeId dest, PortIndex dest_sidechain_port) {
    // Sidechain connections only make sense to Plugin nodes; everything
    // else (Gain, Custom, AudioOutput, ...) has no notion of a sidechain
    // bus. We reject other destinations early so callers fail loudly
    // instead of silently routing into a regular audio port.
    GraphMutationLock mutation_lock(*this);
    const GraphNode* src_n = node(source);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n) return false;
    if (dst_n->type != NodeType::Plugin) return false;
    if (!has_output_port(*src_n, source_port)) return false;
    if (!has_input_port(*dst_n, dest_sidechain_port)) return false;
    if (would_create_cycle(source, dest)) return false;

    Connection conn{};
    conn.source_node = source;
    conn.source_port = source_port;
    conn.dest_node = dest;
    conn.dest_port = dest_sidechain_port;
    conn.sidechain = true;
    for (auto& c : connections_) if (c == conn) return false;
    connections_.push_back(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect_automation(NodeId src, PortIndex src_audio_port,
                                     NodeId dest, uint32_t dest_param_id,
                                     float range_lo, float range_hi,
                                     float smoothing_ms,
                                     AutomationMix mix) {
    GraphMutationLock mutation_lock(*this);
    const GraphNode* src_n = node(src);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n) return false;
    if (dst_n->type != NodeType::Plugin || !dst_n->plugin) return false;
    if (!has_output_port(*src_n, src_audio_port)) return false;

    // Reject automation edges that would introduce a cycle. Automation
    // edges contribute to topological order (the source must be processed
    // before the dest), so a back-edge here would make the graph
    // un-orderable. Use the same has_path_locked_ check as connect().
    if (would_create_cycle(src, dest)) return false;

    // Parameter must exist, be automatable, and not read-only.
    bool ok_param = false;
    for (const auto& pi : cached_or_live_params_locked_(*dst_n)) {
        if (pi.id != dest_param_id) continue;
        if (!pi.flags.automatable || pi.flags.read_only) return false;
        ok_param = true;
        break;
    }
    if (!ok_param) return false;

    // Second Replace edge to same (dest, param) is rejected.
    if (mix == AutomationMix::Replace) {
        for (const auto& c : connections_) {
            if (c.automation && c.dest_node == dest
                && c.automation_param_id == dest_param_id
                && c.automation_mix == AutomationMix::Replace) {
                return false;
            }
        }
    }

    Connection conn{};
    conn.source_node              = src;
    conn.source_port              = src_audio_port;
    conn.dest_node                = dest;
    conn.dest_port                = 0;
    conn.automation               = true;
    conn.automation_param_id      = dest_param_id;
    conn.automation_range_lo      = range_lo;
    conn.automation_range_hi      = range_hi;
    conn.automation_smoothing_ms  = std::max(0.0f, smoothing_ms);
    conn.automation_mix           = mix;
    connections_.push_back(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect_audio_rate_modulation(NodeId src, PortIndex src_audio_port,
                                                NodeId dest, uint32_t dest_param_id,
                                                float range_lo, float range_hi,
                                                float smoothing_ms,
                                                AutomationMix mix) {
    // Holds graph_mutation_mutex_ across audio_rate_modulation_lane() below, which
    // is a lock-free helper that assumes the caller holds it (it scans nodes_).
    GraphMutationLock mutation_lock(*this);
    const GraphNode* src_n = node(src);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n) return false;
    if (dst_n->type != NodeType::Plugin || !dst_n->plugin) return false;
    if (!has_output_port(*src_n, src_audio_port)) return false;
    if (would_create_cycle(src, dest)) return false;

    bool ok_param = false;
    for (const auto& pi : cached_or_live_params_locked_(*dst_n)) {
        if (pi.id != dest_param_id) continue;
        if (!parameter_allows_modulation(
                pi, dest_param_id, state::ParamRate::AudioRate, true)) {
            return false;
        }
        ok_param = true;
        break;
    }
    if (!ok_param) return false;

    if (mix == AutomationMix::Replace) {
        for (const auto& c : connections_) {
            if (c.audio_rate_modulation && c.dest_node == dest
                && c.automation_param_id == dest_param_id
                && c.automation_mix == AutomationMix::Replace) {
                return false;
            }
        }
    }

    Connection conn{};
    conn.source_node              = src;
    conn.source_port              = src_audio_port;
    conn.dest_node                = dest;
    conn.dest_port                = 0;
    conn.audio_rate_modulation    = true;
    conn.automation_param_id      = dest_param_id;
    conn.automation_range_lo      = range_lo;
    conn.automation_range_hi      = range_hi;
    conn.automation_smoothing_ms  = std::max(0.0f, smoothing_ms);
    conn.automation_mix           = mix;
    state::ModulationLane lane;
    // Mutex already held: call the lock-free core directly (the public
    // audio_rate_modulation_lane would re-lock and self-deadlock).
    if (!audio_rate_modulation_lane_locked_(conn, lane)) return false;
    connections_.push_back(conn);
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
    if (!src_n || !dst_n || dst_n->type != NodeType::Plugin || !dst_n->plugin) {
        return false;
    }
    if (!has_output_port(*src_n, connection.source_port)) {
        return false;
    }

    for (const auto& pi : cached_or_live_params_locked_(*dst_n)) {
        if (pi.id != connection.automation_param_id) continue;

        lane = state::ModulationLane{
            .source = {
                .id = static_cast<state::ModulationSourceId>(connection.source_node),
                .scope = state::ModulationScope::GraphNode,
                .rate = state::ModulationRate::Audio,
            },
            .target = {
                .param_id = pi.id,
                .scope = state::ModulationScope::GraphNode,
                .param_rate = pi.rate,
                .modulatable = pi.flags.modulatable
                    && pi.flags.automatable
                    && !pi.flags.stepped,
                .writable = !pi.flags.read_only,
            },
            .mix = modulation_mix_for(connection.automation_mix),
            .depth = std::abs(connection.automation_range_hi
                              - connection.automation_range_lo),
        };
        return state::validate_modulation_lane(lane).accepted;
    }

    return false;
}

bool SignalGraph::inject_midi(NodeId id, const midi::MidiBuffer& events) {
    // Pin the live snapshot for the whole dereference: this control-thread API
    // is not the prepare/release thread, so without the guard a concurrent
    // prepare()/release()/invalidate could retire+free `cg` mid-use.
    auto read_guard = live_slot_.read();
    auto* cg = read_guard.get();
    if (!cg) return false;
    auto it = cg->runtime.find(id);
    if (it == cg->runtime.end()) return false;
    auto shape_it = cg->shapes.find(id);
    if (shape_it == cg->shapes.end()
        || shape_it->second.type != NodeType::MidiInput
        || !it->second.midi_input_mailbox) {
        return false;
    }

    auto& mailbox = *it->second.midi_input_mailbox;
    const uint64_t sequence =
        mailbox.next_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool copied_all = mailbox.writer_scratch.set_from_midi(events, sequence);
    mailbox.published.write(mailbox.writer_scratch);
    return copied_all;
}

bool SignalGraph::inject_parameter_events(
    NodeId id,
    const state::ParameterEventQueue& events) {
    // Same reader-pinned, single-writer publication discipline as inject_midi.
    // The fixed-capacity writer scratch and TripleBuffer were prepared with the
    // snapshot, so this path performs no allocation.
    auto read_guard = live_slot_.read();
    auto* cg = read_guard.get();
    if (!cg) return false;
    auto runtime_it = cg->runtime.find(id);
    if (runtime_it == cg->runtime.end()) return false;
    const auto shape_it = cg->shapes.find(id);
    if (shape_it == cg->shapes.end()
        || shape_it->second.type != NodeType::Plugin
        || cg->plugins.find(id) == cg->plugins.end()
        || !runtime_it->second.parameter_input_mailbox) {
        return false;
    }

    auto& mailbox = *runtime_it->second.parameter_input_mailbox;
    const std::uint64_t sequence =
        mailbox.next_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool copied_all =
        mailbox.writer_scratch.set_from_queue(events, sequence);
    mailbox.published.write(mailbox.writer_scratch);
    return copied_all;
}

std::uint64_t SignalGraph::append_parameter_mailbox_events_(
    void* runtime,
    state::ParameterEventQueue& destination) noexcept {
    auto* rt = static_cast<NodeRuntime*>(runtime);
    if (rt == nullptr || !rt->parameter_input_mailbox) return 0;
    const auto& injected = rt->parameter_input_mailbox->published.read();
    const std::uint64_t sequence_seen =
        rt->parameter_input_mailbox->sequence_seen.load(std::memory_order_relaxed);
    if (injected.sequence == 0
        || injected.sequence == sequence_seen) {
        return 0;
    }
    // Existing graph automation is already in `destination`; append injection
    // afterward so it wins stable-sort ties without displacing existing events
    // when the fixed queue is already near capacity.
    (void)injected.append_to(destination);
    return injected.sequence;
}

bool SignalGraph::extract_midi(NodeId id, midi::MidiBuffer& out) const {
    // Pin the live snapshot for the whole dereference (see inject_midi). const
    // method: Slot::read() only touches the slot's mutable reader counter.
    auto read_guard = live_slot_.read();
    auto* cg = read_guard.get();
    if (!cg) return false;
    auto it = cg->runtime.find(id);
    if (it == cg->runtime.end()) return false;
    auto shape_it = cg->shapes.find(id);
    if (shape_it == cg->shapes.end()
        || shape_it->second.type != NodeType::MidiOutput
        || !it->second.midi_output_mailbox) {
        return false;
    }

    const auto& snapshot = it->second.midi_output_mailbox->read();
    return snapshot.copy_to_midi(out);
}

bool SignalGraph::connect_feedback(NodeId source, PortIndex source_port,
                                   NodeId dest, PortIndex dest_port) {
    GraphMutationLock mutation_lock(*this);
    const GraphNode* src_n = node(source);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n) return false;
    if (!has_output_port(*src_n, source_port)) return false;
    if (!has_input_port(*dst_n, dest_port)) return false;
    Connection conn{source, source_port, dest, dest_port, true};
    for (auto& c : connections_) if (c == conn) return false;
    connections_.push_back(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::disconnect(NodeId source, PortIndex source_port,
                             NodeId dest, PortIndex dest_port) {
    GraphMutationLock mutation_lock(*this);
    Connection target{source, source_port, dest, dest_port};
    auto it = std::find(connections_.begin(), connections_.end(), target);
    if (it == connections_.end()) return false;
    connections_.erase(it);
    invalidate_live_locked_();
    return true;
}

// Does not lock: scans nodes_; the caller MUST hold graph_mutation_mutex_
// (every internal mutator/reader below does). Public direct callers
// (e.g. nodes()/node() external users) own their serialization per the accessor
// contract documented on graph_mutation_mutex_.
const GraphNode* SignalGraph::node(NodeId id) const {
    for (auto& n : nodes_) if (n.id == id) return &n;
    return nullptr;
}

GraphNode* SignalGraph::node_mut_locked_(NodeId id) {
    assert_graph_mutation_locked_();
    return const_cast<GraphNode*>(node(id));
}

bool SignalGraph::has_path_locked_(NodeId from, NodeId to) const {
    std::unordered_set<NodeId> visited;
    std::queue<NodeId> queue;
    queue.push(from);
    while (!queue.empty()) {
        auto current = queue.front();
        queue.pop();
        if (current == to) return true;
        if (visited.count(current)) continue;
        visited.insert(current);
        for (auto& c : connections_) {
            if (c.feedback) continue;
            if (c.source_node == current) queue.push(c.dest_node);
        }
    }
    return false;
}

bool SignalGraph::would_create_cycle(NodeId source, NodeId dest) const {
    return has_path_locked_(dest, source);
}

std::vector<NodeId> SignalGraph::processing_order() const {
    std::unordered_map<NodeId, int> in_degree;
    for (auto& n : nodes_) in_degree[n.id] = 0;
    for (auto& c : connections_) {
        if (c.feedback) continue;
        in_degree[c.dest_node]++;
    }
    std::queue<NodeId> queue;
    for (auto& [id, deg] : in_degree) if (deg == 0) queue.push(id);
    std::vector<NodeId> order;
    while (!queue.empty()) {
        auto current = queue.front();
        queue.pop();
        order.push_back(current);
        for (auto& c : connections_) {
            if (c.feedback) continue;
            // Automation edges DO contribute to topological order — the
            // source must be processed before the dest so its output
            // buffer is valid when we sample it for param events.
            if (c.source_node == current) {
                if (--in_degree[c.dest_node] == 0) queue.push(c.dest_node);
            }
        }
    }
    return order;
}

bool SignalGraph::set_node_parameter(NodeId id, uint32_t param_id, float value) {
    // Scans nodes_ via node(); serialize against a concurrent mutator/prepare.
    // Forwards to the plugin slot's own (independently synchronized) parameter
    // store — no GraphNode plain field is written here.
    GraphMutationLock mutation_lock(*this);
    auto* n = node_mut_locked_(id);
    if (!n || n->type != NodeType::Plugin || !n->plugin) return false;
    n->plugin->set_parameter(param_id, value);
    return true;
}

float SignalGraph::get_node_parameter(NodeId id, uint32_t param_id) const {
    // Scans nodes_ via node(); serialize against a concurrent mutator/prepare.
    GraphMutationLock mutation_lock(*this);
    auto* n = node(id);
    if (!n || n->type != NodeType::Plugin || !n->plugin) return 0.f;
    return n->plugin->get_parameter(param_id);
}

int SignalGraph::node_latency_samples(NodeId id) const {
    // Pin the live snapshot for the whole dereference (see inject_midi).
    auto read_guard = live_slot_.read();
    const auto* cg = read_guard.get();
    if (!cg) return 0;
    auto it = cg->runtime.find(id);
    if (it == cg->runtime.end()) return 0;
    return (int)it->second.input_latency;
}

SignalGraph::PreparedStats SignalGraph::prepared_stats() const {
    return PreparedStats{
        .node_count = prepared_node_count_.load(std::memory_order_relaxed),
        .ordered_node_count =
            prepared_ordered_node_count_.load(std::memory_order_relaxed),
        .connection_count =
            prepared_connection_count_.load(std::memory_order_relaxed),
        .total_ports = prepared_total_ports_.load(std::memory_order_relaxed),
        .max_block_size = prepared_max_block_size_.load(std::memory_order_relaxed),
        .node_audio_buffer_bytes =
            prepared_node_audio_buffer_bytes_.load(std::memory_order_relaxed),
        .automation_buffer_bytes =
            prepared_automation_buffer_bytes_.load(std::memory_order_relaxed),
        .delay_buffer_bytes =
            prepared_delay_buffer_bytes_.load(std::memory_order_relaxed),
        .total_prepared_buffer_bytes =
            prepared_total_buffer_bytes_.load(std::memory_order_relaxed),
    };
}

int SignalGraph::prepared_max_block_size() const noexcept {
    return live_slot_.live() ? live_slot_.live()->max_block_size : 0;
}

std::atomic<float>* SignalGraph::live_gain_atomic(NodeId id) const noexcept {
    if (!live_slot_.live()) return nullptr;
    auto it = live_slot_.live()->runtime.find(id);
    if (it == live_slot_.live()->runtime.end()) return nullptr;
    return it->second.gain.get();
}

PluginSlot* SignalGraph::live_plugin_slot(NodeId id) const noexcept {
    if (!live_slot_.live()) return nullptr;
    auto it = live_slot_.live()->plugins.find(id);
    if (it == live_slot_.live()->plugins.end()) return nullptr;
    return it->second.get();
}

const CustomNodeProcessFn* SignalGraph::live_custom_processor(NodeId id) const noexcept {
    if (!live_slot_.live()) return nullptr;
    auto it = live_slot_.live()->custom_processors.find(id);
    if (it == live_slot_.live()->custom_processors.end()) return nullptr;
    return &it->second;
}

const CustomNodeTransportProcessFn* SignalGraph::live_custom_transport_processor(
    NodeId id) const noexcept {
    if (!live_slot_.live()) return nullptr;
    auto it = live_slot_.live()->custom_transport_processors.find(id);
    if (it == live_slot_.live()->custom_transport_processors.end()) return nullptr;
    return &it->second;
}

std::shared_ptr<const void> SignalGraph::live_snapshot_handle() const noexcept {
    return live_slot_.live();  // aliases the live CompiledGraph as an opaque keepalive
}

pulp::audio::AudioProcessLoadSnapshot SignalGraph::graph_load() const {
    return graph_load_ ? graph_load_->snapshot() : pulp::audio::AudioProcessLoadSnapshot{};
}

void SignalGraph::set_live_dsp_telemetry_enabled(bool enabled) {
    // Record the desired state so future recompiles seed their store from it, then
    // flip the live snapshot's store immediately (atomic) so the toggle takes effect
    // without waiting for a recompile. Pinning keeps the live snapshot alive for the
    // store access; set_enabled touches only a relaxed atomic.
    desired_live_dsp_telemetry_enabled_.store(enabled, std::memory_order_relaxed);
    auto read_guard = live_slot_.read();
    if (auto* cg = read_guard.get()) {
        cg->live_dsp_telemetry.set_enabled(enabled);
    }
}

bool SignalGraph::live_dsp_telemetry_enabled() const {
    return desired_live_dsp_telemetry_enabled_.load(std::memory_order_relaxed);
}

pulp::audio::LiveDspTelemetrySnapshot SignalGraph::poll_live_dsp_telemetry() {
    // Single non-real-time poller (control/UI thread): pin the live snapshot, drain
    // its ring (the SPSC consumer side; the audio thread is the producer), and return
    // a copy of the latest summary while the snapshot is still pinned alive.
    auto read_guard = live_slot_.read();
    auto* cg = read_guard.get();
    if (!cg) return {};
    cg->live_dsp_telemetry.drain();
    return cg->live_dsp_telemetry.latest();
}

std::vector<SignalGraph::NodeLoadReport> SignalGraph::node_loads() const {
    // Control/UI-thread read of the persistent per-node measurers. node_load_
    // is only mutated on the control thread (compile_), so this is race-free
    // against topology recompiles; the audio thread writes only the measurer
    // objects' relaxed atomics, which snapshot() reads coherently.
    // Filter to currently-present nodes so removed nodes' lingering measurers
    // don't surface as phantom reports. The measurers are intentionally NOT
    // erased from node_load_ (it is insert-only — see compile_): a
    // retired-but-not-yet-drained snapshot may still hold raw
    // NodeRuntime::load pointers into them, so erasing here would risk a
    // use-after-free on the draining audio thread. Residual map growth is
    // bounded by the number of distinct NodeIds the graph has ever held.
    // The nodes_ scan is serialized under graph_mutation_mutex_ (against a
    // concurrent mutator/prepare); take it FIRST and release it before
    // node_load_mu_ so the lock order is graph_mutation_mutex_ -> node_load_mu_,
    // matching compile_() (which holds graph_mutation_mutex_ via prepare() and
    // takes node_load_mu_ inside). live_ids is a private copy, so it is safe to
    // use after releasing graph_mutation_mutex_.
    std::unordered_set<NodeId> live_ids;
    {
        GraphMutationLock mutation_lock(*this);
        live_ids.reserve(nodes_.size());
        for (const auto& n : nodes_) live_ids.insert(n.id);
    }

    std::lock_guard<std::mutex> node_load_lock(node_load_mu_);
    std::vector<NodeLoadReport> reports;
    reports.reserve(node_load_.size());
    for (const auto& [id, measurer] : node_load_) {
        if (measurer && live_ids.count(id) != 0) {
            reports.push_back(NodeLoadReport{id, measurer->snapshot()});
        }
    }
    return reports;
}

SignalGraph::RuntimeBudgetReport
SignalGraph::evaluate_optional_runtime_budget(
    runtime::RuntimeBudgetFrame& frame,
    runtime::RuntimeWorkLane lane,
    bool required) const noexcept {
    const auto stats = prepared_stats();
    std::uint64_t estimated = 0;
    estimated = saturating_add_u64(
        estimated,
        saturating_mul_u64(static_cast<std::uint64_t>(stats.node_count), 16));
    estimated = saturating_add_u64(
        estimated,
        saturating_mul_u64(static_cast<std::uint64_t>(stats.connection_count), 8));
    if (stats.max_block_size > 0) {
        estimated = saturating_add_u64(
            estimated,
            saturating_mul_u64(
                static_cast<std::uint64_t>(stats.total_ports),
                static_cast<std::uint64_t>(stats.max_block_size)));
    }
    estimated = saturating_add_u64(
        estimated,
        static_cast<std::uint64_t>(
            stats.total_prepared_buffer_bytes / sizeof(float)));

    const auto decision = frame.evaluate(lane, estimated, required);
    return {
        .decision = decision,
        .frame_stats = frame.stats(),
        .estimated_cost = estimated,
        .prepared = stats.node_count != 0,
    };
}

void SignalGraph::invalidate_live_locked_() {
    assert_graph_mutation_locked_();
    // During a swap-edit the OWNER thread's allow-set mutations must NOT
    // silence the live snapshot — it keeps playing the old compiled graph until
    // prepare_swap() atomically publishes the new one. Any other caller (a second
    // control thread, or the lifecycle / limits / registry mutators, which
    // force-abort the transaction at their top) falls through and invalidates as
    // usual, which safely cancels the no-silence attempt.
    if (in_swap_edit_ && std::this_thread::get_id() == swap_edit_owner_) return;
    // Drop the live snapshot; process() will return silence until prepare()
    // is called again. This is the simple, safe semantic: UI-thread edits
    // always require a re-prepare before audio resumes.
    live_slot_.unpublish();
    total_latency_samples_.store(0, std::memory_order_relaxed);
    clear_prepared_stats_locked_();
}

void SignalGraph::clear_prepared_stats_locked_() {
    assert_graph_mutation_locked_();
    prepared_node_count_.store(0, std::memory_order_relaxed);
    prepared_ordered_node_count_.store(0, std::memory_order_relaxed);
    prepared_connection_count_.store(0, std::memory_order_relaxed);
    prepared_total_ports_.store(0, std::memory_order_relaxed);
    prepared_max_block_size_.store(0, std::memory_order_relaxed);
    prepared_node_audio_buffer_bytes_.store(0, std::memory_order_relaxed);
    prepared_automation_buffer_bytes_.store(0, std::memory_order_relaxed);
    prepared_delay_buffer_bytes_.store(0, std::memory_order_relaxed);
    prepared_total_buffer_bytes_.store(0, std::memory_order_relaxed);
    transport_suppressed_for_anticipation_.store(0, std::memory_order_relaxed);
}

void SignalGraph::publish_prepared_stats_locked_(const CompiledGraph& cg) {
    assert_graph_mutation_locked_();
    std::size_t total_ports = 0;
    for (const auto& [_, shape] : cg.shapes) {
        total_ports += static_cast<std::size_t>(std::max(0, shape.num_input_ports));
        total_ports += static_cast<std::size_t>(std::max(0, shape.num_output_ports));
    }

    std::size_t node_audio_bytes = 0;
    std::size_t automation_bytes = 0;
    for (const auto& [_, rt] : cg.runtime) {
        node_audio_bytes += (rt.output_data.size() + rt.input_data.size())
            * sizeof(float);
        automation_bytes += rt.audio_rate_param_data.size() * sizeof(float);
    }

    std::size_t delay_bytes = 0;
    for (const auto& delay : cg.connection_delays) {
        delay_bytes += (delay.ring.size() + delay.feedback_prev.size())
            * sizeof(float);
    }

    const std::size_t total_buffer_bytes =
        node_audio_bytes + automation_bytes + delay_bytes;

    prepared_node_count_.store(cg.runtime.size(), std::memory_order_relaxed);
    prepared_ordered_node_count_.store(cg.ordered_runtime.size(),
                                       std::memory_order_relaxed);
    prepared_connection_count_.store(cg.connections.size(), std::memory_order_relaxed);
    prepared_total_ports_.store(total_ports, std::memory_order_relaxed);
    prepared_max_block_size_.store(cg.max_block_size, std::memory_order_relaxed);
    prepared_node_audio_buffer_bytes_.store(node_audio_bytes,
                                            std::memory_order_relaxed);
    prepared_automation_buffer_bytes_.store(automation_bytes,
                                            std::memory_order_relaxed);
    prepared_delay_buffer_bytes_.store(delay_bytes, std::memory_order_relaxed);
    prepared_total_buffer_bytes_.store(total_buffer_bytes,
                                       std::memory_order_relaxed);
}

void SignalGraph::compute_latencies_for_(CompiledGraph& cg,
                                         const std::vector<Connection>& /*conns*/,
                                         const std::unordered_map<NodeId, PreparedPluginMetadata>&
                                             plugin_meta) {
    for (NodeId id : cg.order) {
        auto rt_it = cg.runtime.find(id);
        if (rt_it == cg.runtime.end()) continue;
        auto& rt = rt_it->second;

        int64_t max_upstream = 0;
        bool has_upstream = false;
        for (const auto& c : cg.connections) {
            if (c.dest_node != id) continue;
            // Only latency-aligned audio (plain feedforward or dense audio-rate)
            // contributes to a node's input latency — single-sourced via classify.
            if (!connection_affects_latency(c)) continue;
            auto src_it = cg.runtime.find(c.source_node);
            if (src_it == cg.runtime.end()) continue;
            max_upstream = std::max(max_upstream, src_it->second.output_latency);
            has_upstream = true;
        }
        rt.input_latency = has_upstream ? max_upstream : 0;

        int64_t added = 0;
        // 2.2b (H2): read cached latency, never the live slot — latency_samples()
        // reaches into the live plugin (e.g. VST3 getLatencySamples()) and is
        // unsafe concurrent with process() during a swap-time recompile.
        auto mit = plugin_meta.find(id);
        if (mit != plugin_meta.end()) {
            added = std::max<int64_t>(0, mit->second.latency_samples);
        }
        rt.output_latency = rt.input_latency + added;
    }

    cg.total_latency_samples = 0;
    for (auto& [id, shape] : cg.shapes) {
        if (shape.type != NodeType::AudioOutput) continue;
        auto it = cg.runtime.find(id);
        if (it == cg.runtime.end()) continue;
        cg.total_latency_samples = std::max(cg.total_latency_samples, it->second.input_latency);
    }

    cg.connection_delays.assign(cg.connections.size(), ConnectionDelay{});
    for (size_t i = 0; i < cg.connections.size(); ++i) {
        const auto& c = cg.connections[i];
        auto src_it = cg.runtime.find(c.source_node);
        auto dst_it = cg.runtime.find(c.dest_node);
        if (src_it == cg.runtime.end() || dst_it == cg.runtime.end()) continue;

        if (c.feedback) {
            cg.connection_delays[i].feedback_prev.assign(
                static_cast<size_t>(cg.max_block_size), 0.0f);
            continue;
        }
        // Non-feedback edges that carry no latency-aligned audio (MIDI, sparse
        // automation) get no delay line — single-sourced via classify().
        if (!connection_affects_latency(c)) continue;

        int64_t want = dst_it->second.input_latency - src_it->second.output_latency;
        if (want < 0) want = 0;
        cg.connection_delays[i].delay_samples = (int)want;
        if (want > 0) {
            cg.connection_delays[i].ring.assign(
                static_cast<size_t>((int64_t)cg.max_block_size + want), 0.0f);
            cg.connection_delays[i].write_pos = 0;
        }
    }
}

void SignalGraph::compile_snapshot_for_test(double sample_rate, int max_block_size) {
    // Runs the full compilation path and DROPS the result — no null-first
    // prologue, no publish. See the header: this exercises compile_() concurrently
    // with a live process() for the 2.2a no-silence-swap race contract. Discarding
    // the snapshot frees it here (no reader ever pins it), so this leaks nothing.
    auto snapshot = compile_(sample_rate, max_block_size);
    (void)snapshot;
}

bool SignalGraph::build_routing_snapshot_locked_(
    CompiledGraph& cg, bool parallel_safe,
    std::vector<PluginBindingContext>& plugin_ctx,
    std::vector<CustomBindingContext>& custom_ctx,
    format::GraphRuntimeSnapshot& out) {
    const ExecutorSnapshotBinders binders{
        .gain_for =
            [&cg](NodeId id) -> std::atomic<float>* {
                auto it = cg.runtime.find(id);
                return it == cg.runtime.end() ? nullptr : it->second.gain.get();
            },
        .plugin_for =
            [&cg](NodeId id) -> PluginSlot* {
                auto it = cg.plugins.find(id);
                return it == cg.plugins.end() ? nullptr : it->second.get();
            },
        // Each node's persistent CPU-load measurer, so routed execution reports
        // the same node_loads() telemetry as the legacy walk. The measurers are
        // insert-only and persist across snapshots; node_load_ was populated
        // earlier in compile_. Lock the map's structure mutex (compile_ does not
        // hold it) — a concurrent UI-thread node_loads() poll iterates under the
        // same lock.
        .load_for =
            [this](NodeId id) -> audio::AudioProcessLoadMeasurer* {
                std::lock_guard<std::mutex> node_load_lock(node_load_mu_);
                auto it = node_load_.find(id);
                return it == node_load_.end() ? nullptr : it->second.get();
            },
        .custom_for =
            [&cg](NodeId id) -> const CustomNodeProcessFn* {
                auto it = cg.custom_processors.find(id);
                return it == cg.custom_processors.end() ? nullptr : &it->second;
            },
        .custom_transport_for =
            [&cg](NodeId id) -> const CustomNodeTransportProcessFn* {
                auto it = cg.custom_transport_processors.find(id);
                return it == cg.custom_transport_processors.end() ? nullptr
                                                                  : &it->second;
            },
        // Cached plugin metadata so the routing build makes no live PluginSlot
        // metadata call (safe for a swap-time recompile).
        .plugin_latency_for =
            [this](NodeId id) -> int {
                auto it = prepared_plugin_meta_.find(id);
                return it == prepared_plugin_meta_.end()
                           ? 0
                           : it->second.latency_samples;
            },
        .plugin_params_for =
            [this](NodeId id) -> const std::vector<HostParamInfo>* {
                auto it = prepared_plugin_meta_.find(id);
                return it == prepared_plugin_meta_.end() ? nullptr
                                                         : &it->second.parameters;
            },
        .parameter_events_for =
            [&cg](NodeId id) -> ParameterEventInjectionBinding {
                auto it = cg.runtime.find(id);
                if (it == cg.runtime.end() ||
                    !it->second.parameter_input_mailbox) {
                    return {};
                }
                return {
                    .user_data = &it->second,
                    .append = &SignalGraph::append_parameter_mailbox_events_,
                    .sequence_seen =
                        &it->second.parameter_input_mailbox->sequence_seen,
                };
            },
    };
    return build_executor_snapshot(nodes_, connections_, binders, plugin_ctx,
                                   cg.routed.plugin_scratch, out, parallel_safe,
                                   &custom_ctx);
}

std::shared_ptr<SignalGraph::CompiledGraph>
SignalGraph::compile_(double sample_rate, int max_block_size, CompileMode mode) {
    auto cg = std::make_shared<CompiledGraph>();
    cg->max_block_size = max_block_size;
    cg->sample_rate = sample_rate;
    cg->connections = connections_;
    cg->custom_registry_generation = custom_registry_generation_;  // 2.2b predicate (M6)

#ifndef NDEBUG
    // 2.2b (H2): every plugin node MUST have captured metadata. A cache miss
    // silently yields 0 latency / empty param-bounds / inert transport (wrong
    // PDC, wrong automation bounds, transport plugin wrongly ahead-rendered) —
    // exactly what this cache prevents. Impossible in prepare()->compile_(), but
    // a future off-thread swap-recompile that added a plugin without re-capturing
    // would fail SILENTLY; assert loudly instead.
    for (const auto& dbg_n : nodes_) {
        assert((!dbg_n.plugin || prepared_plugin_meta_.count(dbg_n.id) == 1) &&
               "compile_: plugin node missing from prepared_plugin_meta_ — a swap "
               "recompiled without re-capturing plugin metadata (2.2b H2)");
    }
#endif
    cg->order = processing_order();

    for (auto& n : nodes_) {
        NodeRuntime rt;
        // Resolve (or lazily create) this node's persistent load measurer.
        // node_load_ only grows here, so the raw measurer pointer handed to the
        // audio thread via NodeRuntime::load stays valid across snapshot swaps.
        // Locked against a concurrent node_loads() poll on the UI thread.
        {
            std::lock_guard<std::mutex> node_load_lock(node_load_mu_);
            auto& load_slot = node_load_[n.id];
            if (!load_slot) {
                load_slot = std::make_unique<audio::AudioProcessLoadMeasurer>();
            }
            rt.load = load_slot.get();
        }
        const int out_ch = std::max(0, n.num_output_ports);
        const int in_ch  = std::max(0, n.num_input_ports);
        rt.output_data.assign(static_cast<size_t>(out_ch) * max_block_size, 0.f);
        rt.input_data.assign(static_cast<size_t>(in_ch) * max_block_size, 0.f);
        rt.output_ptrs.resize(out_ch);
        rt.input_ptrs.resize(in_ch);
        rt.input_const_ptrs.resize(in_ch);
        for (int c = 0; c < out_ch; ++c)
            rt.output_ptrs[c] = rt.output_data.data() + static_cast<size_t>(c) * max_block_size;
        for (int c = 0; c < in_ch; ++c) {
            rt.input_ptrs[c] = rt.input_data.data() + static_cast<size_t>(c) * max_block_size;
            rt.input_const_ptrs[c] = rt.input_ptrs[c];
        }
        rt.gain = std::make_unique<std::atomic<float>>(n.gain);
        if (n.plugin) {
            // 2.2b (H2): read cached parameter bounds, not the live slot.
            auto mit = prepared_plugin_meta_.find(n.id);
            if (mit != prepared_plugin_meta_.end()) {
                for (const auto& p : mit->second.parameters) {
                    rt.param_bounds.push_back({
                        p.id,
                        p.min_value,
                        p.max_value,
                    });
                }
            }
        }
        auto [runtime_it, inserted] = cg->runtime.emplace(n.id, std::move(rt));
        (void)inserted;
        prepare_midi_block_storage(runtime_it->second.midi_in,
                                   runtime_it->second.midi_in_ump);
        prepare_midi_block_storage(runtime_it->second.midi_out,
                                   runtime_it->second.midi_out_ump);
        if (n.type == NodeType::MidiInput) {
            runtime_it->second.midi_input_mailbox =
                std::make_unique<MidiInputMailbox>();
        }
        if (n.type == NodeType::MidiOutput) {
            runtime_it->second.midi_output_mailbox =
                std::make_unique<runtime::TripleBuffer<MidiBlockSnapshot>>();
        }
        if (n.type == NodeType::Plugin && n.plugin) {
            // Keep mailbox identity across a live snapshot recompile so a
            // batch published while prepare_swap() is building cannot fall
            // between the old and new snapshots. Normal eager prepare has no
            // live snapshot here and allocates a fresh mailbox.
            if (const auto live = live_slot_.live()) {
                auto old_it = live->runtime.find(n.id);
                if (old_it != live->runtime.end()) {
                    runtime_it->second.parameter_input_mailbox =
                        old_it->second.parameter_input_mailbox;
                }
            }
            if (!runtime_it->second.parameter_input_mailbox) {
                runtime_it->second.parameter_input_mailbox =
                    std::make_shared<ParameterInputMailbox>();
            }
        }

        CompiledGraph::NodeShape shape{n.type, n.num_input_ports, n.num_output_ports};
        cg->shapes[n.id] = shape;

        if (n.plugin) cg->plugins[n.id] = n.plugin;
        // Resolve the node's transport-sensitivity ONCE here (before the
        // anticipation eligibility analysis and the routed-snapshot build, both
        // later in compile_). The SAME GraphNode::transport_sensitive value feeds
        // the anticipation partition (seeds AnticipationExclusion::TransportSensitive)
        // and the routed binding (PluginBindingContext::wants_transport /
        // CustomBindingContext::process_transport), so the two can never disagree.
        // Prepare-stable: a slot/type whose capability changes later needs a
        // re-prepare to be observed.
        n.transport_sensitive = false;
        if (n.type == NodeType::Plugin) {
            // 2.2b (H2): read cached transport-sensitivity, not the live slot.
            auto mit = prepared_plugin_meta_.find(n.id);
            n.transport_sensitive =
                (mit != prepared_plugin_meta_.end()) && mit->second.wants_transport;
        }
        if (n.type == NodeType::Custom) {
            // 2.2b: record this Custom node's instance identity for the reinit-
            // free-swap predicate (set-equality vs a prepare_swap candidate).
            cg->custom_instances[n.id] = n.custom_instance.get();
            if (const auto* type = custom_node_type(n.custom_type_id,
                                                    n.custom_type_version);
                type && custom_type_matches_node_shape(*type, n)) {
                if (n.custom_instance && type->process_instance) {
                    // Bind the stateful processor. The lambda captures the
                    // instance shared_ptr BY VALUE, so this snapshot keeps the
                    // instance alive for its whole audio-thread lifetime (same
                    // guarantee as cg->plugins[...] for plugin nodes). No raw
                    // pointer into GraphNode is stored.
                    auto inst = n.custom_instance;
                    auto fn = type->process_instance;
                    cg->custom_processors[n.id] =
                        [inst, fn](audio::BufferView<float>& out,
                                   const audio::BufferView<const float>& in,
                                   int num_samples) {
                            fn(inst.get(), out, in, num_samples);
                        };
                } else if (type->process) {
                    cg->custom_processors[n.id] = type->process;
                }
                // Transport-aware resolution mirrors the plain one: prefer the
                // stateful variant when an instance + stateful callback exist,
                // else the stateless variant. Any non-empty result marks the node
                // transport-sensitive (the presence of the map entry mirrors the
                // flag, both resolved from this one condition).
                if (n.custom_instance && type->process_instance_transport) {
                    auto inst = n.custom_instance;
                    auto fn = type->process_instance_transport;
                    cg->custom_transport_processors[n.id] =
                        [inst, fn](audio::BufferView<float>& out,
                                   const audio::BufferView<const float>& in,
                                   int num_samples,
                                   const format::ProcessContext& transport) {
                            fn(inst.get(), out, in, num_samples, transport);
                        };
                    n.transport_sensitive = true;
                } else if (type->process_transport) {
                    cg->custom_transport_processors[n.id] = type->process_transport;
                    n.transport_sensitive = true;
                }
            }
        }
    }

    for (size_t ci = 0; ci < cg->connections.size(); ++ci) {
        const auto& c = cg->connections[ci];
        auto rt_it = cg->runtime.find(c.dest_node);
        if (rt_it == cg->runtime.end()) continue;
        auto src_rt_it = cg->runtime.find(c.source_node);
        NodeRuntime* source_runtime =
            src_rt_it == cg->runtime.end() ? nullptr : &src_rt_it->second;
        auto& rt = rt_it->second;
        NodeRuntime::EdgeRef edge_ref{ci, source_runtime};
        // Lane bucketing is single-sourced through classify() — the same helper
        // the executor-routing gather uses — so this reference walk and the
        // routed path can never disagree about which lane a Connection carries.
        // sidechain folds into Audio; feedback is orthogonal; the sparse-vs-dense
        // automation split keys off the dense audio_rate flag (the two automation
        // forms are mutually exclusive by construction).
        const ConnectionClass cls = classify(c);
        if (cls.feedback) cg->feedback_edges.push_back(edge_ref);
        if (cls.kind == graph::GraphRuntimeConnectionKind::Event && !cls.feedback) {
            rt.inbound_midi_edges.push_back(edge_ref);
        } else if (cls.kind == graph::GraphRuntimeConnectionKind::Audio) {
            rt.inbound_audio_edges.push_back(edge_ref);
        }
        if (cls.kind == graph::GraphRuntimeConnectionKind::Automation && !cls.audio_rate) {
            rt.sparse_automation_edges.push_back(edge_ref);
            auto& ids = rt.sparse_automation_param_ids;
            if (std::find(ids.begin(), ids.end(), c.automation_param_id) == ids.end()) {
                ids.push_back(c.automation_param_id);
                rt.sparse_automation_accum.resize(ids.size());
            }
        }
        if (cls.kind == graph::GraphRuntimeConnectionKind::Automation && cls.audio_rate) {
            rt.audio_rate_modulation_edges.push_back(edge_ref);
            auto& ids = rt.audio_rate_param_ids;
            if (std::find(ids.begin(), ids.end(), c.automation_param_id) == ids.end()) {
                ids.push_back(c.automation_param_id);
                rt.audio_rate_param_data.resize(
                    ids.size() * static_cast<size_t>(max_block_size), 0.0f);
                rt.audio_rate_accum.resize(ids.size());
            }
        }
    }

    cg->ordered_runtime.reserve(cg->order.size());
    for (NodeId id : cg->order) {
        auto rt_it = cg->runtime.find(id);
        auto shape_it = cg->shapes.find(id);
        if (rt_it == cg->runtime.end() || shape_it == cg->shapes.end()) continue;
        cg->ordered_runtime.push_back({
            id,
            shape_it->second,
            &rt_it->second,
        });
    }

    // Prepare this snapshot's per-node live-DSP telemetry store in ordered_runtime
    // order (slot i == ordered_runtime[i]). Enabled state is inherited from the
    // control-thread desired flag so a toggle survives recompiles. Allocation-free
    // once the audio thread is running; all storage is reserved here.
    {
        const auto kind_of = [](NodeType t) noexcept {
            switch (t) {
                case NodeType::AudioInput:  return audio::LiveDspNodeKind::AudioInput;
                case NodeType::AudioOutput: return audio::LiveDspNodeKind::AudioOutput;
                case NodeType::Plugin:      return audio::LiveDspNodeKind::Plugin;
                case NodeType::Gain:        return audio::LiveDspNodeKind::Gain;
                case NodeType::MidiInput:   return audio::LiveDspNodeKind::MidiInput;
                case NodeType::MidiOutput:  return audio::LiveDspNodeKind::MidiOutput;
                case NodeType::Custom:      return audio::LiveDspNodeKind::Custom;
            }
            return audio::LiveDspNodeKind::Unknown;
        };
        std::vector<audio::LiveDspNodeInfo> infos;
        infos.reserve(cg->ordered_runtime.size());
        for (const auto& ordered : cg->ordered_runtime) {
            audio::LiveDspNodeInfo info;
            info.node_id = ordered.id;
            info.kind = kind_of(ordered.shape.type);
            info.input_ports = static_cast<std::uint32_t>(
                std::max(0, ordered.shape.num_input_ports));
            info.output_ports = static_cast<std::uint32_t>(
                std::max(0, ordered.shape.num_output_ports));
            info.set_name(to_string(info.kind));
            infos.push_back(info);
        }
        if (!infos.empty()) {
            audio::LiveDspTelemetryConfig tcfg;
            cg->live_dsp_telemetry.prepare(tcfg, infos);
            cg->live_dsp_telemetry.set_enabled(
                desired_live_dsp_telemetry_enabled_.load(std::memory_order_relaxed));
        }
    }

    compute_latencies_for_(*cg, connections_, prepared_plugin_meta_);

    // Build the canonical-executor routing for this snapshot when the topology
    // is eligible. The Gain bindings resolve to THIS snapshot's own gain atomics
    // (valid for cg's whole lifetime), so the embedded snapshot needs no
    // keepalive. Ineligible graphs leave routed.serial.valid false and use the walk.
    {
        // Serial routed snapshot (compact buffer layout). The resolver set that
        // reads THIS snapshot's own runtime/plugins and the cached plugin
        // metadata lives in build_routing_snapshot_locked_, shared with the
        // parallel path so the two never drift.
        cg->routed.serial.valid = build_routing_snapshot_locked_(
            *cg, /*parallel_safe=*/false, cg->routed.serial.plugin_ctx,
            cg->routed.serial.custom_ctx, cg->routed.serial.snapshot);
        // Size THIS snapshot's own scratch pool (per-snapshot, retired with the
        // snapshot via RCU — never resized under an in-flight reader).
        if (cg->routed.serial.valid && max_block_size > 0) {
            cg->routed.serial.valid = cg->routed.serial.pool.reset(
                cg->routed.serial.snapshot.buffer_slot_count(),
                static_cast<std::uint32_t>(max_block_size),
                cg->routed.serial.snapshot.buffer_assignment().connection_delay_samples);
        }
        // Per-snapshot MIDI scratch + the MidiInput/MidiOutput node index lists
        // the routed dispatch bridges to the mailboxes. Built only when the
        // routed plan carries MIDI nodes, so audio-only graphs allocate none.
        cg->routed.midi_inputs.clear();
        cg->routed.midi_outputs.clear();
        if (cg->routed.serial.valid) {
            const auto& plan = cg->routed.serial.snapshot.plan();
            bool plan_has_midi = false;
            for (std::uint32_t i = 0; i < plan.nodes.size(); ++i) {
                const auto kind = plan.nodes[i].kind;
                if (kind == graph::GraphRuntimeNodeKind::MidiInput) {
                    cg->routed.midi_inputs.push_back({i, plan.nodes[i].id});
                    plan_has_midi = true;
                } else if (kind == graph::GraphRuntimeNodeKind::MidiOutput) {
                    cg->routed.midi_outputs.push_back({i, plan.nodes[i].id});
                    plan_has_midi = true;
                } else if (plan.nodes[i].event_input_ports > 0 ||
                           plan.nodes[i].event_output_ports > 0) {
                    plan_has_midi = true;  // a plugin carrying MIDI edges
                }
            }
            if (plan_has_midi) {
                cg->routed.serial.valid = cg->routed.midi.reset(plan.node_count());
            }
            // Per-snapshot sparse-automation scratch, built only when the routed
            // plan carries automation connections (audio-only / MIDI graphs
            // allocate none).
            bool plan_has_automation = false;
            for (const auto& conn : plan.connections) {
                if (graph::is_automation_conn(conn)) { plan_has_automation = true; break; }
            }
            if (cg->routed.serial.valid && plan_has_automation && max_block_size > 0) {
                cg->routed.serial.valid = cg->routed.automation.reset(
                    plan, static_cast<std::uint32_t>(max_block_size));
            }
        }

        // Levelized parallel routing: when enabled (at this prepare), build a
        // PARALLEL-SAFE (reuse-free) snapshot of the same eligible graph + its
        // levelization + a dedicated scratch pool, and ensure the persistent
        // worker pool is running. The MIDI/automation scratch and MidiInput/Output
        // node lists are SHARED with the serial path (identical plan). On any
        // failure the parallel path stays invalid and process() falls back.
        cg->routed.parallel.valid = false;
        if (cg->routed.serial.valid && parallel_routing_enabled_.load(std::memory_order_relaxed) &&
            max_block_size > 0) {
            // Parallel-safe routed snapshot: same resolver set as the serial
            // path (via build_routing_snapshot_locked_) but a reuse-free buffer
            // assignment so concurrent same-level nodes never alias a recycled
            // slot. The MIDI/automation scratch and MidiInput/Output node lists
            // are SHARED with the serial path (identical plan).
            bool ok = build_routing_snapshot_locked_(
                *cg, /*parallel_safe=*/true, cg->routed.parallel.plugin_ctx,
                cg->routed.parallel.custom_ctx, cg->routed.parallel.snapshot);
            if (ok) {
                cg->routed.parallel.levelization = graph::build_graph_runtime_levelization(
                    cg->routed.parallel.snapshot.plan());
                ok = cg->routed.parallel.levelization.ok &&
                     cg->routed.parallel.pool.reset(
                         cg->routed.parallel.snapshot.buffer_slot_count(),
                         static_cast<std::uint32_t>(max_block_size),
                         cg->routed.parallel.snapshot.buffer_assignment()
                             .connection_delay_samples);
            }
            if (ok) {
                // Start the persistent worker pool off the audio thread, ONCE.
                // Hardware concurrency capped to a sane bound; participant 0 is
                // the audio thread, so the pool spawns worker_count - 1 threads.
                //
                // INVARIANT (load-bearing for audio-thread safety): the pool size
                // is fixed for the SignalGraph's lifetime and the pool is never
                // stopped/resized on a re-prepare. start()/stop() join worker
                // threads and reset epoch_/completed_/worker_count_; running them
                // concurrently with an in-flight process_parallel -> run() on the
                // audio thread would be a use-after-free. The only legal stop is
                // ~GraphRuntimeWorkerPool during ~SignalGraph, after the audio
                // thread has (by contract) stopped calling process(). So: start
                // only when not yet started (worker_count() == 0 — also retries a
                // previously failed start, safe because routed.parallel.valid was
                // false so process() never entered the parallel branch). A re-
                // prepare just re-checks running(); it must not restart the pool.
                if (worker_pool_.worker_count() == 0) {
                    const unsigned hw = std::thread::hardware_concurrency();
                    const std::uint32_t workers =
                        std::clamp<std::uint32_t>(hw == 0 ? 2 : hw, 2, 16);
                    ok = worker_pool_.start(workers);
                } else {
                    ok = worker_pool_.running();
                }
            }
            cg->routed.parallel.valid = ok;
        }

        // Anticipative rendering: carve an eligible latent interior out of the
        // graph into a lane pre-rendered ahead of the deadline, and prepare the
        // live-path splice (a skip mask over the routed plan + a map from each lane
        // output channel to the interior boundary-source output slot it fills).
        // Requires the canonical routed snapshot (the splice runs on that path).
        // Recomputed every compile: with no active anticipation no node is forced
        // exterior, so the counter must read 0 rather than keep a prior value.
        transport_suppressed_for_anticipation_.store(0, std::memory_order_relaxed);
        if (mode == CompileMode::Normal &&
            cg->routed.serial.valid &&
            anticipation_enabled_.load(std::memory_order_relaxed) &&
            max_block_size > 0) {
            const auto eligibility =
                analyze_anticipation_eligibility(nodes_, connections_);
            // Record how many transport-sensitive nodes anticipation forced
            // exterior: each such node was seeded TransportSensitive above and so
            // is excluded from the interior, running live to observe the host
            // transport. This is the repurposed meaning of
            // transport_suppressed_for_anticipation() — no longer "transport
            // dropped per block" (transport now stays live; the masked interior
            // is transport-insensitive by construction).
            std::uint64_t transport_forced_exterior = 0;
            for (const auto& n : nodes_) {
                if (n.transport_sensitive) ++transport_forced_exterior;
            }
            transport_suppressed_for_anticipation_.store(transport_forced_exterior,
                                                         std::memory_order_relaxed);
            const auto partition =
                build_anticipation_partition(nodes_, connections_, eligibility);
            const auto subgraph =
                build_anticipation_subgraph(nodes_, connections_, partition);
            if (subgraph.renders_anything()) {
                CompiledGraph& cgr = *cg;
                constexpr int kLeadBlocks = 4;
                const bool prepared = cg->anticipation.lane.prepare(
                    subgraph,
                    [&cgr](NodeId id) -> std::atomic<float>* {
                        auto it = cgr.runtime.find(id);
                        return it == cgr.runtime.end() ? nullptr : it->second.gain.get();
                    },
                    [&cgr](NodeId id) -> PluginSlot* {
                        auto it = cgr.plugins.find(id);
                        return it == cgr.plugins.end() ? nullptr : it->second.get();
                    },
                    sample_rate, max_block_size, kLeadBlocks,
                    [&cgr](NodeId id) -> ParameterEventInjectionBinding {
                        auto it = cgr.runtime.find(id);
                        if (it == cgr.runtime.end()
                            || !it->second.parameter_input_mailbox) {
                            return {};
                        }
                        return {
                            .user_data = &it->second,
                            .append = &SignalGraph::append_parameter_mailbox_events_,
                            .sequence_seen =
                                &it->second.parameter_input_mailbox->sequence_seen,
                        };
                    });
                if (prepared) {
                    // Map each interior node id -> its dense index in the ROUTED
                    // plan, to build the skip mask and resolve boundary output slots.
                    const auto& rplan = cg->routed.serial.snapshot.plan();
                    const auto& rassign = cg->routed.serial.snapshot.buffer_assignment();
                    auto routed_index = [&rplan](NodeId id) -> std::uint32_t {
                        for (std::uint32_t i = 0; i < rplan.nodes.size(); ++i) {
                            if (rplan.nodes[i].id == id) return i;
                        }
                        return 0xFFFFFFFFu;
                    };
                    cg->anticipation.skip_mask.assign(rplan.nodes.size(), 0);
                    bool map_ok = true;
                    for (const auto idx : partition.interior_nodes) {
                        const std::uint32_t ri = routed_index(nodes_[idx].id);
                        if (ri == 0xFFFFFFFFu) { map_ok = false; break; }
                        cg->anticipation.skip_mask[ri] = 1;
                    }
                    cg->anticipation.prefill.clear();
                    for (std::uint32_t ch = 0; map_ok && ch < subgraph.outputs.size();
                         ++ch) {
                        const auto& out = subgraph.outputs[ch];
                        const std::uint32_t ri = routed_index(out.source_node);
                        if (ri == 0xFFFFFFFFu) { map_ok = false; break; }
                        const std::uint32_t slot =
                            rassign.nodes[ri].output_base + out.source_port;
                        cg->anticipation.prefill.push_back({ch, slot});
                    }
                    if (map_ok) {
                        cg->anticipation.consume_scratch.assign(
                            subgraph.outputs.size(),
                            std::vector<float>(static_cast<std::size_t>(max_block_size),
                                               0.0f));
                        cg->anticipation.consume_ptrs.clear();
                        for (auto& c : cg->anticipation.consume_scratch) {
                            cg->anticipation.consume_ptrs.push_back(c.data());
                        }
                        cg->anticipation.valid = true;
                    }
                }
            }
        }
    }
    // M5: a SwapNoAnticipation compile must never have built an anticipation lane.
    assert(mode == CompileMode::Normal || !cg->anticipation.valid);
    return cg;
}

int SignalGraph::pump_anticipation(int max_blocks) {
    if (!anticipation_enabled_.load(std::memory_order_relaxed)) return 0;
    // Single-producer guard: a concurrent or reentrant pump degrades to a no-op
    // rather than corrupting the lane's unsynchronized executor/pool/scratch.
    if (anticipation_pump_busy_.exchange(true, std::memory_order_acquire)) return 0;
    // Pin the live snapshot like a process() reader (the same RCU handshake
    // Slot::wait_and_clear waits on) so the CompiledGraph object can't be freed
    // while we render its lane. render_ahead is bounded (<= max_blocks), so this
    // never holds the reader count long enough to stall a re-prepare materially.
    //
    // CONTRACT (host-enforced, not provable here): the host MUST stop calling
    // pump_anticipation AND join its producer thread before any prepare()/graph
    // mutation. The reader-pin only keeps the CompiledGraph object alive — it does
    // NOT make the shared PluginSlot instances exclusive, and prepare() reinitializes
    // those same instances (n.plugin->prepare) and builds a new lane over them. A
    // pump concurrent with prepare() would be a data race on the plugin state. This
    // mirrors the existing "no process() concurrent with prepare()" contract.
    int rendered = 0;
    {
        // The pin is released by the guard's destructor before the busy flag
        // clears, so a prepare() waiting on the reader drain cannot observe a
        // stale pin from a pump that has already finished.
        auto read_guard = live_slot_.read();
        if (auto* cg = read_guard.get()) {
            if (cg->anticipation.valid && max_blocks > 0) {
                rendered = cg->anticipation.lane.render_ahead(max_blocks);
            }
        }
    }
    anticipation_pump_busy_.store(false, std::memory_order_release);
    return rendered;
}

// Edit-time plugin parameter list (see header). Cached copy when prepared, else
// the live slot — so a connect during a swap-edit avoids a live parameters() call
// racing process() on that slot.
std::vector<HostParamInfo>
SignalGraph::cached_or_live_params_locked_(const GraphNode& n) const {
    assert_graph_mutation_locked_();
    const auto it = prepared_plugin_meta_.find(n.id);
    if (it != prepared_plugin_meta_.end()) return it->second.parameters;
    return n.plugin ? n.plugin->parameters() : std::vector<HostParamInfo>{};
}

// Shared preflight for prepare() and (2.2b) prepare_swap(): PURE validation over
// the current nodes_/connections_/limits — the generated-graph limit checks plus
// the audio-rate automation event-capacity gate. Mutates no live state, so
// prepare_swap() can run it before its reinit-free predicate WITHOUT silencing the
// live snapshot (unlike prepare()'s destructive null-first prologue, which stays in
// prepare()). Caller holds graph_mutation_mutex_. Returns false (with a logged
// reason) on any rejection; true if the graph passes every gate.
bool SignalGraph::preflight_locked_(int max_block_size) {
    assert_graph_mutation_locked_();
    const auto generated_validation = validate_generated_graph(max_block_size);
    switch (generated_validation.reason) {
    case GeneratedGraphValidationRejectReason::None:
        break;
    case GeneratedGraphValidationRejectReason::InvalidBlockSize:
        return false;
    case GeneratedGraphValidationRejectReason::MaxBlockSizeExceeded:
        runtime::log_error(
            "SignalGraph: max block size {} exceeds configured limit {}",
            generated_validation.actual,
            generated_validation.limit);
        return false;
    case GeneratedGraphValidationRejectReason::NodeLimitExceeded:
        runtime::log_error(
            "SignalGraph: node count {} exceeds configured limit {}",
            generated_validation.actual,
            generated_validation.limit);
        return false;
    case GeneratedGraphValidationRejectReason::ConnectionLimitExceeded:
        runtime::log_error(
            "SignalGraph: connection count {} exceeds configured limit {}",
            generated_validation.actual,
            generated_validation.limit);
        return false;
    case GeneratedGraphValidationRejectReason::PortLimitExceeded:
        runtime::log_error(
            "SignalGraph: port count {} exceeds configured limit {}",
            generated_validation.actual,
            generated_validation.limit);
        return false;
    case GeneratedGraphValidationRejectReason::EstimatedWorkExceeded:
        runtime::log_error(
            "SignalGraph: estimated work units {} exceed configured limit {}",
            generated_validation.actual,
            generated_validation.limit);
        return false;
    }

    std::unordered_map<NodeId, std::vector<uint32_t>> sparse_params_by_node;
    std::unordered_map<NodeId, std::vector<uint32_t>> audio_rate_params_by_node;
    auto add_unique_param = [](std::vector<uint32_t>& params, uint32_t param_id) {
        if (std::find(params.begin(), params.end(), param_id) == params.end()) {
            params.push_back(param_id);
        }
    };
    for (const auto& c : connections_) {
        if (c.automation) {
            add_unique_param(sparse_params_by_node[c.dest_node], c.automation_param_id);
        }
        if (c.audio_rate_modulation) {
            add_unique_param(audio_rate_params_by_node[c.dest_node], c.automation_param_id);
        }
    }
    for (const auto& [node_id, audio_rate_params] : audio_rate_params_by_node) {
        const auto sparse_it = sparse_params_by_node.find(node_id);
        const size_t sparse_count = sparse_it == sparse_params_by_node.end()
            ? 0
            : sparse_it->second.size();
        const size_t required_events =
            audio_rate_params.size() * static_cast<size_t>(max_block_size)
            + sparse_count * 2;
        if (required_events > ParameterEventQueue::kCapacity) {
            runtime::log_error(
                "SignalGraph: audio-rate modulation for node {} requires {} parameter events (capacity {})",
                node_id,
                required_events,
                ParameterEventQueue::kCapacity);
            return false;
        }
    }
    return true;
}

bool SignalGraph::prepare(double sample_rate, int max_block_size) {
    // Serialize the ENTIRE prepare against concurrent control-thread mutators
    // (set_node_gain / add_*/remove_node, which all run on the UI thread). The
    // lock covers two distinct shared surfaces:
    //   1. The source topology — nodes_ iteration + GraphNode plain-field reads,
    //      including GraphNode::gain in compile_() — vs the mutators' writes.
    //   2. The snapshot-publication state (live_slot_)
    //      mutated by the prologue's retire_snapshot_ + the epilogue's
    //      publish/prune, which a concurrent mutator's invalidate_live_locked_() also
    //      touches. Those were previously single-control-thread-owned; with a
    //      second control thread editing the graph they must be serialized too.
    //
    // Deadlock-free: prepare() only ever drives the NON-blocking
    // Slot::reclaim_if_quiescent() (never the blocking Slot::wait_and_clear),
    // and the one place a thread holds a Slot reader pin AND wants this
    // mutex — set_node_gain() — releases the mutex before pinning, so this lock can
    // never invert order with the reader-drain handshake.
    GraphMutationLock mutation_lock(*this);

    cancel_swap_edit_locked_();

    live_slot_.unpublish();
    total_latency_samples_.store(0, std::memory_order_relaxed);
    clear_prepared_stats_locked_();

    // Generated-graph limits + audio-rate automation event-capacity gate (H3 —
    // shared with prepare_swap()). prepare() has already nulled the live snapshot
    // above, so a preflight failure here leaves the graph silent (existing
    // behavior); prepare_swap() runs the same preflight BEFORE any mutation.
    if (!preflight_locked_(max_block_size)) return false;

    // Prepare each plugin slot first (pre-compile step). Immediately capture
    // each slot's metadata (params/latency/transport) into prepared_plugin_meta_
    // so compile_() and the executor-routing build read the cache instead of
    // calling live PluginSlot metadata methods — the contract the no-silence
    // swap (2.2b) relies on (H2). Safe to read here: null-first prepare, mutation
    // lock held, no concurrent process() on these instances.
    prepared_plugin_meta_.clear();
    for (auto& n : nodes_) {
        if (n.plugin) {
            if (!n.plugin->prepare(sample_rate, max_block_size)) {
                runtime::log_error("SignalGraph: failed to prepare plugin '{}'", n.name);
                return false;
            }
            prepared_plugin_meta_[n.id] = PreparedPluginMetadata{
                n.plugin->parameters(),
                std::max(0, n.plugin->latency_samples()),
                n.plugin->wants_transport()};
        }
    }

    // Create/prepare stateful custom-node instances on this UI thread before
    // the snapshot is published, mirroring the plugin step above. A
    // freshly-loaded state blob is applied exactly once via load_state.
    for (auto& n : nodes_) {
        if (n.type != NodeType::Custom) continue;
        const CustomNodeType* type =
            custom_node_type(n.custom_type_id, n.custom_type_version);
        if (type == nullptr || !type->create
            || !custom_type_matches_node_shape(*type, n)) {
            continue;  // stateless / unresolved / shape-mismatch: no instance
        }
        if (!n.custom_instance) {
            n.custom_instance = make_custom_instance(*type);
        }
        if (n.custom_instance) {
            if (n.custom_state_pending && type->load_state) {
                type->load_state(n.custom_instance.get(), n.custom_state_blob);
                n.custom_state_pending = false;
            }
            if (type->prepare) {
                type->prepare(n.custom_instance.get(), sample_rate, max_block_size);
            }
        }
    }

    auto cg = compile_(sample_rate, max_block_size);
    total_latency_samples_.store(cg->total_latency_samples, std::memory_order_relaxed);
    publish_prepared_stats_locked_(*cg);
    live_slot_.publish(std::move(cg));
    return true;
}

void SignalGraph::set_limits(GraphLimits limits) {
    // Mutates limits_ (read by validate_generated_graph under the lock) and drives
    // invalidate_live_locked_(); serialize against a concurrent prepare()/mutator.
    GraphMutationLock mutation_lock(*this);
    cancel_swap_edit_locked_();
    limits_ = limits;
    invalidate_live_locked_();
}

std::size_t SignalGraph::total_declared_ports_locked_() const {
    std::size_t port_count = 0;
    for (const auto& n : nodes_) {
        port_count += static_cast<std::size_t>(std::max(0, n.num_input_ports));
        port_count += static_cast<std::size_t>(std::max(0, n.num_output_ports));
    }
    return port_count;
}

std::size_t
SignalGraph::estimate_generated_graph_work_units(int max_block_size) const {
    if (max_block_size <= 0) return 0;

    const std::size_t block =
        static_cast<std::size_t>(max_block_size);
    const std::size_t port_count = total_declared_ports_locked_();
    std::size_t dense_edges = 0;
    std::size_t sparse_edges = 0;
    for (const auto& c : connections_) {
        if (c.audio_rate_modulation) ++dense_edges;
        if (c.automation && !c.audio_rate_modulation) ++sparse_edges;
    }

    std::size_t work = 0;
    work = saturating_add(work, saturating_mul(nodes_.size(), 16));
    work = saturating_add(work, saturating_mul(connections_.size(), 8));
    work = saturating_add(work, saturating_mul(port_count, block));
    work = saturating_add(work, saturating_mul(dense_edges, block));
    work = saturating_add(work, saturating_mul(sparse_edges, 2));
    return work;
}

SignalGraph::GeneratedGraphValidation
SignalGraph::validate_generated_graph(int max_block_size) const {
    if (max_block_size <= 0) {
        return {
            false,
            GeneratedGraphValidationRejectReason::InvalidBlockSize,
            max_block_size < 0 ? 0 : static_cast<std::size_t>(max_block_size),
            1,
        };
    }
    if (limits_.max_block_size > 0 && max_block_size > limits_.max_block_size) {
        return {
            false,
            GeneratedGraphValidationRejectReason::MaxBlockSizeExceeded,
            static_cast<std::size_t>(max_block_size),
            static_cast<std::size_t>(limits_.max_block_size),
        };
    }
    if (nodes_.size() > limits_.max_nodes) {
        return {
            false,
            GeneratedGraphValidationRejectReason::NodeLimitExceeded,
            nodes_.size(),
            limits_.max_nodes,
        };
    }
    if (connections_.size() > limits_.max_connections) {
        return {
            false,
            GeneratedGraphValidationRejectReason::ConnectionLimitExceeded,
            connections_.size(),
            limits_.max_connections,
        };
    }
    const std::size_t port_count = total_declared_ports_locked_();
    if (port_count > limits_.max_ports) {
        return {
            false,
            GeneratedGraphValidationRejectReason::PortLimitExceeded,
            port_count,
            limits_.max_ports,
        };
    }
    const std::size_t estimated_work =
        estimate_generated_graph_work_units(max_block_size);
    if (limits_.max_estimated_work_units > 0
        && estimated_work > limits_.max_estimated_work_units) {
        return {
            false,
            GeneratedGraphValidationRejectReason::EstimatedWorkExceeded,
            estimated_work,
            limits_.max_estimated_work_units,
        };
    }
    return {};
}

void SignalGraph::release() {
    // Serialize against concurrent control-thread mutators / prepare() for the
    // same two surfaces prepare() guards: the nodes_ iteration below and the
    // snapshot-publication state (live_slot_).
    // Slot::wait_and_clear() blocks on the reader count while holding this
    // mutex; that is deadlock-free because the only thread that holds a reader pin
    // AND wants this mutex (set_node_gain) releases the mutex before pinning, and
    // the pure-snapshot readers (inject_midi / extract_midi / node_latency_samples)
    // never take this mutex at all.
    GraphMutationLock mutation_lock(*this);

    cancel_swap_edit_locked_();

    live_slot_.unpublish();
    live_slot_.wait_and_clear();

    for (auto& n : nodes_) if (n.plugin) n.plugin->release();
    // Release stateful custom instances on the UI thread, mirroring the plugin
    // release above. The instance object stays alive until its snapshots also
    // drop; release() just lets the type free scratch.
    for (auto& n : nodes_) {
        if (n.type != NodeType::Custom || !n.custom_instance) continue;
        if (const auto* type =
                custom_node_type(n.custom_type_id, n.custom_type_version);
            type && type->release) {
            type->release(n.custom_instance.get());
        }
    }
    total_latency_samples_.store(0, std::memory_order_relaxed);
    clear_prepared_stats_locked_();
}

std::vector<uint8_t> SignalGraph::custom_node_state(NodeId id) const {
    // Reads nodes_ + GraphNode custom fields; serialize against a concurrent
    // mutator/prepare. custom_node_type() is a lock-free helper used under it.
    GraphMutationLock mutation_lock(*this);
    for (const auto& n : nodes_) {
        if (n.id != id) continue;
        if (n.type != NodeType::Custom) return {};
        // Prefer the live instance's current state; fall back to the stored blob
        // (e.g. unresolved nodes, or before the first prepare()).
        if (n.custom_instance) {
            if (const auto* type =
                    custom_node_type(n.custom_type_id, n.custom_type_version);
                type && type->save_state) {
                return type->save_state(n.custom_instance.get());
            }
        }
        return n.custom_state_blob;
    }
    return {};
}

bool SignalGraph::set_custom_node_state(NodeId id,
                                        const std::vector<uint8_t>& bytes) {
    // Writes GraphNode custom fields + invalidate_live_locked_(); serialize against a
    // concurrent mutator/prepare.
    GraphMutationLock mutation_lock(*this);
    cancel_swap_edit_locked_();
    for (auto& n : nodes_) {
        if (n.id != id) continue;
        if (n.type != NodeType::Custom) return false;
        n.custom_state_blob = bytes;
        // Apply to the live instance on the next prepare() (one-shot). The blob
        // is retained regardless, so it survives even when the type is
        // unresolved and is re-emitted on the next serialize.
        n.custom_state_pending = true;
        invalidate_live_locked_();
        return true;
    }
    return false;
}

void SignalGraph::process(audio::BufferView<float>& output,
                          const audio::BufferView<const float>& input,
                          int num_samples) {
    process_impl(output, input, num_samples, /*transport=*/nullptr);
}

void SignalGraph::process(audio::BufferView<float>& output,
                          const audio::BufferView<const float>& input,
                          int num_samples,
                          const format::ProcessContext& transport) {
    process_impl(output, input, num_samples, &transport);
}

void SignalGraph::process_impl(audio::BufferView<float>& output,
                               const audio::BufferView<const float>& input,
                               int num_samples,
                               const format::ProcessContext* transport) {
    // See runtime::Slot: its reader count and the raw snapshot
    // pointer form one RCU-style lifetime handshake. Slot::ReadGuard is a
    // private nested struct (signal_graph.hpp) so the control-thread snapshot
    // readers can pin the same way.
    auto read_guard = live_slot_.read();

    auto* cg = read_guard.get();
    // Negative or zero block sizes mean "nothing to do" — return without
    // touching output (a memset with size_t(negative) wraps to a huge size).
    if (num_samples <= 0) return;
    if (!cg || num_samples > cg->max_block_size) {
        for (std::size_t c = 0; c < output.num_channels(); ++c)
            std::memset(output.channel_ptr(c), 0,
                        sizeof(float) * static_cast<size_t>(num_samples));
        return;
    }

    // Bracket the whole block with the graph-level load measurer (RT-safe: begin()/
    // end() are relaxed-atomic timestamps, no alloc/lock). The RAII guard's end()
    // covers every return path below. graph_load() reads this for live-swap admission.
    if (graph_load_) graph_load_->begin(num_samples, static_cast<float>(cg->sample_rate));

    // Record one live-DSP telemetry block per process() call, path-agnostic: the
    // routed executor and the legacy walk BOTH time each node into its persistent
    // AudioProcessLoadMeasurer (node_load_) and the whole block into graph_load_.
    // This guard reads those already-populated measurers after the block and pushes
    // a single fixed-slot record — so per-node p50/p95/p99 + jitter + over-budget
    // attribution work on every execution path with no per-node hook in either the
    // executor or the walk. Declared BEFORE graph_load_end_guard so it destructs
    // AFTER it (reverse order): graph_load_->end() has stamped this block's graph
    // elapsed by the time this runs. Inactive at one-branch cost when telemetry is
    // off; the record path is allocation-free (pre-sized scratch + drop-on-full ring).
    struct TelemetryRecordGuard {
        CompiledGraph* cg;
        audio::AudioProcessLoadMeasurer* graph_measurer;
        int num_samples;
        ~TelemetryRecordGuard() {
            auto& store = cg->live_dsp_telemetry;
            if (!store.enabled() || !store.prepared()) return;
            std::int64_t* scratch = store.external_record_scratch();
            if (scratch == nullptr) return;
            const std::uint32_t n = store.node_count();
            for (std::uint32_t i = 0; i < n && i < cg->ordered_runtime.size(); ++i) {
                auto* rt = cg->ordered_runtime[i].runtime;
                auto* m = rt ? rt->load : nullptr;
                scratch[i] = m ? m->last_elapsed_ns() : 0;
            }
            const std::int64_t graph_ns =
                graph_measurer ? graph_measurer->last_elapsed_ns() : 0;
            store.inject_block(std::span<const std::int64_t>(scratch, n), graph_ns,
                               static_cast<std::uint32_t>(num_samples), cg->sample_rate);
        }
    } telemetry_record_guard{cg, graph_load_.get(), num_samples};

    struct GraphLoadEndGuard {
        audio::AudioProcessLoadMeasurer* m;
        ~GraphLoadEndGuard() {
            if (m) m->end();
        }
    } graph_load_end_guard{graph_load_.get()};

    // Routed dispatch (opt-in): try the levelized PARALLEL executor first (if
    // enabled), then the SERIAL executor, then fall through to the legacy walk.
    // Both routed paths run GraphRuntimeExecutor (which zeroes the output bus and
    // accumulates AudioOutput itself, so a successful routed call returns before
    // the legacy zero+walk below) and SHARE the MIDI mailbox bridge (the MIDI
    // scratch + MidiInput/Output node lists are shared — identical plan). The
    // dispatch stays inside the Slot reader-pin, so `cg` (snapshots, pools, gain
    // atomics) is pinned for the whole call. Output is bit-identical across paths.
    {
        const auto frames32 = static_cast<std::uint32_t>(num_samples);
        const bool has_midi = cg->routed.midi.node_count() > 0;
        const bool has_automation = cg->routed.automation.node_count() > 0;

        pulp::format::BusBufferSet buses;
        const bool buses_ok =
            buses.add_input("main", input, pulp::format::BusRole::Main) &&
            buses.add_output("main", output, pulp::format::BusRole::Main);
        if (buses_ok) {
            // Anticipation safety (per-node, not blanket): a transport-sensitive
            // node opts in via GraphNode::transport_sensitive, which seeds
            // AnticipationExclusion::TransportSensitive and so excludes that node
            // (and its downstream cone) from the ahead-rendered interior. Every
            // node that IS ahead-rendered is therefore transport-insensitive by
            // construction and ignores block.transport. So the live transport can
            // stay populated even while anticipation is active: forwarding it is
            // inert for the masked interior, and the transport-sensitive nodes
            // that need it always run live/exterior. (Resolved at compile; the
            // count of nodes forced exterior is in
            // transport_suppressed_for_anticipation().)
            pulp::format::ProcessBlock block;
            block.sample_rate = cg->sample_rate;
            block.frame_count = frames32;
            block.buses = &buses;
            if (transport != nullptr) {
                // Deliver transport + process_mode + render_speed_hint to
                // Processors via *block.transport. block.sample_rate stays sourced
                // from cg (the prepared rate is authoritative). render_speed is left
                // at 1.0: render_speed_hint is a categorical hint that reaches
                // Processors through *block.transport, not a numeric multiplier.
                block.transport = transport;
                block.mode = transport->process_mode;
            }

            // Run one routed path with the shared MIDI mailbox bridge around it.
            // `run` returns the executor result; this returns true iff routing
            // succeeded (took the path). On failure the consumed MIDI sequences
            // are NOT committed, so a fallback path re-consumes the same block.
            auto dispatch_routed = [&](auto&& run) -> bool {
                if (!block.validate()) return false;
                reset_plugin_parameter_event_sequences(cg->routed.serial.plugin_ctx);
                reset_plugin_parameter_event_sequences(
                    cg->routed.parallel.plugin_ctx);
                if (has_midi) {
                    for (auto& mi : cg->routed.midi_inputs) {
                        mi.pending_seq = 0;
                        midi::MidiBuffer* out_buf = cg->routed.midi.out(mi.plan_index);
                        if (out_buf == nullptr) continue;
                        clear_midi_block(*out_buf);
                        cg->routed.midi.set_out_incomplete(mi.plan_index, false);
                        auto rt_it = cg->runtime.find(mi.id);
                        if (rt_it == cg->runtime.end() ||
                            !rt_it->second.midi_input_mailbox) {
                            continue;
                        }
                        const auto& injected =
                            rt_it->second.midi_input_mailbox->published.read();
                        if (injected.sequence != 0 &&
                            injected.sequence != rt_it->second.midi_input_sequence_seen) {
                            cg->routed.midi.set_out_incomplete(
                                mi.plan_index, !injected.copy_to_midi(*out_buf));
                            mi.pending_seq = injected.sequence;
                        }
                    }
                }
                if (!run().ok()) return false;
                commit_plugin_parameter_event_sequences(cg->routed.serial.plugin_ctx);
                commit_plugin_parameter_event_sequences(
                    cg->routed.parallel.plugin_ctx);
                if (has_midi) {
                    for (const auto& mi : cg->routed.midi_inputs) {
                        if (mi.pending_seq == 0) continue;
                        auto rt_it = cg->runtime.find(mi.id);
                        if (rt_it != cg->runtime.end()) {
                            rt_it->second.midi_input_sequence_seen = mi.pending_seq;
                        }
                    }
                    for (const auto& mo : cg->routed.midi_outputs) {
                        midi::MidiBuffer* in_buf = cg->routed.midi.in(mo.plan_index);
                        auto rt_it = cg->runtime.find(mo.id);
                        if (in_buf == nullptr || rt_it == cg->runtime.end() ||
                            !rt_it->second.midi_output_mailbox) {
                            continue;
                        }
                        cg->midi_publish_scratch.set_from_midi(
                            *in_buf, 0, cg->routed.midi.in_incomplete(mo.plan_index));
                        rt_it->second.midi_output_mailbox->write(cg->midi_publish_scratch);
                    }
                }
                return true;
            };

            // worker_pool_.running() is a plain flag read here; it is safe without
            // a drain handshake only because running_ is flipped false exactly
            // once, by ~GraphRuntimeWorkerPool, when no audio-thread reader exists
            // (see the compile_ start invariant). Each dispatch is idempotent: it
            // re-clears the MIDI ingress buffers and does not commit consumed
            // sequences until run() succeeds, and every executor zeroes the output
            // bus before accumulating — so a parallel attempt that returns false
            // (a node failed, or it does not fit) can fall through to the serial
            // executor and then the legacy walk re-rendering the same block with
            // no double-consumed MIDI and no doubled output.
            // Anticipative rendering (tried first): the lane has pre-rendered the
            // interior off the audio thread. Consume one block, fill the interior
            // boundary-source output slots with it (or zero them on an underrun /
            // block-size mismatch — NEVER re-run the interior, whose plugin state
            // the producer owns), then run the routed walk with the interior masked
            // so only the exterior runs live. Uses the serial routed snapshot.
            if (anticipation_enabled_.load(std::memory_order_relaxed) &&
                cg->anticipation.valid &&
                cg->anticipation.lane.output_channels() ==
                    cg->anticipation.prefill.size()) {
                // STRUCTURALLY TERMINAL once anticipation is valid: the interior's
                // plugin state is advanced solely by the producer (pump_anticipation),
                // so the live path must NEVER run the interior — not via a fallback,
                // and not even when the pool can't fit the block. The fits() check is
                // INSIDE the branch (not an entry gate) precisely so a future change
                // that made it false could not let control fall through to the
                // parallel/legacy paths below, which would run the masked interior
                // live and double-advance producer-owned state. Worst case here is a
                // silent block, never a double-render.
                bool ok = false;
                if (cg->routed.serial.pool.fits(cg->routed.serial.snapshot, frames32)) {
                    const bool size_ok =
                        frames32 == static_cast<std::uint32_t>(
                                        cg->anticipation.lane.block_frames());
                    bool hit = false;
                    if (size_ok) {
                        pulp::audio::BufferView<float> cap(
                            cg->anticipation.consume_ptrs.data(),
                            cg->anticipation.consume_ptrs.size(), frames32);
                        hit = cg->anticipation.lane.consume(cap);
                    }
                    for (const auto& pf : cg->anticipation.prefill) {
                        float* dst = cg->routed.serial.pool.slot_data(pf.slot);
                        if (dst == nullptr) continue;
                        if (hit) {
                            std::copy_n(cg->anticipation.consume_ptrs[pf.out_channel],
                                        num_samples, dst);
                        } else {
                            std::fill_n(dst, num_samples, 0.0f);
                        }
                    }
                    ok = dispatch_routed([&] {
                        return executor_.process_routed(
                            block, cg->routed.serial.snapshot, cg->routed.serial.pool,
                            has_midi ? &cg->routed.midi : nullptr,
                            has_automation ? &cg->routed.automation : nullptr, {}, {},
                            {}, {}, cg->anticipation.skip_mask);
                    });
                }
                if (!ok) {
                    for (std::size_t c = 0; c < output.num_channels(); ++c) {
                        std::memset(output.channel_ptr(c), 0,
                                    sizeof(float) * static_cast<std::size_t>(num_samples));
                    }
                }
                return;
            }

            // Reached only when anticipation is NOT valid (the branch above is
            // terminal whenever it is) — REQUIRED for safety: the parallel and
            // legacy paths run every node, including any interior the producer
            // owns, so they must never execute while anticipation is active.
            // Observability for a SILENT degradation the parity test cannot see:
            // when a routed path is ELIGIBLE (enabled + a valid routed snapshot +
            // the pool fits) but its dispatch returns false, control falls through
            // to the legacy walk. Because the walk is BOTH the oracle and the
            // fallback, that produces no divergence — so an eligible graph that
            // stopped routing would be invisible. Flag it: count it (relaxed,
            // RT-safe) and warn in debug builds. A normal fallback (routing
            // disabled, ineligible, or build failure → routed.serial.valid false) does
            // NOT set this, so the counter isolates the should-have-routed case.
            bool routed_eligible_dispatch_failed = false;

            if (parallel_routing_enabled_.load(std::memory_order_relaxed) &&
                cg->routed.parallel.valid && worker_pool_.running() &&
                cg->routed.parallel.pool.fits(cg->routed.parallel.snapshot, frames32)) {
                if (dispatch_routed([&] {
                        return executor_.process_parallel(
                            block, cg->routed.parallel.snapshot,
                            cg->routed.parallel.levelization, cg->routed.parallel.pool,
                            worker_pool_, has_midi ? &cg->routed.midi : nullptr,
                            has_automation ? &cg->routed.automation : nullptr);
                    })) {
                    return;
                }
                routed_eligible_dispatch_failed = true;  // eligible but did not route
            }

            if (canonical_executor_routing_enabled_.load(std::memory_order_relaxed) &&
                cg->routed.serial.valid &&
                cg->routed.serial.pool.fits(cg->routed.serial.snapshot, frames32)) {
                if (dispatch_routed([&] {
                        return executor_.process_routed(
                            block, cg->routed.serial.snapshot, cg->routed.serial.pool,
                            has_midi ? &cg->routed.midi : nullptr,
                            has_automation ? &cg->routed.automation : nullptr);
                    })) {
                    return;
                }
                routed_eligible_dispatch_failed = true;  // eligible but did not route
            }

            if (routed_eligible_dispatch_failed) {
                const std::uint64_t prev =
                    routed_walk_fallbacks_.fetch_add(1, std::memory_order_relaxed);
                (void)prev;
#ifndef NDEBUG
                // Warn ONCE per graph (the counter carries the full tally) — the
                // log path is not RT-safe, so don't repeat it every block.
                if (prev == 0) {
                    runtime::log_warn(
                        "SignalGraph: routed dispatch failed for an eligible graph; "
                        "falling back to the legacy walk (see routed_walk_fallbacks())");
                }
#endif
            }
        }
        // Any setup failure / disabled path falls through to the legacy walk.
    }

    // No routed path took this block — run the legacy serial reference walk,
    // the hand-maintained bit-exact oracle/fallback (see
    // signal_graph_reference_walk.cpp).
    run_reference_walk_(output, input, num_samples, cg);
}

void SignalGraph::clear() {
    // Wipes nodes_/connections_ + invalidate_live_locked_(); serialize against a
    // concurrent mutator/prepare. invalidate_live_locked_() drives only the non-blocking
    // prune, so holding the mutex across it is deadlock-free.
    GraphMutationLock mutation_lock(*this);
    cancel_swap_edit_locked_();
    connections_.clear();
    nodes_.clear();
    next_id_ = 1;
    invalidate_live_locked_();
}

bool SignalGraph::set_node_gain(NodeId id, float linear_gain) {
    // Write the UI-thread-owned scalar on GraphNode so it survives future
    // compile_() calls. Also reflect into the live snapshot's runtime through
    // a per-runtime atomic so the change takes effect without a re-prepare.
    //
    // The GraphNode::gain write + the node() scan of nodes_ are serialized under
    // graph_mutation_mutex_ against compile_()'s read of GraphNode::gain and its
    // nodes_ iteration (this API runs on the UI thread, prepare()/compile_() on a
    // host thread). The lock is RELEASED before the Slot-pinned
    // snapshot reflection below, so it never nests inside the reader-pin / RCU
    // drain mechanism and cannot invert lock order with release()'s reader wait.
    {
        GraphMutationLock mutation_lock(*this);
        auto* n = node_mut_locked_(id);
        if (!n) return false;
        n->gain = linear_gain;
    }
    // Pin the live snapshot around the load + the per-runtime gain store: this
    // UI-thread-owned API is not the prepare/release thread, so without the
    // guard a concurrent prepare()/release() could retire+free `cg` between the
    // load and the store (use-after-free).
    auto read_guard = live_slot_.read();
    auto* cg = read_guard.get();
    if (cg) {
        auto it = cg->runtime.find(id);
        if (it != cg->runtime.end() && it->second.gain) {
            it->second.gain->store(linear_gain, std::memory_order_relaxed);
        }
    }
    return true;
}

float SignalGraph::node_gain(NodeId id) const {
    // Read counterpart of set_node_gain(): the node() scan of nodes_ and the
    // GraphNode::gain read are serialized under graph_mutation_mutex_ against
    // compile_()'s nodes_ iteration / gain read and concurrent set_node_gain().
    GraphMutationLock mutation_lock(*this);
    auto* n = node(id);
    if (!n) return 1.0f;
    return n->gain;
}

// Drag-add helper.
NodeId add_plugin_node_from_drop(SignalGraph& graph,
                                 const PluginInfo& info,
                                 bool* loaded_out)
{
    // Try the live-load path first. add_plugin_node calls PluginSlot::load,
    // which may return null when the bundle is missing or refuses to load.
    const NodeId id = graph.add_plugin_node(info);
    if (auto* n = graph.node(id); n && n->plugin) {
        if (loaded_out) *loaded_out = true;
        return id;
    }

    // Live-load failed — remove the half-loaded node and create an
    // unresolved placeholder so the graph still carries the user's intent.
    graph.remove_node(id);
    if (loaded_out) *loaded_out = false;
    return graph.add_unresolved_plugin_node(
        info, info.num_inputs, info.num_outputs,
        info.name.empty() ? info.path : info.name);
}

} // namespace pulp::host
