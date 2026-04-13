// Signal Graph implementation
// Directed acyclic graph for audio routing between nodes.
// Topological sort determines processing order.

#include <pulp/host/signal_graph.hpp>
#include <pulp/runtime/log.hpp>
#include <algorithm>
#include <queue>
#include <unordered_set>
#include <unordered_map>
#include <cstring>

namespace pulp::host {

NodeId SignalGraph::add_input_node(int channels, const std::string& name) {
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::AudioInput;
    node.name = name;
    node.num_output_ports = channels;
    nodes_.push_back(std::move(node));
    return nodes_.back().id;
}

NodeId SignalGraph::add_output_node(int channels, const std::string& name) {
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::AudioOutput;
    node.name = name;
    node.num_input_ports = channels;
    nodes_.push_back(std::move(node));
    return nodes_.back().id;
}

NodeId SignalGraph::add_plugin_node(const PluginInfo& info) {
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Plugin;
    node.name = info.name;
    node.num_input_ports = info.num_inputs;
    node.num_output_ports = info.num_outputs;
    node.plugin = PluginSlot::load(info);
    nodes_.push_back(std::move(node));
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
    node.plugin = std::move(slot);
    nodes_.push_back(std::move(node));
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
    return nodes_.back().id;
}

NodeId SignalGraph::add_midi_input_node(const std::string& name) {
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::MidiInput;
    node.name = name;
    node.num_output_ports = 1;
    nodes_.push_back(std::move(node));
    return nodes_.back().id;
}

NodeId SignalGraph::add_midi_output_node(const std::string& name) {
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::MidiOutput;
    node.name = name;
    node.num_input_ports = 1;
    nodes_.push_back(std::move(node));
    return nodes_.back().id;
}

bool SignalGraph::remove_node(NodeId id) {
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
        [id](const GraphNode& n) { return n.id == id; });
    if (it == nodes_.end()) return false;

    // Remove all connections involving this node
    connections_.erase(
        std::remove_if(connections_.begin(), connections_.end(),
            [id](const Connection& c) {
                return c.source_node == id || c.dest_node == id;
            }),
        connections_.end());

    nodes_.erase(it);
    return true;
}

bool SignalGraph::connect(NodeId source, PortIndex source_port,
                          NodeId dest, PortIndex dest_port) {
    // Verify nodes exist
    if (!node(source) || !node(dest)) return false;

    // Check for cycle
    if (would_create_cycle(source, dest)) return false;

    // Check for duplicate
    Connection conn{source, source_port, dest, dest_port};
    for (auto& c : connections_) {
        if (c == conn) return false;
    }

    connections_.push_back(conn);
    return true;
}

bool SignalGraph::connect_midi(NodeId source, NodeId dest) {
    if (!node(source) || !node(dest)) return false;
    if (would_create_cycle(source, dest)) return false;
    Connection conn{source, 0, dest, 0, false, true};
    for (auto& c : connections_) {
        if (c == conn && c.midi == conn.midi) return false;
    }
    connections_.push_back(conn);
    return true;
}

bool SignalGraph::inject_midi(NodeId id, const midi::MidiBuffer& events) {
    auto it = runtime_.find(id);
    if (it == runtime_.end()) return false;
    it->second.midi_out.clear();
    for (const auto& ev : events) it->second.midi_out.add(ev);
    return true;
}

bool SignalGraph::extract_midi(NodeId id, midi::MidiBuffer& out) const {
    auto it = runtime_.find(id);
    if (it == runtime_.end()) return false;
    for (const auto& ev : it->second.midi_in) out.add(ev);
    return true;
}

bool SignalGraph::connect_feedback(NodeId source, PortIndex source_port,
                                   NodeId dest, PortIndex dest_port) {
    if (!node(source) || !node(dest)) return false;
    Connection conn{source, source_port, dest, dest_port, true};
    for (auto& c : connections_) {
        if (c == conn) return false;  // duplicate
    }
    connections_.push_back(conn);
    return true;
}

bool SignalGraph::disconnect(NodeId source, PortIndex source_port,
                             NodeId dest, PortIndex dest_port) {
    Connection target{source, source_port, dest, dest_port};
    auto it = std::find(connections_.begin(), connections_.end(), target);
    if (it == connections_.end()) return false;
    connections_.erase(it);
    return true;
}

