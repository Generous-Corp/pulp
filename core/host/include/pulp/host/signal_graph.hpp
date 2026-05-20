#pragma once

// Signal Graph for Pulp Host
// A directed acyclic graph of audio processing nodes for routing audio
// between plugin slots, I/O, and utility nodes (gain, mix, split).
//
// Usage:
//   SignalGraph graph;
//   auto input = graph.add_input_node(2);
//   auto slot = graph.add_plugin_node(plugin_info);
//   auto output = graph.add_output_node(2);
//   graph.connect(input, 0, slot, 0);  // input port 0 → slot port 0
//   graph.connect(slot, 0, output, 0);
//   graph.prepare(48000, 512);
//   graph.process(output_buffer, input_buffer, num_samples);

#include <pulp/host/plugin_slot.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <atomic>
#include <memory>
#include <vector>
#include <unordered_map>
#include <string>
#include <string_view>
#include <cstdint>

namespace pulp::host {

// ── Node types ──────────────────────────────────────────────────────────

enum class NodeType {
    AudioInput,    // System audio input
    AudioOutput,   // System audio output
    Plugin,        // Plugin slot (VST3/AU/CLAP)
    Gain,          // Simple gain utility
    MidiInput,     // System MIDI input
    MidiOutput,    // System MIDI output
    Custom,        // String-keyed extension node
};

using NodeId = uint32_t;
using PortIndex = uint32_t;

struct CustomNodeType {
    std::string type_id;
    int version = 1;
    int num_input_ports = 0;
    int num_output_ports = 0;
    std::string default_name;
};

// ── Connection ──────────────────────────────────────────────────────────

enum class AutomationMix : uint8_t {
    Replace = 0,  // default; graph refuses a 2nd Replace edge to same (node,param)
    Add     = 1,  // summed with other Add edges, clamped to param range
};

struct Connection {
    NodeId source_node;
    PortIndex source_port;
    NodeId dest_node;
    PortIndex dest_port;      // audio: dest port index; automation: ignored
    bool feedback = false;    // back-edge: reads previous block's audio, breaks
                              // the cycle for topological sort and PDC.
    bool midi = false;        // event-edge: routes MidiBuffer events instead of
                              // audio samples. Ports are ignored.
    bool automation = false;  // automation-edge: source audio drives a param on
                              // the dest plugin.
    bool audio_rate_modulation = false; // dense CV edge into an AudioRate param.

    // Parameter-modulation fields (valid when automation or
    // audio_rate_modulation is true).
    uint32_t automation_param_id  = 0;
    float automation_range_lo     = 0.0f;  // plain param domain
    float automation_range_hi     = 1.0f;  // plain param domain
    float automation_smoothing_ms = 0.0f;  // per-source pre-mix slew
    AutomationMix automation_mix  = AutomationMix::Replace;

    bool operator==(const Connection& o) const {
        return source_node == o.source_node && source_port == o.source_port
            && dest_node == o.dest_node && dest_port == o.dest_port
            && automation == o.automation
            && audio_rate_modulation == o.audio_rate_modulation
            && ((automation || audio_rate_modulation)
                ? automation_param_id == o.automation_param_id : true);
    }
};

// ── Graph Node ──────────────────────────────────────────────────────────

struct GraphNode {
    NodeId id;
    NodeType type;
    std::string name;
    int num_input_ports = 0;
    int num_output_ports = 0;

    // For Plugin nodes, the loaded plugin slot. Held as shared_ptr so that
    // published CompiledGraph snapshots can keep the plugin alive while the
    // audio thread is still referencing a now-stale snapshot.
    std::shared_ptr<PluginSlot> plugin;

    // For Plugin nodes, the identity used to load it. Preserved even when
    // the slot itself is null (e.g., plugin missing on this machine after a
    // .pulpgraph load) so subsequent serializations retain the identity for
    // later re-resolution.
    PluginInfo plugin_info;

    // UI-thread-owned scalar state that needs to survive snapshot
    // recompilation. compile_() copies these into per-snapshot NodeRuntime.
    float gain = 1.0f;

    // For Custom nodes, the registry identity that created the node. The
    // version is serialized with the graph so older custom topologies can be
    // distinguished from newer incompatible factories.
    std::string custom_type_id;
    int custom_type_version = 0;
};

// ── Signal Graph ────────────────────────────────────────────────────────

class SignalGraph {
public:
    SignalGraph() = default;

    // Add nodes — returns the node ID
    NodeId add_input_node(int channels, const std::string& name = "Input");
    NodeId add_output_node(int channels, const std::string& name = "Output");
    NodeId add_plugin_node(const PluginInfo& info);
    NodeId add_unresolved_plugin_node(const PluginInfo& info,
                                      int num_inputs, int num_outputs,
                                      const std::string& name);

