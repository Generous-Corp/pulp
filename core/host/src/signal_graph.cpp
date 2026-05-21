// Signal Graph implementation.
//
// Phase 0B: audio-thread reads happen exclusively against an immutable
// CompiledGraph snapshot, published via atomic<shared_ptr>. UI-thread
// mutations (add_*, connect*, disconnect, remove_node, set_node_gain,
// set_node_parameter) invalidate the snapshot; prepare() rebuilds a fresh
// snapshot and atomic-swaps it in. See signal_graph.hpp for the mutation
// protocol details.

#include <pulp/host/signal_graph.hpp>
#include <pulp/runtime/log.hpp>
#include <algorithm>
#include <queue>
#include <unordered_set>
#include <unordered_map>
#include <cstring>
#include <utility>

namespace pulp::host {
namespace {

bool parameter_allows_modulation(const HostParamInfo& p,
                                 uint32_t param_id,
                                 state::ParamRate required_rate) {
    return p.id == param_id
        && p.rate == required_rate
        && p.flags.automatable
        && !p.flags.read_only
        && !p.flags.stepped;
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

} // namespace

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
    if (!node(source) || !node(dest)) return false;
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

bool SignalGraph::connect_automation(NodeId src, PortIndex src_audio_port,
                                     NodeId dest, uint32_t dest_param_id,
                                     float range_lo, float range_hi,
                                     float smoothing_ms,
                                     AutomationMix mix) {
    const GraphNode* src_n = node(src);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n) return false;
    if (dst_n->type != NodeType::Plugin || !dst_n->plugin) return false;
    if ((int)src_audio_port >= src_n->num_output_ports) return false;

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
    if ((int)src_audio_port >= src_n->num_output_ports) return false;
    if (would_create_cycle(src, dest)) return false;