const GraphNode* SignalGraph::node(NodeId id) const {
    for (auto& n : nodes_) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

bool SignalGraph::has_path(NodeId from, NodeId to) const {
    // BFS to check reachability
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
            if (c.source_node == current) {
                queue.push(c.dest_node);
            }
        }
    }
    return false;
}

bool SignalGraph::would_create_cycle(NodeId source, NodeId dest) const {
    // Adding source→dest creates a cycle if there's already a path dest→source
    return has_path(dest, source);
}

std::vector<NodeId> SignalGraph::processing_order() const {
    // Kahn's algorithm for topological sort. Feedback connections are
    // invisible to the sort — they're the back-edges the user intentionally
    // introduced, and the runtime breaks them via a one-block delay.
    std::unordered_map<NodeId, int> in_degree;
    for (auto& n : nodes_) in_degree[n.id] = 0;

    for (auto& c : connections_) {
        if (c.feedback) continue;
        in_degree[c.dest_node]++;
    }

    std::queue<NodeId> queue;
    for (auto& [id, deg] : in_degree) {
        if (deg == 0) queue.push(id);
    }

    std::vector<NodeId> order;
    while (!queue.empty()) {
        auto current = queue.front();
        queue.pop();
        order.push_back(current);

        for (auto& c : connections_) {
            if (c.feedback) continue;
            if (c.source_node == current) {
                if (--in_degree[c.dest_node] == 0) {
                    queue.push(c.dest_node);
                }
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
    auto it = runtime_.find(id);
    if (it == runtime_.end()) return 0;
    return (int)it->second.input_latency;
}

void SignalGraph::compute_latencies_() {
    // Walk nodes in topological order; each node's input_latency is the max
    // of its inbound connections' source output_latency, and its
    // output_latency is that plus the node's own added latency (plugin
    // latency for Plugin nodes, 0 otherwise).
    for (NodeId id : cached_order_) {
        auto rt_it = runtime_.find(id);
        if (rt_it == runtime_.end()) continue;
        auto& rt = rt_it->second;

        int64_t max_upstream = 0;
        bool has_upstream = false;
        for (const auto& c : connections_) {
            if (c.dest_node != id) continue;
            if (c.feedback) continue;  // back-edge; one-block delay breaks it
            if (c.midi) continue;      // MIDI carries no audio latency
            auto src_it = runtime_.find(c.source_node);
            if (src_it == runtime_.end()) continue;
            max_upstream = std::max(max_upstream, src_it->second.output_latency);
            has_upstream = true;
        }
        rt.input_latency = has_upstream ? max_upstream : 0;

        int64_t added = 0;
        if (auto* n = node(id); n && n->plugin) {
            added = std::max(0, n->plugin->latency_samples());
        }
        rt.output_latency = rt.input_latency + added;
    }

    // Compute graph-wide latency: max input_latency across AudioOutput nodes.
    total_latency_samples_ = 0;
    for (auto& n : nodes_) {
        if (n.type != NodeType::AudioOutput) continue;
        auto it = runtime_.find(n.id);
        if (it == runtime_.end()) continue;
        total_latency_samples_ = std::max(total_latency_samples_, it->second.input_latency);
    }

    // Allocate per-connection delay lines to align inbound branches. Each
    // connection's delay brings the source output up to the dest node's
    // input_latency.
    connection_delays_.assign(connections_.size(), ConnectionDelay{});
    for (size_t i = 0; i < connections_.size(); ++i) {
        const auto& c = connections_[i];
        auto src_it = runtime_.find(c.source_node);
        auto dst_it = runtime_.find(c.dest_node);
        if (src_it == runtime_.end() || dst_it == runtime_.end()) continue;

        if (c.feedback) {
            // Feedback back-edge: allocate a one-block buffer that holds the
            // source's previous block's output. Destination reads from it
            // before source writes the current block; after the block we
            // overwrite with the just-computed source output.
            connection_delays_[i].feedback_prev.assign(
                static_cast<size_t>(max_block_size_), 0.0f);
            continue;
        }
        if (c.midi) continue;  // MIDI edges need no audio delay line

        int64_t want = dst_it->second.input_latency - src_it->second.output_latency;
        if (want < 0) want = 0;
        connection_delays_[i].delay_samples = (int)want;
        if (want > 0) {
            // Ring size = delay + max_block so one process() call can push
            // max_block samples and pull max_block delayed samples without
            // overlapping itself.
            connection_delays_[i].ring.assign(
                static_cast<size_t>((int64_t)max_block_size_ + want), 0.0f);
            connection_delays_[i].write_pos = 0;
        }
    }
}

bool SignalGraph::prepare(double sample_rate, int max_block_size) {
    if (max_block_size <= 0) return false;

    max_block_size_ = max_block_size;
    runtime_.clear();
    connection_delays_.clear();
    total_latency_samples_ = 0;

    for (auto& n : nodes_) {
        if (n.plugin) {
            if (!n.plugin->prepare(sample_rate, max_block_size)) {
                runtime::log_error("SignalGraph: failed to prepare plugin '{}'", n.name);
                return false;
            }
        }

        // Allocate scratch buffers sized to the largest port-count the node
        // will exercise. Allocating zero-sized channels is harmless; we just
        // leave the vectors empty.
        NodeRuntime rt;
        const int out_ch = std::max(0, n.num_output_ports);
        const int in_ch  = std::max(0, n.num_input_ports);
        rt.output_data.assign(static_cast<size_t>(out_ch) * max_block_size, 0.f);
        rt.input_data.assign(static_cast<size_t>(in_ch) * max_block_size, 0.f);
        rt.output_ptrs.resize(out_ch);
        rt.input_ptrs.resize(in_ch);
        for (int c = 0; c < out_ch; ++c) {
            rt.output_ptrs[c] = rt.output_data.data() + static_cast<size_t>(c) * max_block_size;
        }
        for (int c = 0; c < in_ch; ++c) {
            rt.input_ptrs[c] = rt.input_data.data() + static_cast<size_t>(c) * max_block_size;
        }
        runtime_[n.id] = std::move(rt);
    }

    cached_order_ = processing_order();
    compute_latencies_();
    prepared_ = true;
    return true;
}

void SignalGraph::release() {
    for (auto& n : nodes_) {
        if (n.plugin) n.plugin->release();
    }
    prepared_ = false;
    runtime_.clear();
    connection_delays_.clear();
    cached_order_.clear();
    total_latency_samples_ = 0;
}

void SignalGraph::process(audio::BufferView<float>& output,
                          const audio::BufferView<const float>& input,
                          int num_samples) {
    if (!prepared_ || num_samples <= 0 || num_samples > max_block_size_) return;

    // Clear the final destination; AudioOutput nodes accumulate into it.
    for (std::size_t c = 0; c < output.num_channels(); ++c) {
        std::memset(output.channel_ptr(c), 0, sizeof(float) * static_cast<size_t>(num_samples));
    }

    // Walk the cached topological order. For each node:
    //   1. Zero its input scratch.
    //   2. Sum each inbound connection from the source's output port.
    //   3. Produce output in the node's own output scratch, based on type.
    for (NodeId id : cached_order_) {
        auto* n = node(id);
        if (!n) continue;
        auto rt_it = runtime_.find(id);
        if (rt_it == runtime_.end()) continue;
        auto& rt = rt_it->second;

        // 1. Zero input scratch.
        if (!rt.input_data.empty()) {
            std::memset(rt.input_data.data(), 0, rt.input_data.size() * sizeof(float));
        }
        rt.midi_in.clear();
        // MidiInput's midi_out is populated by inject_midi() before process()
        // and must survive; clear other nodes' midi_out so plugins/outputs
        // start each block empty.
        if (n->type != NodeType::MidiInput) rt.midi_out.clear();

        // 1b. Gather MIDI from MIDI-flagged inbound connections.
        for (const auto& c : connections_) {
            if (c.dest_node != id || !c.midi || c.feedback) continue;
            auto src_it = runtime_.find(c.source_node);
            if (src_it == runtime_.end()) continue;
            for (const auto& ev : src_it->second.midi_out) rt.midi_in.add(ev);
        }

        // 2. Gather inbound connections, pushing through per-connection
        //    delay lines so every inbound branch arrives aligned at this
        //    node's input_latency.
        for (size_t ci = 0; ci < connections_.size(); ++ci) {
            const auto& c = connections_[ci];
            if (c.dest_node != id) continue;
            if (c.midi) continue;  // MIDI gathered above, not audio
            const int dport = static_cast<int>(c.dest_port);
            if (dport < 0 || dport >= static_cast<int>(rt.input_ptrs.size())) continue;
            if (c.source_node == 0) continue;  // reserved sentinel

            auto src_rt_it = runtime_.find(c.source_node);
            if (src_rt_it == runtime_.end()) continue;
            const auto& src_rt = src_rt_it->second;
            const int sport = static_cast<int>(c.source_port);
            if (sport < 0 || sport >= static_cast<int>(src_rt.output_ptrs.size())) continue;

            float* dst = rt.input_ptrs[dport];
            const float* src = src_rt.output_ptrs[sport];
            auto& dl = connection_delays_[ci];
            if (c.feedback) {
                // Read the source's previous block's output — this is
                // available *before* the source processes the current block,
                // which is what makes cycles tractable. After the full pass
                // we overwrite feedback_prev with the now-current source
                // output (see the per-block update loop below).
                if (!dl.feedback_prev.empty()) {
                    for (int i = 0; i < num_samples; ++i) dst[i] += dl.feedback_prev[(size_t)i];
                }
                continue;
            }
            if (dl.delay_samples <= 0 || dl.ring.empty()) {
                // Zero-latency pass-through: sum source output into dest input.
                for (int i = 0; i < num_samples; ++i) dst[i] += src[i];
            } else {
                const int ring_size = (int)dl.ring.size();
                const int D = dl.delay_samples;
                // Push num_samples new samples in, read num_samples delayed
                // samples out. Read position lags write position by D frames.
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
        switch (n->type) {
            case NodeType::AudioInput: {
                // Copy from host `input` into this node's output ports.
                const int chs = std::min(
                    static_cast<int>(rt.output_ptrs.size()),
                    static_cast<int>(input.num_channels()));
                for (int c = 0; c < chs; ++c) {
                    std::memcpy(rt.output_ptrs[c], input.channel_ptr(c),
                                sizeof(float) * static_cast<size_t>(num_samples));
                }
                // If the node has more output ports than the host provides,
                // the extras stay zero (already zeroed by allocation).
                break;
            }
            case NodeType::Plugin: {
                if (!n->plugin) break;
                audio::BufferView<float> in_view(
                    rt.input_ptrs.data(),
                    rt.input_ptrs.size(),
                    static_cast<std::size_t>(num_samples));
                audio::BufferView<float> out_view(
                    rt.output_ptrs.data(),
                    rt.output_ptrs.size(),
                    static_cast<std::size_t>(num_samples));
                // Build a const input view without casting away constness in
                // process()'s signature — the plugin slot takes a
                // BufferView<const float> so wrap the same pointers.
                std::vector<const float*> in_const(rt.input_ptrs.begin(), rt.input_ptrs.end());
                audio::BufferView<const float> in_c(
                    in_const.data(), in_const.size(),
                    static_cast<std::size_t>(num_samples));
                // midi_in was gathered from upstream MIDI edges above. The
                // plugin populates midi_out; downstream MIDI destinations
                // pick it up in their own gather phase.
                rt.midi_out.clear();
                n->plugin->process(out_view, in_c, rt.midi_in, rt.midi_out, num_samples);
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
                // Sum this node's input scratch into the host `output` view.
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
                // MIDI routing lands in Phase 2 (events wired via the graph).
                break;
        }
    }

    // Capture each feedback source's current block for the *next* block's
    // reader. This runs after the full topological pass so it sees the
    // freshly computed output of each feedback source node.
    for (size_t ci = 0; ci < connections_.size(); ++ci) {
        const auto& c = connections_[ci];
        if (!c.feedback) continue;
        auto src_it = runtime_.find(c.source_node);
        if (src_it == runtime_.end()) continue;
        const auto& src_rt = src_it->second;
        const int sport = static_cast<int>(c.source_port);
        if (sport < 0 || sport >= static_cast<int>(src_rt.output_ptrs.size())) continue;
        auto& dl = connection_delays_[ci];
        if (dl.feedback_prev.empty()) continue;
        const float* src = src_rt.output_ptrs[sport];
        std::memcpy(dl.feedback_prev.data(), src,
                    sizeof(float) * static_cast<size_t>(num_samples));
    }
}

void SignalGraph::clear() {
    connections_.clear();
    nodes_.clear();
    runtime_.clear();
    cached_order_.clear();
    prepared_ = false;
    next_id_ = 1;
}

bool SignalGraph::set_node_gain(NodeId id, float linear_gain) {
    auto it = runtime_.find(id);
    if (it == runtime_.end()) return false;
    it->second.gain = linear_gain;
    return true;
}

float SignalGraph::node_gain(NodeId id) const {
    auto it = runtime_.find(id);
    if (it == runtime_.end()) return 1.0f;
    return it->second.gain;
}

} // namespace pulp::host