    // Add a plugin node wrapping a caller-provided slot. Useful for tests
    // (mock latency, mock processing) and for hosts that build their own
    // PluginSlot implementations outside of PluginSlot::load().
    NodeId add_plugin_node(std::unique_ptr<PluginSlot> slot,
                           int num_inputs, int num_outputs,
                           const std::string& name = "Plugin");
    NodeId add_gain_node(const std::string& name = "Gain");
    NodeId add_midi_input_node(const std::string& name = "MIDI In");
    NodeId add_midi_output_node(const std::string& name = "MIDI Out");
    bool register_custom_node_type(CustomNodeType type);
    const CustomNodeType* custom_node_type(std::string_view type_id) const;
    NodeId add_custom_node(std::string_view type_id,
                           const std::string& name = {});
    NodeId add_unresolved_custom_node(std::string_view type_id,
                                      int version,
                                      int num_inputs,
                                      int num_outputs,
                                      const std::string& name);

    // Remove a node and all its connections
    bool remove_node(NodeId id);

    // Connect two nodes (port-to-port)
    bool connect(NodeId source, PortIndex source_port,
                 NodeId dest, PortIndex dest_port);

    // Connect with an explicit one-block delay. Permitted to close a cycle
    // (the back-edge the user is intentionally introducing) and invisible to
    // topological sort. The destination reads the source's previous-block
    // output, giving the feedback loop a block-sized delay.
    bool connect_feedback(NodeId source, PortIndex source_port,
                          NodeId dest, PortIndex dest_port);

    // MIDI connection: routes events from source's MIDI output into dest's
    // MIDI input. Ports are ignored (MIDI is node-scoped, not port-scoped).
    // Participates in cycle detection and topological sort the same way as
    // audio connections.
    bool connect_midi(NodeId source, NodeId dest);

    // Automation connection: the audio samples on `src`'s output port drive
    // `dest`'s parameter `dest_param_id`. Two control points per block (first
    // + last sample) are delivered to the plugin via
    // PluginSlot::process()'s ParameterEventQueue so plugins can interpolate
    // sample-accurately.
    //
    // Source values are clamped to [0,1] then mapped linearly to
    // [range_lo, range_hi] in the plugin's plain parameter domain.
    //
    // smoothing_ms applies a per-source linear slew before mixing (TODO).
    // MixMode::Replace is the default; a second Replace edge targeting the
    // same (dest, param) is rejected. MixMode::Add sums multiple edges,
    // then clamps to the param's range.
    bool connect_automation(NodeId src, PortIndex src_audio_port,
                            NodeId dest, uint32_t dest_param_id,
                            float range_lo, float range_hi,
                            float smoothing_ms = 0.0f,
                            AutomationMix mix = AutomationMix::Replace);

    // Audio-rate modulation connection: source audio samples drive every
    // sample of an AudioRate destination parameter. This edge is distinct
    // from sparse automation above; it is accepted only for continuous
    // automatable parameters whose HostParamInfo::rate is AudioRate.
    bool connect_audio_rate_modulation(NodeId src, PortIndex src_audio_port,
                                       NodeId dest, uint32_t dest_param_id,
                                       float range_lo, float range_hi,
                                       float smoothing_ms = 0.0f,
                                       AutomationMix mix = AutomationMix::Replace);

    // Inject a MIDI buffer into a MidiInput source node. Call before
    // process(); the events become that node's MIDI output this block.
    bool inject_midi(NodeId midi_input_node, const midi::MidiBuffer& events);

    // Drain the MIDI events that arrived at a MidiOutput sink node during
    // the last process() call. Appends to `out`.
    bool extract_midi(NodeId midi_output_node, midi::MidiBuffer& out) const;

    // Disconnect
    bool disconnect(NodeId source, PortIndex source_port,
                    NodeId dest, PortIndex dest_port);

    // Query
    const GraphNode* node(NodeId id) const;
    const std::vector<GraphNode>& nodes() const { return nodes_; }
    const std::vector<Connection>& connections() const { return connections_; }

    // Check if connecting would create a cycle
    bool would_create_cycle(NodeId source, NodeId dest) const;

    // Compute processing order (topological sort)
    std::vector<NodeId> processing_order() const;

    // Lifecycle
    bool prepare(double sample_rate, int max_block_size);
    void release();

    // Process one block of audio through the graph
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 int num_samples);

    // Clear all nodes and connections
    void clear();

    // Gain for a Gain node (linear, not dB). Defaults to 1.0.
    bool set_node_gain(NodeId id, float linear_gain);
    float node_gain(NodeId id) const;

    // Latency in samples from any AudioInput to the graph's AudioOutput, as
    // computed by prepare(). Reflects plugin-reported latencies plus any
    // delay inserted by PDC. Returns 0 when not prepared.
    int latency_samples() const { return (int)total_latency_samples_.load(std::memory_order_relaxed); }

    // Latency arriving at a specific node's input (samples). Returns 0 when
    // the node is unknown or the graph is not prepared.
    int node_latency_samples(NodeId id) const;