    bool ok_param = false;
    for (const auto& pi : dst_n->plugin->parameters()) {
        if (pi.id != dest_param_id) continue;
        if (!parameter_allows_modulation(pi, dest_param_id, state::ParamRate::AudioRate)) {
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
    connections_.push_back(conn);
    invalidate_live_();
    return true;
}

bool SignalGraph::inject_midi(NodeId id, const midi::MidiBuffer& events) {
    auto cg = std::atomic_load_explicit(&live_, std::memory_order_acquire);
    if (!cg) return false;
    auto it = cg->runtime.find(id);
    if (it == cg->runtime.end()) return false;
    it->second.midi_out.clear();
    for (const auto& ev : events) it->second.midi_out.add(ev);
    return true;
}

bool SignalGraph::extract_midi(NodeId id, midi::MidiBuffer& out) const {
    auto cg = std::atomic_load_explicit(&live_, std::memory_order_acquire);
    if (!cg) return false;
    auto it = cg->runtime.find(id);
    if (it == cg->runtime.end()) return false;
    for (const auto& ev : it->second.midi_in) out.add(ev);
    return true;
}

bool SignalGraph::connect_feedback(NodeId source, PortIndex source_port,
                                   NodeId dest, PortIndex dest_port) {
    if (!node(source) || !node(dest)) return false;
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
    auto cg = std::atomic_load_explicit(&live_, std::memory_order_acquire);
    if (!cg) return 0;
    auto it = cg->runtime.find(id);
    if (it == cg->runtime.end()) return 0;
    return (int)it->second.input_latency;
}

void SignalGraph::invalidate_live_() {
    // Drop the live snapshot; process() will return silence until prepare()
    // is called again. This is the simple, safe semantic: UI-thread edits
    // always require a re-prepare before audio resumes.
    std::atomic_store_explicit(&live_, std::shared_ptr<CompiledGraph>(nullptr), std::memory_order_release);
    total_latency_samples_.store(0, std::memory_order_relaxed);
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
SignalGraph::compile_(double /*sample_rate*/, int max_block_size) {
    auto cg = std::make_shared<CompiledGraph>();
    cg->max_block_size = max_block_size;
    cg->connections = connections_;
    cg->order = processing_order();

    for (auto& n : nodes_) {
        NodeRuntime rt;
        const int out_ch = std::max(0, n.num_output_ports);
        const int in_ch  = std::max(0, n.num_input_ports);
        rt.output_data.assign(static_cast<size_t>(out_ch) * max_block_size, 0.f);
        rt.input_data.assign(static_cast<size_t>(in_ch) * max_block_size, 0.f);
        rt.output_ptrs.resize(out_ch);
        rt.input_ptrs.resize(in_ch);
        for (int c = 0; c < out_ch; ++c)
            rt.output_ptrs[c] = rt.output_data.data() + static_cast<size_t>(c) * max_block_size;
        for (int c = 0; c < in_ch; ++c)
            rt.input_ptrs[c] = rt.input_data.data() + static_cast<size_t>(c) * max_block_size;
        rt.gain = n.gain;  // copy UI-thread scalar into per-snapshot runtime
        if (n.plugin) {
            for (const auto& p : n.plugin->parameters()) {
                rt.param_bounds.push_back({
                    p.id,
                    p.min_value,
                    p.max_value,
                });
            }
        }
        cg->runtime[n.id] = std::move(rt);

        CompiledGraph::NodeShape shape{n.type, n.num_input_ports, n.num_output_ports};
        cg->shapes[n.id] = shape;

        if (n.plugin) cg->plugins[n.id] = n.plugin;
    }

    for (const auto& c : cg->connections) {
        if (!c.audio_rate_modulation) continue;
        auto rt_it = cg->runtime.find(c.dest_node);
        if (rt_it == cg->runtime.end()) continue;
        auto& ids = rt_it->second.audio_rate_param_ids;
        if (std::find(ids.begin(), ids.end(), c.automation_param_id) == ids.end()) {
            ids.push_back(c.automation_param_id);
            rt_it->second.audio_rate_param_data.resize(
                ids.size() * static_cast<size_t>(max_block_size), 0.0f);
        }
    }

    compute_latencies_for_(*cg, connections_);
    return cg;
}

bool SignalGraph::prepare(double sample_rate, int max_block_size) {
    if (max_block_size <= 0) return false;

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

    auto cg = compile_(sample_rate, max_block_size);
    total_latency_samples_.store(cg->total_latency_samples, std::memory_order_relaxed);
    std::atomic_store_explicit(&live_, std::move(cg), std::memory_order_release);
    return true;
}

void SignalGraph::release() {
    for (auto& n : nodes_) if (n.plugin) n.plugin->release();
    std::atomic_store_explicit(&live_, std::shared_ptr<CompiledGraph>(nullptr), std::memory_order_release);
    total_latency_samples_.store(0, std::memory_order_relaxed);
}

void SignalGraph::process(audio::BufferView<float>& output,
                          const audio::BufferView<const float>& input,
                          int num_samples) {
    auto cg = std::atomic_load_explicit(&live_, std::memory_order_acquire);
    // Negative or zero block sizes mean "nothing to do" — return without
    // touching output (a memset with size_t(negative) wraps to a huge size).
    if (num_samples <= 0) return;
    if (!cg || num_samples > cg->max_block_size) {
        for (std::size_t c = 0; c < output.num_channels(); ++c)
            std::memset(output.channel_ptr(c), 0,
                        sizeof(float) * static_cast<size_t>(num_samples));
        return;
    }

    // Clear the final destination; AudioOutput nodes accumulate into it.
    for (std::size_t c = 0; c < output.num_channels(); ++c) {
        std::memset(output.channel_ptr(c), 0, sizeof(float) * static_cast<size_t>(num_samples));
    }

    for (NodeId id : cg->order) {
        auto shape_it = cg->shapes.find(id);
        if (shape_it == cg->shapes.end()) continue;
        const auto& shape = shape_it->second;
        auto rt_it = cg->runtime.find(id);
        if (rt_it == cg->runtime.end()) continue;
        auto& rt = rt_it->second;

        // 1. Zero input scratch.
        if (!rt.input_data.empty()) {
            std::memset(rt.input_data.data(), 0, rt.input_data.size() * sizeof(float));
        }
        rt.midi_in.clear();
        if (shape.type != NodeType::MidiInput) rt.midi_out.clear();

        // 1b. Gather MIDI from MIDI-flagged inbound connections.
        for (const auto& c : cg->connections) {
            if (c.dest_node != id || !c.midi || c.feedback) continue;
            auto src_it = cg->runtime.find(c.source_node);
            if (src_it == cg->runtime.end()) continue;
            for (const auto& ev : src_it->second.midi_out) rt.midi_in.add(ev);
        }

        // 2. Gather audio inbound with PDC/feedback delay lines.
        for (size_t ci = 0; ci < cg->connections.size(); ++ci) {
            const auto& c = cg->connections[ci];
            if (c.dest_node != id) continue;
            if (c.midi) continue;
            if (c.automation) continue;  // dispatched in the Plugin branch
            if (c.audio_rate_modulation) continue;  // dense path is built separately
            const int dport = static_cast<int>(c.dest_port);
            if (dport < 0 || dport >= static_cast<int>(rt.input_ptrs.size())) continue;
            if (c.source_node == 0) continue;

            auto src_rt_it = cg->runtime.find(c.source_node);
            if (src_rt_it == cg->runtime.end()) continue;
            const auto& src_rt = src_rt_it->second;
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

        // 3. Produce output based on node type.
        switch (shape.type) {
            case NodeType::AudioInput: {
                const int chs = std::min(
                    static_cast<int>(rt.output_ptrs.size()),
                    static_cast<int>(input.num_channels()));
                for (int c = 0; c < chs; ++c) {
                    std::memcpy(rt.output_ptrs[c], input.channel_ptr(c),
                                sizeof(float) * static_cast<size_t>(num_samples));
                }
                break;
            }
            case NodeType::Plugin: {
                auto pit = cg->plugins.find(id);
                if (pit == cg->plugins.end() || !pit->second) {
                    // Issue #491 P2 bonus: graph_serializer rehydration
                    // creates a "placeholder" Plugin node when the saved
                    // plugin can't be resolved (missing / moved / wrong
                    // format). Without an explicit branch here, output
                    // scratch keeps whatever stale data it carried —
                    // audible artifacts on the next AudioOutput gather.
                    // Deterministic fallback: pass input → output when
                    // channel counts match, zero-fill when they don't.
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
                    break;
                }
                audio::BufferView<float> out_view(
                    rt.output_ptrs.data(), rt.output_ptrs.size(),
                    static_cast<std::size_t>(num_samples));
                std::vector<const float*> in_const(rt.input_ptrs.begin(), rt.input_ptrs.end());
                audio::BufferView<const float> in_c(
                    in_const.data(), in_const.size(),
                    static_cast<std::size_t>(num_samples));
                rt.midi_out.clear();

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

                    struct Accum {
                        float v0 = 0.f, vN = 0.f;
                        float lo = 0.f, hi = 1.f;
                        bool has_add = false;
                    };
                    std::unordered_map<uint32_t, Accum> acc;
                    const int last = num_samples - 1;
                    for (const auto& c : cg->connections) {
                        if (!c.automation || c.dest_node != id) continue;
                        auto src_it = cg->runtime.find(c.source_node);
                        if (src_it == cg->runtime.end()) continue;
                        const int sport = static_cast<int>(c.source_port);
                        if (sport < 0 || sport >= (int)src_it->second.output_ptrs.size()) continue;
                        const float* src = src_it->second.output_ptrs[sport];
                        const float s0 = std::clamp(src[0], 0.0f, 1.0f);
                        const float sN = std::clamp(src[last < 0 ? 0 : last], 0.0f, 1.0f);
                        const float m0 = c.automation_range_lo
                            + s0 * (c.automation_range_hi - c.automation_range_lo);
                        const float mN = c.automation_range_lo
                            + sN * (c.automation_range_hi - c.automation_range_lo);
                        auto& a = acc[c.automation_param_id];
                        const auto bounds = bounds_for_param(c.automation_param_id,
                                                             c.automation_range_lo,
                                                             c.automation_range_hi);
                        a.lo = bounds.first;
                        a.hi = bounds.second;
                        if (c.automation_mix == AutomationMix::Replace) {
                            a.v0 = m0;
                            a.vN = mN;
                        } else {
                            a.v0 += m0;
                            a.vN += mN;
                            a.has_add = true;
                        }
                    }
                    for (auto& [pid, a] : acc) {
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

                        struct DenseAccum {
                            float* values = nullptr;
                            float lo = 0.0f;
                            float hi = 1.0f;
                            bool has_replace = false;
                            bool has_add = false;
                        };
                        std::vector<DenseAccum> dense;
                        dense.reserve(rt.audio_rate_param_ids.size());
                        for (size_t pi = 0; pi < rt.audio_rate_param_ids.size(); ++pi) {
                            dense.push_back({
                                rt.audio_rate_param_data.data() + pi * block,
                                0.0f,
                                1.0f,
                                false,
                                false,
                            });
                        }

                        for (size_t ci = 0; ci < cg->connections.size(); ++ci) {
                            const auto& c = cg->connections[ci];
                            if (!c.audio_rate_modulation || c.dest_node != id) continue;
                            auto src_it = cg->runtime.find(c.source_node);
                            if (src_it == cg->runtime.end()) continue;
                            const int sport = static_cast<int>(c.source_port);
                            if (sport < 0
                                || sport >= (int)src_it->second.output_ptrs.size()) {
                                continue;
                            }

                            auto param_it = std::find(rt.audio_rate_param_ids.begin(),
                                                      rt.audio_rate_param_ids.end(),
                                                      c.automation_param_id);
                            if (param_it == rt.audio_rate_param_ids.end()) continue;
                            auto& dst = dense[static_cast<size_t>(
                                std::distance(rt.audio_rate_param_ids.begin(), param_it))];
                            const auto bounds = bounds_for_param(c.automation_param_id,
                                                                 c.automation_range_lo,
                                                                 c.automation_range_hi);
                            dst.lo = bounds.first;
                            dst.hi = bounds.second;

                            const float* src = src_it->second.output_ptrs[sport];
                            auto& dl = cg->connection_delays[ci];
                            if (dl.delay_samples <= 0 || dl.ring.empty()) {
                                for (int i = 0; i < num_samples; ++i) {
                                    const float value = map_modulation_sample(c, src[i]);
                                    if (c.automation_mix == AutomationMix::Replace) {
                                        dst.values[static_cast<size_t>(i)] = value;
                                        dst.has_replace = true;
                                    } else {
                                        dst.values[static_cast<size_t>(i)] += value;
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
                                        dst.values[static_cast<size_t>(i)] = value;
                                        dst.has_replace = true;
                                    } else {
                                        dst.values[static_cast<size_t>(i)] += value;
                                        dst.has_add = true;
                                    }
                                    if (++wp == ring_size) wp = 0;
                                    if (++rp == ring_size) rp = 0;
                                }
                                dl.write_pos = wp;
                            }
                        }

                        for (size_t pi = 0; pi < dense.size(); ++pi) {
                            const auto& d = dense[pi];
                            if (!d.has_replace && !d.has_add) continue;
                            const uint32_t param_id = rt.audio_rate_param_ids[pi];
                            const float lo = std::min(d.lo, d.hi);
                            const float hi = std::max(d.lo, d.hi);
                            for (int i = 0; i < num_samples; ++i) {
                                float value = d.values[static_cast<size_t>(i)];
                                if (d.has_add) value = std::clamp(value, lo, hi);
                                if (!push_parameter_event(param_events, param_id, i, value)) {
                                    break;
                                }
                            }
                        }
                    }
                    param_events.sort();
                }

                pit->second->process(out_view, in_c, rt.midi_in, rt.midi_out,
                                     param_events, num_samples);
                break;
            }
            case NodeType::Gain: {
                const float g = rt.gain;
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
            case NodeType::MidiOutput:
                break;
        }
    }

    // Capture each feedback source's current block for the *next* block.
    for (size_t ci = 0; ci < cg->connections.size(); ++ci) {
        const auto& c = cg->connections[ci];
        if (!c.feedback) continue;
        auto src_it = cg->runtime.find(c.source_node);
        if (src_it == cg->runtime.end()) continue;
        const auto& src_rt = src_it->second;
        const int sport = static_cast<int>(c.source_port);
        if (sport < 0 || sport >= static_cast<int>(src_rt.output_ptrs.size())) continue;
        auto& dl = cg->connection_delays[ci];
        if (dl.feedback_prev.empty()) continue;
        const float* src = src_rt.output_ptrs[sport];
        std::memcpy(dl.feedback_prev.data(), src,
                    sizeof(float) * static_cast<size_t>(num_samples));
    }

    // Drain MidiInput nodes' midi_out so events injected for THIS block
    // don't get re-consumed next block. inject_midi() runs before the
    // next process() call to refill them.
    for (auto& [nid, rt] : cg->runtime) {
        auto sit = cg->shapes.find(nid);
        if (sit != cg->shapes.end() && sit->second.type == NodeType::MidiInput) {
            rt.midi_out.clear();
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
    // compile_() calls. Also reflect into the live snapshot's runtime (safe:
    // the audio thread reads it once per block as a plain float load) so the
    // change takes effect without a re-prepare.
    auto* n = const_cast<GraphNode*>(node(id));
    if (!n) return false;
    n->gain = linear_gain;
    auto cg = std::atomic_load_explicit(&live_, std::memory_order_acquire);
    if (cg) {
        auto it = cg->runtime.find(id);
        if (it != cg->runtime.end()) it->second.gain = linear_gain;
    }
    return true;
}

float SignalGraph::node_gain(NodeId id) const {
    auto* n = node(id);
    if (!n) return 1.0f;
    return n->gain;
}

} // namespace pulp::host
