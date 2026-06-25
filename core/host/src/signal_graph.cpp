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
#include <pulp/host/signal_graph_executor_routing.hpp>
#include <pulp/runtime/log.hpp>
#include <algorithm>
#include <array>
#include <queue>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <unordered_map>
#include <cstring>
#include <thread>
#include <utility>

namespace pulp::host {
namespace {

constexpr std::size_t kGraphMidiEventCapacity =
    state::ParameterEventQueue::kCapacity;
constexpr std::size_t kGraphMidiSysexCapacity = 128;
constexpr std::size_t kGraphMidiSysexPayloadCapacity = 4096;

void prepare_midi_block_storage(midi::MidiBuffer& block,
                                midi::UmpBuffer& ump) {
    block.reserve(kGraphMidiEventCapacity,
                  kGraphMidiSysexCapacity,
                  kGraphMidiSysexPayloadCapacity);
    block.set_realtime_capacity_limit(true);
    ump.reserve(kGraphMidiEventCapacity);
    ump.set_realtime_capacity_limit(true);
    block.attach_ump(&ump);
}

void clear_midi_block(midi::MidiBuffer& block) {
    block.clear();
    block.clear_sysex();
    if (auto* ump = block.ump()) ump->clear();
}

bool midi_block_has_drops(const midi::MidiBuffer& block) {
    if (block.dropped_event_count() > 0 || block.dropped_sysex_count() > 0) {
        return true;
    }
    const auto* ump = block.ump();
    return ump && ump->dropped_event_count() > 0;
}

bool copy_midi_block(const midi::MidiBuffer& src, midi::MidiBuffer& dst) {
    bool copied_all = !midi_block_has_drops(src);
    for (const auto& ev : src) {
        if (!dst.add(ev)) copied_all = false;
    }
    for (const auto& sx : src.sysex()) {
        if (sx.data.empty()) {
            if (!dst.add_sysex({}, sx.sample_offset, sx.timestamp)) {
                copied_all = false;
            }
        } else {
            if (!dst.add_sysex_copy(sx.data.data(), sx.data.size(),
                                    sx.sample_offset, sx.timestamp)) {
                copied_all = false;
            }
        }
    }
    const auto* src_ump = src.ump();
    auto* dst_ump = dst.ump();
    if (src_ump && dst_ump) {
        for (const auto& ev : *src_ump) {
            if (!dst_ump->add(ev)) copied_all = false;
        }
    } else if (src_ump && !src_ump->empty()) {
        copied_all = false;
    }
    return copied_all;
}

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

float map_modulation_sample(const Connection& c, float sample) {
    const float normalized = std::clamp(sample, 0.0f, 1.0f);
    return c.automation_range_lo
        + normalized * (c.automation_range_hi - c.automation_range_lo);
}

bool push_parameter_event(ParameterEventQueue& queue,
                          uint32_t param_id,
                          int sample_offset,
                          float value) {
    return queue.push({
        param_id,
        sample_offset,
        value,
        0,
    });
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

NodeId SignalGraph::add_input_node(int channels, const std::string& name) {
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::AudioInput;
    node.name = name;
    node.num_output_ports = channels;
    nodes_.push_back(std::move(node));
    invalidate_live_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_output_node(int channels, const std::string& name) {
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::AudioOutput;
    node.name = name;
    node.num_input_ports = channels;
    nodes_.push_back(std::move(node));
    invalidate_live_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_plugin_node(const PluginInfo& info) {
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Plugin;
    node.name = info.name;
    node.num_input_ports = info.num_inputs;
    node.num_output_ports = info.num_outputs;
    node.plugin = std::shared_ptr<PluginSlot>(PluginSlot::load(info));
    node.plugin_info = info;  // preserve identity even if load failed
    nodes_.push_back(std::move(node));
    invalidate_live_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_unresolved_plugin_node(const PluginInfo& info,
                                               int num_inputs,
                                               int num_outputs,
                                               const std::string& name) {
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
    invalidate_live_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_plugin_node(std::unique_ptr<PluginSlot> slot,
                                    int num_inputs, int num_outputs,
                                    const std::string& name) {
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
    invalidate_live_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_gain_node(const std::string& name) {
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Gain;
    node.name = name;
    node.num_input_ports = 2;
    node.num_output_ports = 2;
    nodes_.push_back(std::move(node));
    invalidate_live_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_midi_input_node(const std::string& name) {
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::MidiInput;
    node.name = name;
    node.num_output_ports = 1;
    nodes_.push_back(std::move(node));
    invalidate_live_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_midi_output_node(const std::string& name) {
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::MidiOutput;
    node.name = name;
    node.num_input_ports = 1;
    nodes_.push_back(std::move(node));
    invalidate_live_();
    return nodes_.back().id;
}

bool SignalGraph::register_custom_node_type(CustomNodeType type) {
    if (!is_valid_custom_node_type(type)) return false;
    const bool affects_existing_nodes = std::any_of(
        nodes_.begin(), nodes_.end(), [&](const GraphNode& node) {
            return node.type == NodeType::Custom
                && node.custom_type_id == type.type_id
                && node.custom_type_version == type.version;
        });
    if (type.default_name.empty()) type.default_name = type.type_id;
    const auto key = custom_node_key(type.type_id, type.version);
    custom_node_types_[key] = std::move(type);
    if (affects_existing_nodes) invalidate_live_();
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

    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Custom;
    node.name = name.empty() ? type.type_id : name;
    node.num_input_ports = num_inputs;
    node.num_output_ports = num_outputs;
    node.custom_type_id = std::move(type.type_id);
    node.custom_type_version = version;
    nodes_.push_back(std::move(node));
    invalidate_live_();
    return nodes_.back().id;
}

bool SignalGraph::remove_node(NodeId id) {
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
    invalidate_live_();
    return true;
}

bool SignalGraph::connect(NodeId source, PortIndex source_port,
                          NodeId dest, PortIndex dest_port) {
    const GraphNode* src_n = node(source);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n) return false;
    if (!has_output_port(*src_n, source_port)) return false;
    if (!has_input_port(*dst_n, dest_port)) return false;
    if (would_create_cycle(source, dest)) return false;
    Connection conn{source, source_port, dest, dest_port};
    for (auto& c : connections_) if (c == conn) return false;
    connections_.push_back(conn);
    invalidate_live_();
    return true;
}

bool SignalGraph::connect_midi(NodeId source, NodeId dest) {
    if (!node(source) || !node(dest)) return false;
    if (would_create_cycle(source, dest)) return false;
    Connection conn{source, 0, dest, 0, false, true};
    for (auto& c : connections_) if (c == conn && c.midi == conn.midi) return false;
    connections_.push_back(conn);
    invalidate_live_();
    return true;
}

bool SignalGraph::connect_sidechain(NodeId source, PortIndex source_port,
                                    NodeId dest, PortIndex dest_sidechain_port) {
    // Sidechain connections only make sense to Plugin nodes; everything
    // else (Gain, Custom, AudioOutput, ...) has no notion of a sidechain
    // bus. We reject other destinations early so callers fail loudly
    // instead of silently routing into a regular audio port.
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
    invalidate_live_();
    return true;
}

bool SignalGraph::connect_automation(NodeId src, PortIndex src_audio_port,
                                     NodeId dest, uint32_t dest_param_id,
                                     float range_lo, float range_hi,
                                     float smoothing_ms,
                                     AutomationMix mix) {
    const GraphNode* src_n = node(src);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n) return false;
    if (dst_n->type != NodeType::Plugin || !dst_n->plugin) return false;
    if (!has_output_port(*src_n, src_audio_port)) return false;

    // Reject automation edges that would introduce a cycle. Automation
    // edges contribute to topological order (the source must be processed
    // before the dest), so a back-edge here would make the graph
    // un-orderable. Use the same has_path check as connect().
    if (would_create_cycle(src, dest)) return false;

    // Parameter must exist, be automatable, and not read-only.
    bool ok_param = false;
    for (const auto& pi : dst_n->plugin->parameters()) {
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
    invalidate_live_();
    return true;
}

bool SignalGraph::connect_audio_rate_modulation(NodeId src, PortIndex src_audio_port,
                                                NodeId dest, uint32_t dest_param_id,
                                                float range_lo, float range_hi,
                                                float smoothing_ms,
                                                AutomationMix mix) {
    const GraphNode* src_n = node(src);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n) return false;
    if (dst_n->type != NodeType::Plugin || !dst_n->plugin) return false;
    if (!has_output_port(*src_n, src_audio_port)) return false;
    if (would_create_cycle(src, dest)) return false;

    bool ok_param = false;
    for (const auto& pi : dst_n->plugin->parameters()) {
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
    if (!audio_rate_modulation_lane(conn, lane)) return false;
    connections_.push_back(conn);
    invalidate_live_();
    return true;
}

bool SignalGraph::audio_rate_modulation_lane(const Connection& connection,
                                             state::ModulationLane& lane) const {
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

    for (const auto& pi : dst_n->plugin->parameters()) {
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
    auto* cg = live_raw_.load(std::memory_order_acquire);
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

bool SignalGraph::extract_midi(NodeId id, midi::MidiBuffer& out) const {
    auto* cg = live_raw_.load(std::memory_order_acquire);
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
    const GraphNode* src_n = node(source);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n) return false;
    if (!has_output_port(*src_n, source_port)) return false;
    if (!has_input_port(*dst_n, dest_port)) return false;
    Connection conn{source, source_port, dest, dest_port, true};
    for (auto& c : connections_) if (c == conn) return false;
    connections_.push_back(conn);
    invalidate_live_();
    return true;
}

bool SignalGraph::disconnect(NodeId source, PortIndex source_port,
                             NodeId dest, PortIndex dest_port) {
    Connection target{source, source_port, dest, dest_port};
    auto it = std::find(connections_.begin(), connections_.end(), target);
    if (it == connections_.end()) return false;
    connections_.erase(it);
    invalidate_live_();
    return true;
}

const GraphNode* SignalGraph::node(NodeId id) const {
    for (auto& n : nodes_) if (n.id == id) return &n;
    return nullptr;
}

bool SignalGraph::has_path(NodeId from, NodeId to) const {
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
    return has_path(dest, source);
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
    auto* n = const_cast<GraphNode*>(node(id));
    if (!n || n->type != NodeType::Plugin || !n->plugin) return false;
    n->plugin->set_parameter(param_id, value);
    return true;
}

float SignalGraph::get_node_parameter(NodeId id, uint32_t param_id) const {
    auto* n = node(id);
    if (!n || n->type != NodeType::Plugin || !n->plugin) return 0.f;
    return n->plugin->get_parameter(param_id);
}

int SignalGraph::node_latency_samples(NodeId id) const {
    const auto* cg = live_raw_.load(std::memory_order_acquire);
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
    return live_ ? live_->max_block_size : 0;
}

std::atomic<float>* SignalGraph::live_gain_atomic(NodeId id) const noexcept {
    if (!live_) return nullptr;
    auto it = live_->runtime.find(id);
    if (it == live_->runtime.end()) return nullptr;
    return it->second.gain.get();
}

PluginSlot* SignalGraph::live_plugin_slot(NodeId id) const noexcept {
    if (!live_) return nullptr;
    auto it = live_->plugins.find(id);
    if (it == live_->plugins.end()) return nullptr;
    return it->second.get();
}

std::shared_ptr<const void> SignalGraph::live_snapshot_handle() const noexcept {
    return live_;  // aliases the live CompiledGraph as an opaque keepalive
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
    std::unordered_set<NodeId> live_ids;
    live_ids.reserve(nodes_.size());
    for (const auto& n : nodes_) live_ids.insert(n.id);

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

void SignalGraph::invalidate_live_() {
    // Drop the live snapshot; process() will return silence until prepare()
    // is called again. This is the simple, safe semantic: UI-thread edits
    // always require a re-prepare before audio resumes.
    live_raw_.store(nullptr, std::memory_order_seq_cst);
    retire_snapshot_(std::move(live_));
    total_latency_samples_.store(0, std::memory_order_relaxed);
    clear_prepared_stats_();
}

void SignalGraph::clear_prepared_stats_() {
    prepared_node_count_.store(0, std::memory_order_relaxed);
    prepared_ordered_node_count_.store(0, std::memory_order_relaxed);
    prepared_connection_count_.store(0, std::memory_order_relaxed);
    prepared_total_ports_.store(0, std::memory_order_relaxed);
    prepared_max_block_size_.store(0, std::memory_order_relaxed);
    prepared_node_audio_buffer_bytes_.store(0, std::memory_order_relaxed);
    prepared_automation_buffer_bytes_.store(0, std::memory_order_relaxed);
    prepared_delay_buffer_bytes_.store(0, std::memory_order_relaxed);
    prepared_total_buffer_bytes_.store(0, std::memory_order_relaxed);
}

void SignalGraph::publish_prepared_stats_(const CompiledGraph& cg) {
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

void SignalGraph::retire_snapshot_(std::shared_ptr<CompiledGraph> snapshot) {
    if (snapshot) retired_snapshots_.emplace_back(std::move(snapshot));
    prune_retired_snapshots_();
}

void SignalGraph::prune_retired_snapshots_() {
    // Seq-cst pairs with ProcessReadGuard and live_raw_ publication so a
    // writer cannot miss a reader that is about to load the retired snapshot.
    if (active_process_readers_.load(std::memory_order_seq_cst) == 0) {
        retired_snapshots_.clear();
    }
}

void SignalGraph::wait_for_retired_snapshots_() {
    while (active_process_readers_.load(std::memory_order_seq_cst) != 0) {
        std::this_thread::yield();
    }
    retired_snapshots_.clear();
}

void SignalGraph::compute_latencies_for_(CompiledGraph& cg,
                                         const std::vector<Connection>& /*conns*/) {
    for (NodeId id : cg.order) {
        auto rt_it = cg.runtime.find(id);
        if (rt_it == cg.runtime.end()) continue;
        auto& rt = rt_it->second;

        int64_t max_upstream = 0;
        bool has_upstream = false;
        for (const auto& c : cg.connections) {
            if (c.dest_node != id) continue;
            if (c.feedback || c.midi || c.automation) continue;
            auto src_it = cg.runtime.find(c.source_node);
            if (src_it == cg.runtime.end()) continue;
            max_upstream = std::max(max_upstream, src_it->second.output_latency);
            has_upstream = true;
        }
        rt.input_latency = has_upstream ? max_upstream : 0;

        int64_t added = 0;
        auto pit = cg.plugins.find(id);
        if (pit != cg.plugins.end() && pit->second) {
            added = std::max(0, pit->second->latency_samples());
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
        if (c.midi || c.automation) continue;

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

std::shared_ptr<SignalGraph::CompiledGraph>
SignalGraph::compile_(double sample_rate, int max_block_size) {
    auto cg = std::make_shared<CompiledGraph>();
    cg->max_block_size = max_block_size;
    cg->sample_rate = sample_rate;
    cg->connections = connections_;
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
            for (const auto& p : n.plugin->parameters()) {
                rt.param_bounds.push_back({
                    p.id,
                    p.min_value,
                    p.max_value,
                });
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

        CompiledGraph::NodeShape shape{n.type, n.num_input_ports, n.num_output_ports};
        cg->shapes[n.id] = shape;

        if (n.plugin) cg->plugins[n.id] = n.plugin;
        if (n.type == NodeType::Custom) {
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
        if (c.feedback) cg->feedback_edges.push_back(edge_ref);
        if (c.midi && !c.feedback) {
            rt.inbound_midi_edges.push_back(edge_ref);
        } else if (!c.midi && !c.automation && !c.audio_rate_modulation) {
            rt.inbound_audio_edges.push_back(edge_ref);
        }
        if (c.automation) {
            rt.sparse_automation_edges.push_back(edge_ref);
            auto& ids = rt.sparse_automation_param_ids;
            if (std::find(ids.begin(), ids.end(), c.automation_param_id) == ids.end()) {
                ids.push_back(c.automation_param_id);
                rt.sparse_automation_accum.resize(ids.size());
            }
        }
        if (c.audio_rate_modulation) {
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

    compute_latencies_for_(*cg, connections_);

    // Build the canonical-executor routing for this snapshot when the topology
    // is eligible. The Gain bindings resolve to THIS snapshot's own gain atomics
    // (valid for cg's whole lifetime), so the embedded snapshot needs no
    // keepalive. Ineligible graphs leave routing_valid false and use the walk.
    {
        CompiledGraph& cgr = *cg;
        cg->routing_valid = build_executor_snapshot(
            nodes_, connections_,
            [&cgr](NodeId id) -> std::atomic<float>* {
                auto it = cgr.runtime.find(id);
                return it == cgr.runtime.end() ? nullptr : it->second.gain.get();
            },
            [&cgr](NodeId id) -> PluginSlot* {
                auto it = cgr.plugins.find(id);
                return it == cgr.plugins.end() ? nullptr : it->second.get();
            },
            cg->routing_plugin_ctx, cg->routing_plugin_scratch,
            cg->routing_snapshot);
        // Size THIS snapshot's own scratch pool (per-snapshot, retired with the
        // snapshot via RCU — never resized under an in-flight reader).
        if (cg->routing_valid && max_block_size > 0) {
            cg->routing_valid = cg->exec_pool.reset(
                cg->routing_snapshot.buffer_slot_count(),
                static_cast<std::uint32_t>(max_block_size),
                cg->routing_snapshot.buffer_assignment().connection_delay_samples);
        }
        // Per-snapshot MIDI scratch + the MidiInput/MidiOutput node index lists
        // the routed dispatch bridges to the mailboxes. Built only when the
        // routed plan carries MIDI nodes, so audio-only graphs allocate none.
        cg->routing_midi_inputs.clear();
        cg->routing_midi_outputs.clear();
        if (cg->routing_valid) {
            const auto& plan = cg->routing_snapshot.plan();
            bool plan_has_midi = false;
            for (std::uint32_t i = 0; i < plan.nodes.size(); ++i) {
                const auto kind = plan.nodes[i].kind;
                if (kind == graph::GraphRuntimeNodeKind::MidiInput) {
                    cg->routing_midi_inputs.push_back({i, plan.nodes[i].id});
                    plan_has_midi = true;
                } else if (kind == graph::GraphRuntimeNodeKind::MidiOutput) {
                    cg->routing_midi_outputs.push_back({i, plan.nodes[i].id});
                    plan_has_midi = true;
                } else if (plan.nodes[i].event_input_ports > 0 ||
                           plan.nodes[i].event_output_ports > 0) {
                    plan_has_midi = true;  // a plugin carrying MIDI edges
                }
            }
            if (plan_has_midi) {
                cg->routing_valid = cg->routing_midi.reset(plan.node_count());
            }
            // Per-snapshot sparse-automation scratch, built only when the routed
            // plan carries automation connections (audio-only / MIDI graphs
            // allocate none).
            bool plan_has_automation = false;
            for (const auto& conn : plan.connections) {
                if (conn.is_automation) { plan_has_automation = true; break; }
            }
            if (cg->routing_valid && plan_has_automation && max_block_size > 0) {
                cg->routing_valid = cg->routing_automation.reset(
                    plan, static_cast<std::uint32_t>(max_block_size));
            }
        }

        // Levelized parallel routing: when enabled (at this prepare), build a
        // PARALLEL-SAFE (reuse-free) snapshot of the same eligible graph + its
        // levelization + a dedicated scratch pool, and ensure the persistent
        // worker pool is running. The MIDI/automation scratch and MidiInput/Output
        // node lists are SHARED with the serial path (identical plan). On any
        // failure the parallel path stays invalid and process() falls back.
        cg->routing_parallel_valid = false;
        if (cg->routing_valid && parallel_routing_enabled_.load(std::memory_order_relaxed) &&
            max_block_size > 0) {
            CompiledGraph& cgr = *cg;
            bool ok = build_executor_snapshot(
                nodes_, connections_,
                [&cgr](NodeId id) -> std::atomic<float>* {
                    auto it = cgr.runtime.find(id);
                    return it == cgr.runtime.end() ? nullptr : it->second.gain.get();
                },
                [&cgr](NodeId id) -> PluginSlot* {
                    auto it = cgr.plugins.find(id);
                    return it == cgr.plugins.end() ? nullptr : it->second.get();
                },
                cg->routing_plugin_ctx_parallel, cg->routing_plugin_scratch,
                cg->routing_snapshot_parallel, /*parallel_safe=*/true);
            if (ok) {
                cg->routing_levelization = graph::build_graph_runtime_levelization(
                    cg->routing_snapshot_parallel.plan());
                ok = cg->routing_levelization.ok &&
                     cg->exec_pool_parallel.reset(
                         cg->routing_snapshot_parallel.buffer_slot_count(),
                         static_cast<std::uint32_t>(max_block_size),
                         cg->routing_snapshot_parallel.buffer_assignment()
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
                // previously failed start, safe because routing_parallel_valid was
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
            cg->routing_parallel_valid = ok;
        }
    }
    return cg;
}

bool SignalGraph::prepare(double sample_rate, int max_block_size) {
    live_raw_.store(nullptr, std::memory_order_seq_cst);
    retire_snapshot_(std::move(live_));
    total_latency_samples_.store(0, std::memory_order_relaxed);
    clear_prepared_stats_();

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

    // Prepare each plugin slot first (pre-compile step).
    for (auto& n : nodes_) {
        if (n.plugin) {
            if (!n.plugin->prepare(sample_rate, max_block_size)) {
                runtime::log_error("SignalGraph: failed to prepare plugin '{}'", n.name);
                return false;
            }
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
    publish_prepared_stats_(*cg);
    live_ = std::move(cg);
    live_raw_.store(live_.get(), std::memory_order_seq_cst);
    prune_retired_snapshots_();
    return true;
}

void SignalGraph::set_limits(GraphLimits limits) {
    limits_ = limits;
    invalidate_live_();
}

std::size_t SignalGraph::total_declared_ports_() const {
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
    const std::size_t port_count = total_declared_ports_();
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
    const std::size_t port_count = total_declared_ports_();
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
    live_raw_.store(nullptr, std::memory_order_seq_cst);
    retire_snapshot_(std::move(live_));
    wait_for_retired_snapshots_();

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
    clear_prepared_stats_();
}

std::vector<uint8_t> SignalGraph::custom_node_state(NodeId id) const {
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
    for (auto& n : nodes_) {
        if (n.id != id) continue;
        if (n.type != NodeType::Custom) return false;
        n.custom_state_blob = bytes;
        // Apply to the live instance on the next prepare() (one-shot). The blob
        // is retained regardless, so it survives even when the type is
        // unresolved and is re-emitted on the next serialize.
        n.custom_state_pending = true;
        invalidate_live_();
        return true;
    }
    return false;
}

void SignalGraph::process(audio::BufferView<float>& output,
                          const audio::BufferView<const float>& input,
                          int num_samples) {
    struct ProcessReadGuard {
        explicit ProcessReadGuard(SignalGraph& owner) noexcept : owner_(owner) {
            // See prune_retired_snapshots_(): this reader count and the raw
            // snapshot pointer form one RCU-style lifetime handshake.
            owner_.active_process_readers_.fetch_add(1, std::memory_order_seq_cst);
        }
        ~ProcessReadGuard() noexcept {
            owner_.active_process_readers_.fetch_sub(1, std::memory_order_seq_cst);
        }
        SignalGraph& owner_;
    } read_guard{*this};

    auto* cg = live_raw_.load(std::memory_order_seq_cst);
    // Negative or zero block sizes mean "nothing to do" — return without
    // touching output (a memset with size_t(negative) wraps to a huge size).
    if (num_samples <= 0) return;
    if (!cg || num_samples > cg->max_block_size) {
        for (std::size_t c = 0; c < output.num_channels(); ++c)
            std::memset(output.channel_ptr(c), 0,
                        sizeof(float) * static_cast<size_t>(num_samples));
        return;
    }

    // Routed dispatch (opt-in): try the levelized PARALLEL executor first (if
    // enabled), then the SERIAL executor, then fall through to the legacy walk.
    // Both routed paths run GraphRuntimeExecutor (which zeroes the output bus and
    // accumulates AudioOutput itself, so a successful routed call returns before
    // the legacy zero+walk below) and SHARE the MIDI mailbox bridge (the MIDI
    // scratch + MidiInput/Output node lists are shared — identical plan). The
    // dispatch stays inside the ProcessReadGuard, so `cg` (snapshots, pools, gain
    // atomics) is pinned for the whole call. Output is bit-identical across paths.
    {
        const auto frames32 = static_cast<std::uint32_t>(num_samples);
        const bool has_midi = cg->routing_midi.node_count() > 0;
        const bool has_automation = cg->routing_automation.node_count() > 0;

        pulp::format::BusBufferSet buses;
        const bool buses_ok =
            buses.add_input("main", input, pulp::format::BusRole::Main) &&
            buses.add_output("main", output, pulp::format::BusRole::Main);
        if (buses_ok) {
            pulp::format::ProcessBlock block;
            block.sample_rate = cg->sample_rate;
            block.frame_count = frames32;
            block.buses = &buses;

            // Run one routed path with the shared MIDI mailbox bridge around it.
            // `run` returns the executor result; this returns true iff routing
            // succeeded (took the path). On failure the consumed MIDI sequences
            // are NOT committed, so a fallback path re-consumes the same block.
            auto dispatch_routed = [&](auto&& run) -> bool {
                if (!block.validate()) return false;
                if (has_midi) {
                    for (auto& mi : cg->routing_midi_inputs) {
                        mi.pending_seq = 0;
                        midi::MidiBuffer* out_buf = cg->routing_midi.out(mi.plan_index);
                        if (out_buf == nullptr) continue;
                        clear_midi_block(*out_buf);
                        cg->routing_midi.set_out_incomplete(mi.plan_index, false);
                        auto rt_it = cg->runtime.find(mi.id);
                        if (rt_it == cg->runtime.end() ||
                            !rt_it->second.midi_input_mailbox) {
                            continue;
                        }
                        const auto& injected =
                            rt_it->second.midi_input_mailbox->published.read();
                        if (injected.sequence != 0 &&
                            injected.sequence != rt_it->second.midi_input_sequence_seen) {
                            cg->routing_midi.set_out_incomplete(
                                mi.plan_index, !injected.copy_to_midi(*out_buf));
                            mi.pending_seq = injected.sequence;
                        }
                    }
                }
                if (!run().ok()) return false;
                if (has_midi) {
                    for (const auto& mi : cg->routing_midi_inputs) {
                        if (mi.pending_seq == 0) continue;
                        auto rt_it = cg->runtime.find(mi.id);
                        if (rt_it != cg->runtime.end()) {
                            rt_it->second.midi_input_sequence_seen = mi.pending_seq;
                        }
                    }
                    for (const auto& mo : cg->routing_midi_outputs) {
                        midi::MidiBuffer* in_buf = cg->routing_midi.in(mo.plan_index);
                        auto rt_it = cg->runtime.find(mo.id);
                        if (in_buf == nullptr || rt_it == cg->runtime.end() ||
                            !rt_it->second.midi_output_mailbox) {
                            continue;
                        }
                        cg->midi_publish_scratch.set_from_midi(
                            *in_buf, 0, cg->routing_midi.in_incomplete(mo.plan_index));
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
            if (parallel_routing_enabled_.load(std::memory_order_relaxed) &&
                cg->routing_parallel_valid && worker_pool_.running() &&
                cg->exec_pool_parallel.fits(cg->routing_snapshot_parallel, frames32)) {
                if (dispatch_routed([&] {
                        return executor_.process_parallel(
                            block, cg->routing_snapshot_parallel,
                            cg->routing_levelization, cg->exec_pool_parallel,
                            worker_pool_, has_midi ? &cg->routing_midi : nullptr,
                            has_automation ? &cg->routing_automation : nullptr);
                    })) {
                    return;
                }
            }

            if (canonical_executor_routing_enabled_.load(std::memory_order_relaxed) &&
                cg->routing_valid &&
                cg->exec_pool.fits(cg->routing_snapshot, frames32)) {
                if (dispatch_routed([&] {
                        return executor_.process_routed(
                            block, cg->routing_snapshot, cg->exec_pool,
                            has_midi ? &cg->routing_midi : nullptr,
                            has_automation ? &cg->routing_automation : nullptr);
                    })) {
                    return;
                }
            }
        }
        // Any setup failure / disabled path falls through to the legacy walk.
    }

    // Clear the final destination; AudioOutput nodes accumulate into it.
    for (std::size_t c = 0; c < output.num_channels(); ++c) {
        std::memset(output.channel_ptr(c), 0, sizeof(float) * static_cast<size_t>(num_samples));
    }

    auto pass_through_or_zero = [num_samples](NodeRuntime& rt) {
        const int in_ch  = static_cast<int>(rt.input_ptrs.size());
        const int out_ch = static_cast<int>(rt.output_ptrs.size());
        const int chs = std::min(in_ch, out_ch);
        for (int c = 0; c < chs; ++c) {
            std::memcpy(rt.output_ptrs[c], rt.input_ptrs[c],
                        sizeof(float) * static_cast<size_t>(num_samples));
        }
        for (int c = chs; c < out_ch; ++c) {
            std::memset(rt.output_ptrs[c], 0,
                        sizeof(float) * static_cast<size_t>(num_samples));
        }
    };

    for (const auto& ordered : cg->ordered_runtime) {
        const NodeId id = ordered.id;
        const auto& shape = ordered.shape;
        auto& rt = *ordered.runtime;

        // 1. Zero input scratch.
        if (!rt.input_data.empty()) {
            std::memset(rt.input_data.data(), 0, rt.input_data.size() * sizeof(float));
        }
        rt.midi_in_incomplete = false;
        clear_midi_block(rt.midi_in);
        rt.midi_out_incomplete = false;
        clear_midi_block(rt.midi_out);
        if (shape.type == NodeType::MidiInput && rt.midi_input_mailbox) {
            const auto& injected = rt.midi_input_mailbox->published.read();
            if (injected.sequence != 0
                && injected.sequence != rt.midi_input_sequence_seen) {
                rt.midi_input_sequence_seen = injected.sequence;
                if (!injected.copy_to_midi(rt.midi_out)) {
                    rt.midi_out_incomplete = true;
                }
            }
        }

        // 1b. Gather MIDI from MIDI-flagged inbound connections.
        for (const auto& edge : rt.inbound_midi_edges) {
            if (!edge.source_runtime) continue;
            if (!copy_midi_block(edge.source_runtime->midi_out, rt.midi_in)
                || edge.source_runtime->midi_out_incomplete) {
                rt.midi_in_incomplete = true;
            }
        }

        // 2. Gather audio inbound with PDC/feedback delay lines.
        for (const auto& edge : rt.inbound_audio_edges) {
            const size_t ci = edge.connection_index;
            const auto& c = cg->connections[ci];
            const int dport = static_cast<int>(c.dest_port);
            if (dport < 0 || dport >= static_cast<int>(rt.input_ptrs.size())) continue;
            if (!edge.source_runtime) continue;
            const auto& src_rt = *edge.source_runtime;
            const int sport = static_cast<int>(c.source_port);
            if (sport < 0 || sport >= static_cast<int>(src_rt.output_ptrs.size())) continue;

            float* dst = rt.input_ptrs[dport];
            const float* src = src_rt.output_ptrs[sport];
            auto& dl = cg->connection_delays[ci];
            if (c.feedback) {
                if (!dl.feedback_prev.empty()) {
                    for (int i = 0; i < num_samples; ++i) dst[i] += dl.feedback_prev[(size_t)i];
                }
                continue;
            }
            if (dl.delay_samples <= 0 || dl.ring.empty()) {
                for (int i = 0; i < num_samples; ++i) dst[i] += src[i];
            } else {
                const int ring_size = (int)dl.ring.size();
                const int D = dl.delay_samples;
                int wp = dl.write_pos;
                int rp = wp - D;
                if (rp < 0) rp += ring_size;
                for (int i = 0; i < num_samples; ++i) {
                    dl.ring[(size_t)wp] = src[i];
                    dst[i] += dl.ring[(size_t)rp];
                    if (++wp == ring_size) wp = 0;
                    if (++rp == ring_size) rp = 0;
                }
                dl.write_pos = wp;
            }
        }

        // 3. Produce output based on node type. Wrap the node's work in its
        // persistent load measurer (relaxed-atomic begin()/end(); RT-safe) so
        // per-node CPU load is attributable via node_loads().
        if (rt.load) rt.load->begin(num_samples, static_cast<float>(cg->sample_rate));
        switch (shape.type) {
            case NodeType::AudioInput: {
                const int chs = std::min(
                    static_cast<int>(rt.output_ptrs.size()),
                    static_cast<int>(input.num_channels()));
                for (int c = 0; c < chs; ++c) {
                    std::memcpy(rt.output_ptrs[c], input.channel_ptr(c),
                                sizeof(float) * static_cast<size_t>(num_samples));
                }
                for (int c = chs; c < static_cast<int>(rt.output_ptrs.size()); ++c) {
                    std::memset(rt.output_ptrs[c], 0,
                                sizeof(float) * static_cast<size_t>(num_samples));
                }
                break;
            }
            case NodeType::Plugin: {
                auto pit = cg->plugins.find(id);
                if (pit == cg->plugins.end() || !pit->second) {
                    // GraphSerializer rehydration creates a placeholder Plugin
                    // node when the saved plugin can't be resolved (missing,
                    // moved, or wrong format). Without an explicit branch
                    // here, output scratch keeps whatever stale data it
                    // carried, causing audible artifacts on the next
                    // AudioOutput gather.
                    // Deterministic fallback: pass input → output when
                    // channel counts match, zero-fill when they don't.
                    pass_through_or_zero(rt);
                    break;
                }
                audio::BufferView<float> out_view(
                    rt.output_ptrs.data(), rt.output_ptrs.size(),
                    static_cast<std::size_t>(num_samples));
                audio::BufferView<const float> in_c(
                    rt.input_const_ptrs.data(), rt.input_const_ptrs.size(),
                    static_cast<std::size_t>(num_samples));
                clear_midi_block(rt.midi_out);
                rt.midi_out_incomplete = false;

                // Build per-block automation event queue. For each
                // (param_id) target, collect values from every automation
                // edge and mix per edges' MixMode. Two control points per
                // block (sample 0 and N-1) so the plugin can interpolate.
                // Audio-rate modulation edges append one event per sample
                // after applying the same PDC delay-line alignment used by
                // normal audio connections.
                ParameterEventQueue param_events;
                {
                    auto bounds_for_param = [&rt](uint32_t param_id,
                                                  float fallback_lo,
                                                  float fallback_hi) {
                        for (const auto& bounds : rt.param_bounds) {
                            if (bounds.id != param_id) continue;
                            return std::pair<float, float>{
                                std::min(bounds.min_value, bounds.max_value),
                                std::max(bounds.min_value, bounds.max_value),
                            };
                        }
                        return std::pair<float, float>{
                            std::min(fallback_lo, fallback_hi),
                            std::max(fallback_lo, fallback_hi),
                        };
                    };

                    for (auto& a : rt.sparse_automation_accum) {
                        a = NodeRuntime::SparseAutomationAccum{};
                    }
                    const int last = num_samples - 1;
                    for (const auto& edge : rt.sparse_automation_edges) {
                        const size_t ci = edge.connection_index;
                        const auto& c = cg->connections[ci];
                        if (!edge.source_runtime) continue;
                        const int sport = static_cast<int>(c.source_port);
                        if (sport < 0
                            || sport >= (int)edge.source_runtime->output_ptrs.size()) {
                            continue;
                        }
                        const float* src = edge.source_runtime->output_ptrs[sport];
                        const float s0 = std::clamp(src[0], 0.0f, 1.0f);
                        const float sN = std::clamp(src[last < 0 ? 0 : last], 0.0f, 1.0f);
                        float m0 = c.automation_range_lo
                            + s0 * (c.automation_range_hi - c.automation_range_lo);
                        float mN = c.automation_range_lo
                            + sN * (c.automation_range_hi - c.automation_range_lo);

                        // Per-source linear slew. The user declares
                        // automation_smoothing_ms; we limit how far the
                        // delivered value can move per sample at that ramp
                        // speed. State (last_value/primed) lives on the
                        // parallel ConnectionDelay so it survives the next
                        // block.
                        if (c.automation_smoothing_ms > 0.0f
                            && cg->sample_rate > 0.0
                            && ci < cg->connection_delays.size()) {
                            auto& dl = cg->connection_delays[ci];
                            const float range = std::abs(
                                c.automation_range_hi - c.automation_range_lo);
                            const double slew_samples =
                                (double)c.automation_smoothing_ms * 0.001
                                * cg->sample_rate;
                            // max move per sample, in the plugin's plain
                            // parameter domain. We use the connection's
                            // mapped range as the "full sweep" scale so
                            // smoothing_ms behaves like "ms to traverse
                            // the entire declared range".
                            const float max_step = slew_samples > 0.0
                                ? (range / (float)slew_samples)
                                : range;
                            if (!dl.slew_primed) {
                                // First block — snap so we don't glide
                                // up from 0.
                                dl.slew_last_value = m0;
                                dl.slew_primed = true;
                            }
                            auto ramp_to = [max_step](float from, float target) {
                                const float delta = target - from;
                                if (delta > max_step) return from + max_step;
                                if (delta < -max_step) return from - max_step;
                                return target;
                            };
                            // v0 lands at sample 0; vN lands at sample
                            // `last`. Slew from the previous block's
                            // post-slew value to m0 in one step, then
                            // from there to mN over `last` steps.
                            const float new_v0 = ramp_to(dl.slew_last_value, m0);
                            float new_vN = new_v0;
                            if (last > 0) {
                                const float max_block_step =
                                    max_step * (float)last;
                                const float delta = mN - new_v0;
                                if (delta > max_block_step) {
                                    new_vN = new_v0 + max_block_step;
                                } else if (delta < -max_block_step) {
                                    new_vN = new_v0 - max_block_step;
                                } else {
                                    new_vN = mN;
                                }
                            }
                            dl.slew_last_value = new_vN;
                            m0 = new_v0;
                            mN = new_vN;
                        }

                        auto param_it = std::find(rt.sparse_automation_param_ids.begin(),
                                                  rt.sparse_automation_param_ids.end(),
                                                  c.automation_param_id);
                        if (param_it == rt.sparse_automation_param_ids.end()) continue;
                        auto& a = rt.sparse_automation_accum[static_cast<size_t>(
                            std::distance(rt.sparse_automation_param_ids.begin(), param_it))];
                        const auto bounds = bounds_for_param(c.automation_param_id,
                                                             c.automation_range_lo,
                                                             c.automation_range_hi);
                        a.lo = bounds.first;
                        a.hi = bounds.second;
                        a.touched = true;
                        if (c.automation_mix == AutomationMix::Replace) {
                            a.v0 = m0;
                            a.vN = mN;
                        } else {
                            a.v0 += m0;
                            a.vN += mN;
                            a.has_add = true;
                        }
                    }
                    for (size_t pi = 0; pi < rt.sparse_automation_accum.size(); ++pi) {
                        auto& a = rt.sparse_automation_accum[pi];
                        if (!a.touched) continue;
                        const uint32_t pid = rt.sparse_automation_param_ids[pi];
                        float v0 = a.v0, vN = a.vN;
                        if (a.has_add) {
                            const float lo = std::min(a.lo, a.hi);
                            const float hi = std::max(a.lo, a.hi);
                            v0 = std::clamp(v0, lo, hi);
                            vN = std::clamp(vN, lo, hi);
                        }
                        if (!push_parameter_event(param_events, pid, 0, v0)) break;
                        if (last > 0
                            && !push_parameter_event(param_events, pid, last, vN)) {
                            break;
                        }
                    }

                    if (!rt.audio_rate_param_ids.empty()) {
                        const size_t block = static_cast<size_t>(cg->max_block_size);
                        std::fill(rt.audio_rate_param_data.begin(),
                                  rt.audio_rate_param_data.end(),
                                  0.0f);

                        for (auto& d : rt.audio_rate_accum) {
                            d = NodeRuntime::DenseAutomationAccum{};
                        }

                        for (const auto& edge : rt.audio_rate_modulation_edges) {
                            const size_t ci = edge.connection_index;
                            const auto& c = cg->connections[ci];
                            if (!edge.source_runtime) continue;
                            const int sport = static_cast<int>(c.source_port);
                            if (sport < 0
                                || sport >= (int)edge.source_runtime->output_ptrs.size()) {
                                continue;
                            }

                            auto param_it = std::find(rt.audio_rate_param_ids.begin(),
                                                      rt.audio_rate_param_ids.end(),
                                                      c.automation_param_id);
                            if (param_it == rt.audio_rate_param_ids.end()) continue;
                            const size_t param_index = static_cast<size_t>(
                                std::distance(rt.audio_rate_param_ids.begin(), param_it));
                            auto& dst = rt.audio_rate_accum[param_index];
                            float* dst_values =
                                rt.audio_rate_param_data.data() + param_index * block;
                            const auto bounds = bounds_for_param(c.automation_param_id,
                                                                 c.automation_range_lo,
                                                                 c.automation_range_hi);
                            dst.lo = bounds.first;
                            dst.hi = bounds.second;

                            const float* src = edge.source_runtime->output_ptrs[sport];
                            auto& dl = cg->connection_delays[ci];
                            if (dl.delay_samples <= 0 || dl.ring.empty()) {
                                for (int i = 0; i < num_samples; ++i) {
                                    const float value = map_modulation_sample(c, src[i]);
                                    if (c.automation_mix == AutomationMix::Replace) {
                                        dst_values[static_cast<size_t>(i)] = value;
                                        dst.has_replace = true;
                                    } else {
                                        dst_values[static_cast<size_t>(i)] += value;
                                        dst.has_add = true;
                                    }
                                }
                            } else {
                                const int ring_size = (int)dl.ring.size();
                                const int D = dl.delay_samples;
                                int wp = dl.write_pos;
                                int rp = wp - D;
                                if (rp < 0) rp += ring_size;
                                for (int i = 0; i < num_samples; ++i) {
                                    dl.ring[static_cast<size_t>(wp)] = src[i];
                                    const float value = map_modulation_sample(
                                        c, dl.ring[static_cast<size_t>(rp)]);
                                    if (c.automation_mix == AutomationMix::Replace) {
                                        dst_values[static_cast<size_t>(i)] = value;
                                        dst.has_replace = true;
                                    } else {
                                        dst_values[static_cast<size_t>(i)] += value;
                                        dst.has_add = true;
                                    }
                                    if (++wp == ring_size) wp = 0;
                                    if (++rp == ring_size) rp = 0;
                                }
                                dl.write_pos = wp;
                            }
                        }

                        for (size_t pi = 0; pi < rt.audio_rate_accum.size(); ++pi) {
                            const auto& d = rt.audio_rate_accum[pi];
                            if (!d.has_replace && !d.has_add) continue;
                            const uint32_t param_id = rt.audio_rate_param_ids[pi];
                            const float* values =
                                rt.audio_rate_param_data.data() + pi * block;
                            const float lo = std::min(d.lo, d.hi);
                            const float hi = std::max(d.lo, d.hi);
                            for (int i = 0; i < num_samples; ++i) {
                                float value = values[static_cast<size_t>(i)];
                                if (d.has_add) value = std::clamp(value, lo, hi);
                                if (!push_parameter_event(param_events, param_id, i, value)) {
                                    break;
                                }
                            }
                        }
                    }
                    param_events.sort();
                }

                std::array<format::ProcessBusBufferView<const float>, 1> input_buses{{
                    {
                        .info = {
                            .name = "Plugin Node In",
                            .index = 0,
                            .direction = format::BusDirection::Input,
                            // bus label for a plugin node's main I/O inside a SignalGraph
                            .role = format::BusRole::Main,
                            .declared_channels =
                                static_cast<int>(in_c.num_channels()),
                            .optional = in_c.num_channels() == 0,
                            .active = in_c.num_channels() > 0,
                        },
                        .buffer = in_c,
                    },
                }};
                std::array<format::ProcessBusBufferView<float>, 1> output_buses{{
                    {
                        .info = {
                            .name = "Plugin Node Out",
                            .index = 0,
                            .direction = format::BusDirection::Output,
                            .role = format::BusRole::Main,
                            .declared_channels =
                                static_cast<int>(out_view.num_channels()),
                            .optional = false,
                            .active = out_view.num_channels() > 0,
                        },
                        .buffer = out_view,
                    },
                }};
                format::ProcessBuffers process_buffers{
                    format::ProcessBusBufferSet<const float>{std::span(input_buses)},
                    format::ProcessBusBufferSet<float>{std::span(output_buses)},
                };

                pit->second->process(process_buffers, rt.midi_in, rt.midi_out,
                                     param_events, num_samples);
                rt.midi_out_incomplete =
                    rt.midi_in_incomplete || midi_block_has_drops(rt.midi_out);
                break;
            }
            case NodeType::Gain: {
                const float g = rt.gain
                    ? rt.gain->load(std::memory_order_relaxed)
                    : 1.0f;
                const int chs = std::min(
                    static_cast<int>(rt.input_ptrs.size()),
                    static_cast<int>(rt.output_ptrs.size()));
                for (int c = 0; c < chs; ++c) {
                    const float* in = rt.input_ptrs[c];
                    float* out = rt.output_ptrs[c];
                    for (int i = 0; i < num_samples; ++i) out[i] = in[i] * g;
                }
                break;
            }
            case NodeType::AudioOutput: {
                const int chs = std::min(
                    static_cast<int>(rt.input_ptrs.size()),
                    static_cast<int>(output.num_channels()));
                for (int c = 0; c < chs; ++c) {
                    float* dst = output.channel_ptr(c);
                    const float* src = rt.input_ptrs[c];
                    for (int i = 0; i < num_samples; ++i) dst[i] += src[i];
                }
                break;
            }
            case NodeType::MidiInput:
                break;
            case NodeType::MidiOutput:
                if (rt.midi_output_mailbox) {
                    cg->midi_publish_scratch.set_from_midi(
                        rt.midi_in,
                        0,
                        rt.midi_in_incomplete);
                    rt.midi_output_mailbox->write(cg->midi_publish_scratch);
                }
                break;
            case NodeType::Custom:
                if (auto custom_it = cg->custom_processors.find(id);
                    custom_it != cg->custom_processors.end()) {
                    audio::BufferView<float> out_view(
                        rt.output_ptrs.data(), rt.output_ptrs.size(),
                        static_cast<std::size_t>(num_samples));
                    audio::BufferView<const float> in_view(
                        rt.input_const_ptrs.data(), rt.input_const_ptrs.size(),
                        static_cast<std::size_t>(num_samples));
                    custom_it->second(out_view, in_view, num_samples);
                } else {
                    pass_through_or_zero(rt);
                }
                break;
        }
        if (rt.load) rt.load->end();
    }

    // Capture each feedback source's current block for the *next* block.
    for (const auto& edge : cg->feedback_edges) {
        const size_t ci = edge.connection_index;
        const auto& c = cg->connections[ci];
        if (!edge.source_runtime) continue;
        const auto& src_rt = *edge.source_runtime;
        const int sport = static_cast<int>(c.source_port);
        if (sport < 0 || sport >= static_cast<int>(src_rt.output_ptrs.size())) continue;
        auto& dl = cg->connection_delays[ci];
        if (dl.feedback_prev.empty()) continue;
        const float* src = src_rt.output_ptrs[sport];
        std::memcpy(dl.feedback_prev.data(), src,
                    sizeof(float) * static_cast<size_t>(num_samples));
    }

    // Drain MidiInput nodes' audio-thread scratch so events consumed for THIS
    // block never carry over. inject_midi() publishes into the mailbox; the
    // next process() call copies a new, unseen snapshot back into midi_out.
    for (const auto& ordered : cg->ordered_runtime) {
        if (ordered.shape.type == NodeType::MidiInput) {
            clear_midi_block(ordered.runtime->midi_out);
            ordered.runtime->midi_out_incomplete = false;
        }
    }
}

void SignalGraph::clear() {
    connections_.clear();
    nodes_.clear();
    next_id_ = 1;
    invalidate_live_();
}

bool SignalGraph::set_node_gain(NodeId id, float linear_gain) {
    // Write the UI-thread-owned scalar on GraphNode so it survives future
    // compile_() calls. Also reflect into the live snapshot's runtime through
    // a per-runtime atomic so the change takes effect without a re-prepare.
    auto* n = const_cast<GraphNode*>(node(id));
    if (!n) return false;
    n->gain = linear_gain;
    auto* cg = live_raw_.load(std::memory_order_acquire);
    if (cg) {
        auto it = cg->runtime.find(id);
        if (it != cg->runtime.end() && it->second.gain) {
            it->second.gain->store(linear_gain, std::memory_order_relaxed);
        }
    }
    return true;
}

float SignalGraph::node_gain(NodeId id) const {
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