    // Set a parameter on a Plugin node at the graph level. The call is
    // forwarded to PluginSlot::set_parameter(). Returns false if the node
    // is not a Plugin node or has no loaded slot. This is the single-value
    // knob — full automation-curve routing (one node's output driving
    // another node's parameter over a block) is follow-up work.
    bool set_node_parameter(NodeId id, uint32_t param_id, float value);

    // Read a parameter's current value from a Plugin node (returns 0.0f if
    // the node is not a Plugin or has no slot).
    float get_node_parameter(NodeId id, uint32_t param_id) const;

private:
    struct NodeRuntime {
        // Per-node output-port channel storage (interleaved per-port, flat).
        // data_ has size num_output_ports * max_block_size_; channel_ptrs_[p]
        // points at data_[p * max_block_size_].
        std::vector<float> output_data;
        std::vector<float*> output_ptrs;
        // Per-node input-port scratch — callers write into these before the
        // node processes, then zero before the next block.
        std::vector<float> input_data;
        std::vector<float*> input_ptrs;
        float gain = 1.0f;

        // PDC: cumulative samples of latency from AudioInput to this node's
        // input ports (input_latency) and output ports (output_latency).
        // output_latency = input_latency + (plugin->latency_samples() for
        // Plugin nodes, 0 otherwise).
        int64_t input_latency = 0;
        int64_t output_latency = 0;

        struct ParamBounds {
            uint32_t id = 0;
            float min_value = 0.0f;
            float max_value = 1.0f;
        };
        std::vector<ParamBounds> param_bounds;

        // MIDI scratch. Cleared at the start of each process() call except
        // for MidiInput nodes, whose midi_out is populated by inject_midi()
        // and must survive the process() entry-clear.
        midi::MidiBuffer midi_in;
        midi::MidiBuffer midi_out;

        // Audio-rate modulation scratch. Each listed param gets one
        // max-block-sized slice in audio_rate_param_data, filled immediately
        // before the destination plugin processes.
        std::vector<uint32_t> audio_rate_param_ids;
        std::vector<float> audio_rate_param_data;
    };

    // One delay line per graph connection, parallel to connections_. Used to
    // align branch latencies so a node receives all its inbound audio with a
    // common alignment at input_latency samples.
    struct ConnectionDelay {
        int delay_samples = 0;
        // Ring buffer: delay_samples + max_block_size_ frames per source
        // channel. Empty when delay_samples == 0 (pass-through path).
        std::vector<float> ring;
        int write_pos = 0;
        // Feedback edges hold the previous block's source-port audio so the
        // destination can read it before the source writes the current block.
        std::vector<float> feedback_prev;  // size = max_block_size_
    };

    // CompiledGraph — immutable audio-thread-safe snapshot of the graph
    // after prepare(). Published via atomic<shared_ptr> so topology mutations
    // on the UI thread never tear state the audio thread is reading.
    //
    // Mutation protocol:
    //   1. UI thread changes nodes_ / connections_ / etc. (NOT snapshot state).
    //   2. Snapshot is atomically reset to nullptr, making process() return
    //      silence until the caller invokes prepare() again.
    //   3. prepare() rebuilds a fresh CompiledGraph and atomic-swaps it in.
    //
    // Because the snapshot owns its own copies of connections / delays / per
    // node runtime AND holds shared_ptr<PluginSlot>, it's safe to read even
    // if GraphNode owners are mutated before the audio thread releases its
    // reference. The old snapshot is destroyed only when both threads let go.
    struct CompiledGraph {
        std::vector<NodeId> order;
        std::vector<Connection> connections;
        std::vector<ConnectionDelay> connection_delays;  // parallel to connections
        // Runtime + plugin + node-info keyed by NodeId so we don't rely on
        // pointers into an outer container.
        std::unordered_map<NodeId, NodeRuntime> runtime;
        std::unordered_map<NodeId, std::shared_ptr<PluginSlot>> plugins;
        struct NodeShape {
            NodeType type;
            int num_input_ports;
            int num_output_ports;
        };
        std::unordered_map<NodeId, NodeShape> shapes;
        int max_block_size = 0;
        int64_t total_latency_samples = 0;
    };

    std::vector<GraphNode> nodes_;
    std::vector<Connection> connections_;
    std::unordered_map<std::string, CustomNodeType> custom_node_types_;
    NodeId next_id_ = 1;

    // Audio-thread snapshot, published by prepare() / mutators. Uses the
    // deprecated-but-supported std::atomic_store/atomic_load free-function
    // overloads on shared_ptr because libc++ in our toolchain does not yet
    // specialize std::atomic<std::shared_ptr<T>>. Acquire/release semantics.
    std::shared_ptr<CompiledGraph> live_;
    std::atomic<int64_t> total_latency_samples_{0};  // reflected for const-query access

    bool has_path(NodeId from, NodeId to) const;
    std::shared_ptr<CompiledGraph> compile_(double sample_rate, int max_block_size);
    void invalidate_live_();
    static void compute_latencies_for_(CompiledGraph& cg,
                                       const std::vector<Connection>& connections);
};

} // namespace pulp::host
