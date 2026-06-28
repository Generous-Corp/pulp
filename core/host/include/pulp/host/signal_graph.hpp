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

#include <pulp/host/anticipation_lane.hpp>
#include <pulp/host/graph_types.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/load_measurer.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>
#include <pulp/runtime/budget_policy.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/state/modulation_lane.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <string>
#include <string_view>
#include <cstdint>
#include <cstddef>

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

using CustomNodeProcessFn = std::function<void(audio::BufferView<float>& output,
                                              const audio::BufferView<const float>& input,
                                              int num_samples)>;

// Transport-aware custom-node callback (additive). Identical to
// CustomNodeProcessFn plus the host transport for the block. A custom type that
// registers one of the transport-aware callbacks below is treated as
// transport-sensitive: its routed binding forwards the live transport and it is
// excluded from the anticipation interior so it always runs live (see
// GraphNode::transport_sensitive).
using CustomNodeTransportProcessFn =
    std::function<void(audio::BufferView<float>& output,
                       const audio::BufferView<const float>& input,
                       int num_samples,
                       const format::ProcessContext& transport)>;

struct CustomNodeType {
    std::string type_id;
    int version = 1;
    int num_input_ports = 0;
    int num_output_ports = 0;
    std::string default_name;
    CustomNodeProcessFn process;  // stateless (used when `create` is empty)

    // Optional stateful lifecycle.
    // When `create` is set, the graph owns ONE opaque instance per node (RAII
    // via `destroy`). `process_instance` runs instead of `process`, and
    // prepare/release/reset/save_state/load_state operate on that instance. All
    // empty == today's stateless process-only node, byte-for-byte unchanged
    // (no instance is created and no state is serialized).
    //
    // Threading mirrors PluginSlot: create/prepare/release/save_state/load_state
    // are called on the UI/main thread (never from process()); process_instance
    // runs on the audio thread and must be real-time-safe. As with plugin
    // state, call save_state/load_state from non-audio control paths (graph not
    // live, or after invalidate + re-prepare).
    //
    // Parallel routing concurrency: the framework owns one DISTINCT instance per
    // node, so two nodes of the same type never share instance state. But under
    // levelized parallel routing, process_instance for sibling nodes in a level
    // may run on different worker threads at once — so a callback that touches
    // state SHARED across nodes (e.g. a captured global, not the per-instance
    // pointer) must itself be concurrent-safe, or that graph must not enable
    // parallel routing. The per-instance `void*` is always single-threaded.
    std::function<void*()> create;
    std::function<void(void* /*instance*/)> destroy;
    std::function<void(void* /*instance*/, double /*sample_rate*/, int /*max_block*/)> prepare;
    std::function<void(void* /*instance*/)> release;
    std::function<void(void* /*instance*/)> reset;
    std::function<void(void* /*instance*/, audio::BufferView<float>& /*output*/,
                       const audio::BufferView<const float>& /*input*/,
                       int /*num_samples*/)>
        process_instance;
    std::function<std::vector<uint8_t>(void* /*instance*/)> save_state;
    std::function<bool(void* /*instance*/, const std::vector<uint8_t>& /*bytes*/)> load_state;

    // Optional transport-aware callbacks (additive opt-in). When either is set,
    // the node is transport-sensitive: the graph forwards the host transport to
    // it and excludes it from the anticipation interior so it always runs live.
    // `process_transport` is the stateless variant (used when `create` is empty);
    // `process_instance_transport` is the stateful variant (leading instance
    // pointer, used when an instance exists). Both default-empty == today's
    // transport-unaware node, byte-for-byte unchanged. A type that sets a
    // transport-aware callback alongside its plain `process`/`process_instance`
    // gets the transport variant on the routed path; the plain one remains the
    // fallback when no transport is available for the block.
    CustomNodeTransportProcessFn process_transport;  // stateless
    std::function<void(void* /*instance*/, audio::BufferView<float>& /*output*/,
                       const audio::BufferView<const float>& /*input*/,
                       int /*num_samples*/, const format::ProcessContext& /*transport*/)>
        process_instance_transport;
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
    bool sidechain = false;   // sidechain-edge: like a normal audio edge, but
                              // routes into one of the destination plugin's
                              // sidechain-bus input ports. The
                              // topological-sort + PDC treat sidechain as a
                              // hard edge — it is not a back-edge.

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
            && sidechain == o.sidechain
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

    // Opaque state for a stateful custom node. `custom_instance` is the live
    // per-node object (RAII via the type's destroy), created on the UI
    // thread in prepare() and captured into each compiled snapshot like a
    // plugin shared_ptr so old audio snapshots stay alive. `custom_state_blob`
    // is the serialized form, preserved even when the type is unresolved (so a
    // round-trip through .pulpgraph keeps the state). `custom_state_pending`
    // marks a freshly-loaded blob to apply to the instance exactly once.
    std::shared_ptr<void> custom_instance;
    std::vector<uint8_t> custom_state_blob;
    bool custom_state_pending = false;

    // Cached, prepare-stable mirror of this node's transport-sensitivity
    // capability. Resolved ONCE in compile_() — for a Plugin node from its
    // slot's PluginSlot::wants_transport(), for a Custom node from whether its
    // type registered a transport-aware callback — BEFORE the anticipation
    // eligibility analysis runs, and read by BOTH the anticipation analyzer
    // (which seeds AnticipationExclusion::TransportSensitive on a true value) and
    // the routed binding resolution (which forwards the live transport to a
    // true-valued node). The single shared read guarantees the partition and the
    // bindings can never disagree. Prepare-stable: if a node's capability could
    // change after compile, that requires a re-prepare for the graph to observe
    // it — process() never re-polls the live slot per block.
    bool transport_sensitive = false;
};

// ── Signal Graph ────────────────────────────────────────────────────────

class SignalGraph {
public:
    struct GraphLimits {
        std::size_t max_nodes = 4096;
        std::size_t max_connections = 16384;
        std::size_t max_ports = 32768;
        int max_block_size = 16384;
        // Deterministic generated-graph work-unit budget. This is not a
        // hardware CPU-cycle estimate; it is a stable shape/block-size score
        // for importers that need to reject expensive generated graphs before
        // prepare(). Zero disables the budget.
        std::size_t max_estimated_work_units = 0;
    };

    enum class GeneratedGraphValidationRejectReason : uint8_t {
        None,
        InvalidBlockSize,
        MaxBlockSizeExceeded,
        NodeLimitExceeded,
        ConnectionLimitExceeded,
        PortLimitExceeded,
        EstimatedWorkExceeded,
    };

    struct GeneratedGraphValidation {
        bool accepted = true;
        GeneratedGraphValidationRejectReason reason =
            GeneratedGraphValidationRejectReason::None;
        std::size_t actual = 0;
        std::size_t limit = 0;
    };

    struct PreparedStats {
        std::size_t node_count = 0;
        std::size_t ordered_node_count = 0;
        std::size_t connection_count = 0;
        std::size_t total_ports = 0;
        int max_block_size = 0;
        std::size_t node_audio_buffer_bytes = 0;
        std::size_t automation_buffer_bytes = 0;
        std::size_t delay_buffer_bytes = 0;
        std::size_t total_prepared_buffer_bytes = 0;
    };

    struct RuntimeBudgetReport {
        runtime::RuntimeBudgetDecision decision{};
        runtime::RuntimeBudgetFrameStats frame_stats{};
        std::uint64_t estimated_cost = 0;
        bool prepared = false;

        bool should_run_optional_work() const noexcept {
            return decision.should_run();
        }
    };

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
    // Registers or replaces a custom type. If existing nodes use the same
    // `(type_id, version)`, the live snapshot is invalidated so prepare()
    // can rebuild with the matching process callback. Shape mismatches keep
    // placeholder passthrough semantics instead of attaching the callback.
    bool register_custom_node_type(CustomNodeType type);
    const CustomNodeType* custom_node_type(std::string_view type_id) const;
    const CustomNodeType* custom_node_type(std::string_view type_id,
                                           int version) const;
    NodeId add_custom_node(std::string_view type_id,
                           const std::string& name = {});
    NodeId add_custom_node(std::string_view type_id,
                           int version,
                           const std::string& name = {});
    NodeId add_unresolved_custom_node(std::string_view type_id,
                                      int version,
                                      int num_inputs,
                                      int num_outputs,
                                      const std::string& name);

    // Opaque per-node state for stateful custom nodes.
    // custom_node_state() returns the live instance's save_state() when the node
    // is resolved + stateful, else the last-loaded blob (preserved for
    // unresolved nodes). Empty when `id` is not a custom node or has no state.
    // set_custom_node_state() stores the blob to apply to the instance on the
    // next prepare(); returns false when `id` is not a custom node. Both run on
    // the UI/main thread — never the audio thread.
    std::vector<uint8_t> custom_node_state(NodeId id) const;
    bool set_custom_node_state(NodeId id, const std::vector<uint8_t>& bytes);

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

    // Sidechain connection: routes a source's audio output port into the
    // destination plugin's sidechain bus port. The destination
    // port is `dest_sidechain_port` — the caller supplies the absolute
    // port index on the destination node (the host knows how many main
    // input ports a plugin exposes via PluginInfo::num_inputs; sidechain
    // ports follow main inputs). The flag does NOT change topological
    // ordering — sidechain participates in cycle detection and PDC the
    // same as a normal audio edge — but it is tagged so UIs, serializers,
    // and per-format adapters can recognise the role.
    //
    // Tagging is metadata: the actual routing still uses (source, source
    // port) → (dest, dest sidechain port). Calling this is equivalent to
    // connect() with an extra flag, plus a guard that the destination is
    // a Plugin node (sidechain only makes sense on plugins).
    bool connect_sidechain(NodeId source, PortIndex source_port,
                           NodeId dest, PortIndex dest_sidechain_port);

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
    // from sparse automation above; it is accepted only when the edge can be
    // represented as a valid GraphNode-scoped state::ModulationLane targeting
    // a continuous AudioRate destination parameter.
    bool connect_audio_rate_modulation(NodeId src, PortIndex src_audio_port,
                                       NodeId dest, uint32_t dest_param_id,
                                       float range_lo, float range_hi,
                                       float smoothing_ms = 0.0f,
                                       AutomationMix mix = AutomationMix::Replace);

    // Project an accepted graph audio-rate modulation edge into the typed
    // modulation-lane contract used by instruments, adapters, and generated
    // graphs. Returns false for non-modulation edges or unresolved metadata.
    bool audio_rate_modulation_lane(const Connection& connection,
                                    state::ModulationLane& lane) const;

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

    // Prepare-time topology bounds. Hosts that accept generated or user-built
    // graphs can lower these before prepare() so oversized graphs fail before
    // snapshot allocation or plugin prepare.
    void set_limits(GraphLimits limits);
    GraphLimits limits() const { return limits_; }
    std::size_t estimate_generated_graph_work_units(int max_block_size) const;
    GeneratedGraphValidation validate_generated_graph(int max_block_size) const;
    PreparedStats prepared_stats() const;

    // Per-node CPU-load telemetry, accumulated by process() and read from the
    // control/UI thread. process() wraps each node's work in an
    // AudioProcessLoadMeasurer begin()/end() (relaxed-atomic, RT-safe); these
    // measurers persist across re-prepare() so a node's load history survives
    // topology recompiles. Returns one entry per currently-present node that
    // has been prepared (removed nodes' lingering measurers are filtered out),
    // each a latest-value snapshot (may mix adjacent callbacks). Safe to poll
    // while the audio thread runs.
    struct NodeLoadReport {
        NodeId node_id = 0;
        audio::AudioProcessLoadSnapshot load;
    };
    std::vector<NodeLoadReport> node_loads() const;

    // ── Canonical-executor migration ─────────────────────────────────────
    // Hooks the SignalGraph→GraphRuntimeExecutor translation needs without
    // exposing the private CompiledGraph. The translation lives in
    // signal_graph_executor_routing.{hpp,cpp}; these are control-thread only.

    // True once prepare() has published a live compiled snapshot.
    bool is_prepared() const noexcept { return live_ != nullptr; }
    // Max block size the live snapshot was prepared for (0 if not prepared).
    int prepared_max_block_size() const noexcept;
    // The live compiled snapshot's per-node gain atomic (Gain nodes only), or
    // nullptr. The pointer stays valid only while the snapshot returned by
    // live_snapshot_handle() is retained AND no re-prepare has occurred; a
    // routing built from it must be rebuilt after the graph recompiles.
    std::atomic<float>* live_gain_atomic(NodeId id) const noexcept;
    // The live compiled snapshot's PluginSlot for a Plugin node, or nullptr.
    // Same lifetime contract as live_gain_atomic: valid only while
    // live_snapshot_handle() is retained and no re-prepare has occurred.
    PluginSlot* live_plugin_slot(NodeId id) const noexcept;
    // The live compiled snapshot's resolved process callback for a Custom node,
    // or nullptr when the node is unresolved (unregistered type / shape
    // mismatch). The pointee is the snapshot's own callback (which keeps any
    // stateful instance alive); same lifetime contract as live_plugin_slot.
    const CustomNodeProcessFn* live_custom_processor(NodeId id) const noexcept;
    // The live compiled snapshot's transport-aware custom callback for a Custom
    // node, or nullptr when the node's type registered none. Same lifetime
    // contract as live_custom_processor; non-null iff that node's
    // GraphNode::transport_sensitive was resolved true.
    const CustomNodeTransportProcessFn* live_custom_transport_processor(
        NodeId id) const noexcept;
    // Opaque keepalive for the live compiled snapshot so a translated routing
    // can pin the lifetime of the gain atomics + plugin slots it references.
    std::shared_ptr<const void> live_snapshot_handle() const noexcept;

    // Controls whether the audio callback drives the canonical
    // GraphRuntimeExecutor when the live snapshot is executor-eligible. Default
    // ON: the routed executor is the primary inter-node backend. Set it OFF to
    // force the legacy walk — the parity tests do this to keep the walk as an
    // independent reference oracle. Ineligible graphs always fall back to the
    // legacy walk regardless of this flag. The flag is a control-thread toggle read
    // relaxed on the audio thread. The two paths produce bit-identical output
    // per block for eligible graphs, so toggling mid-stream is RT-safe and
    // seamless for feedforward graphs. For graphs with FEEDBACK, the legacy walk
    // and the executor keep INDEPENDENT one-block-delay state (the legacy
    // ConnectionDelay::feedback_prev vs the executor's per-edge prev slot), so a
    // mid-stream switch resets feedback history to whatever the destination path
    // last wrote — a one-block transient, not a crash or a permanent divergence.
    // Pick a path before starting a feedback graph and keep it for the stream.
    void set_canonical_executor_routing_enabled(bool enabled) noexcept {
        canonical_executor_routing_enabled_.store(enabled, std::memory_order_relaxed);
    }
    // Returns the requested opt-in flag, not a promise that the current block
    // is taking the executor path. Actual dispatch also requires an eligible
    // prepared snapshot and a scratch pool sized for the block.
    bool canonical_executor_routing_enabled() const noexcept {
        return canonical_executor_routing_enabled_.load(std::memory_order_relaxed);
    }

    // Opt into the LEVELIZED PARALLEL executor for eligible graphs (default OFF,
    // independent of the serial executor opt-in). Enable BEFORE prepare(): the
    // parallel-safe routing snapshot + levelization are built at compile time and
    // the worker pool is started there, so flipping this on after prepare() has
    // no effect until the next prepare(). When enabled + eligible + the pool is
    // running + the parallel pool fits the block, process() routes through
    // GraphRuntimeExecutor::process_parallel; otherwise it falls back to the
    // serial executor (if its opt-in is set) and then the legacy walk. Output is
    // bit-identical across all three paths.
    void set_parallel_routing_enabled(bool enabled) noexcept {
        parallel_routing_enabled_.store(enabled, std::memory_order_relaxed);
    }
    bool parallel_routing_enabled() const noexcept {
        return parallel_routing_enabled_.load(std::memory_order_relaxed);
    }
    // Break-even threshold for the parallel executor. A level runs across the
    // worker pool only when its static work-weight x frame count reaches this
    // many channel-samples; lower-cost levels stay serial to avoid fork/join
    // overhead. Default 0 preserves the original "parallelize every eligible
    // level" behavior. This is an RT-safe atomic setting and can be changed
    // before or after prepare().
    void set_parallel_min_work_units(std::uint64_t channel_samples) noexcept {
        executor_.set_parallel_min_work_units(channel_samples);
    }
    std::uint64_t parallel_min_work_units() const noexcept {
        return executor_.parallel_min_work_units();
    }
    // Diagnostic executor counters for verifying whether routed processing
    // actually took the parallel/serial executor paths. This is telemetry, not a
    // synchronization primitive; values may mix adjacent audio blocks.
    format::GraphRuntimeExecutorStats routing_executor_stats() const noexcept {
        return executor_.stats();
    }

    // Count of blocks where a routed path was ELIGIBLE (canonical/parallel routing
    // enabled, a valid routed snapshot, and the pool fit the block) yet routed
    // dispatch returned failure, so process() silently fell back to the legacy
    // walk. The walk is BOTH the reference oracle and the fallback, so this
    // degradation is invisible to the routed-vs-walk parity test — this counter is
    // the only signal that an eligible graph stopped routing. Stays 0 in healthy
    // operation; a non-zero value means the routed path is failing for graphs it
    // should handle. Relaxed-atomic, written only by the audio thread.
    std::uint64_t routed_walk_fallbacks() const noexcept {
        return routed_walk_fallbacks_.load(std::memory_order_relaxed);
    }

    // Opt into ANTICIPATIVE RENDERING for eligible graphs (default OFF). When
    // enabled + the canonical-executor routing is eligible + the graph has an
    // anticipation-eligible latent interior (no live input / feedback / sidechain
    // dependency — see analyze_anticipation_eligibility), compile_ carves that
    // interior into an AnticipationLane that is pre-rendered AHEAD of the deadline
    // off the audio thread; process() then consumes the pre-rendered boundary
    // signals and runs the rest of the graph with the interior masked, absorbing
    // the interior's CPU cost off the critical block. Requires the canonical
    // executor path (set_canonical_executor_routing_enabled). Enable BEFORE prepare().
    //
    // Output is bit-identical to the canonical (interior-live) render WHEN the host
    // pumps enough and uses a fixed block size equal to the prepared max block. Two
    // intrinsic exceptions: (1) a block whose size differs from the prepared max, or
    // a ring underrun, silences the interior for that block (it is never re-rendered
    // live — see the producer-ownership note below); (2) a parameter/gain change on
    // an interior node takes effect at render-ahead time, i.e. up to a lead's-worth
    // of blocks earlier than in a live render.
    //
    // The interior's plugin state is advanced ONLY by the anticipation producer
    // (pump_anticipation), never by process() — so the interior is always masked
    // on the live path; an underrun yields silence for that block, never a live
    // re-render (which would double-advance the producer-owned state).
    //
    // SAFETY NOTE: a host-clock-sensitive node (one whose output depends on the
    // transport playhead) is NOT detectable from topology alone, so it opts in
    // via PluginSlot::wants_transport() / a transport-aware custom callback. That
    // capability is resolved once at compile into GraphNode::transport_sensitive
    // and SEEDS AnticipationExclusion::TransportSensitive in
    // analyze_anticipation_eligibility, which excludes the node AND its whole
    // downstream cone from the ahead-rendered interior. A transport-sensitive
    // node therefore always runs live/exterior, never ahead — so the live
    // transport can stay populated on every block (the masked interior nodes are
    // transport-insensitive by construction and ignore it). This per-node
    // exclusion replaces the former blanket transport suppression under
    // anticipation; transport_suppressed_for_anticipation() now counts the
    // transport-sensitive nodes that anticipation forced exterior.
    void set_anticipation_enabled(bool enabled) noexcept {
        anticipation_enabled_.store(enabled, std::memory_order_relaxed);
    }
    bool anticipation_enabled() const noexcept {
        return anticipation_enabled_.load(std::memory_order_relaxed);
    }
    // Producer pump (OFF the audio thread): render the live graph's anticipation
    // interior ahead into its ring, up to `max_blocks` blocks. The host calls this
    // from a single background/idle thread (it is the lane's sole producer; a
    // concurrent/reentrant call is a guarded no-op). A no-op when anticipation is
    // disabled or the live graph has no eligible interior. Returns the number of
    // blocks rendered ahead this call.
    //
    // CONTRACT: the host MUST stop calling pump_anticipation and JOIN its producer
    // thread before any prepare()/graph mutation. The pump renders the interior's
    // plugin instances, which prepare() reinitializes and re-binds; running them
    // concurrently is a data race (the snapshot reader-pin protects object lifetime,
    // not plugin-state exclusivity). Same discipline as "no process() during
    // prepare()". Re-prepare also resets the lead buffer — expect a brief transient.
    int pump_anticipation(int max_blocks = 8);

    RuntimeBudgetReport evaluate_optional_runtime_budget(
        runtime::RuntimeBudgetFrame& frame,
        runtime::RuntimeWorkLane lane = runtime::RuntimeWorkLane::Background,
        bool required = false) const noexcept;

    // Process one block of audio through the graph
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 int num_samples);

    // Process one block with host transport/timeline context. Additive overload
    // of the no-transport process() above: the supplied transport is delivered to
    // the nodes that consume it (e.g. ProcessorNode, via the routed ProcessBlock's
    // transport pointer), so a Processor sees the host playhead, mode, and
    // render-speed hint for the block. Nodes that do not consume transport are
    // bit-identical to the no-transport overload. Under active anticipation the
    // transport stays live: transport-sensitive nodes are excluded from the
    // ahead-rendered interior (AnticipationExclusion::TransportSensitive) and run
    // exterior, so every interior node that IS ahead-rendered is
    // transport-insensitive by construction and ignores the populated transport
    // (see set_anticipation_enabled / transport_suppressed_for_anticipation).
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 int num_samples,
                 const format::ProcessContext& transport);

    // Count of transport-sensitive nodes that anticipation forced to run
    // exterior (excluded from the ahead-rendered interior so they observe the
    // live host transport). Resolved at compile when anticipation is active; the
    // former "transport suppressed per block" meaning is retired — the per-node
    // exclusion (AnticipationExclusion::TransportSensitive) replaced the blanket
    // suppression. Stays 0 unless a transport-sensitive node coexists with active
    // anticipation. Relaxed-atomic; written by compile_() on the control thread.
    std::uint64_t transport_suppressed_for_anticipation() const noexcept {
        return transport_suppressed_for_anticipation_.load(std::memory_order_relaxed);
    }

    // Clear all nodes and connections
    void clear();

    // Gain for a Gain node (linear, not dB). Defaults to 1.0. The setter is
    // a UI/control-thread API; when prepared, it also updates the live
    // snapshot's atomic gain so process() observes the change without
    // re-prepare. The getter reads UI-owned graph state and is UI-thread-only.
    bool set_node_gain(NodeId id, float linear_gain);
    float node_gain(NodeId id) const;

    // Latency in samples from any AudioInput to the graph's AudioOutput, as
    // computed by prepare(). Reflects plugin-reported latencies plus any
    // delay inserted by PDC. Returns 0 when not prepared.
    int latency_samples() const { return (int)total_latency_samples_.load(std::memory_order_relaxed); }

    // Latency arriving at a specific node's input (samples). Returns 0 when
    // the node is unknown or the graph is not prepared.
    int node_latency_samples(NodeId id) const;

    // Set a single parameter value on a Plugin node at the graph level. The
    // call is forwarded to PluginSlot::set_parameter(). Returns false if the
    // node is not a Plugin node or has no loaded slot. For block-rate routing
    // from graph audio into parameters, use connect_automation() or
    // connect_audio_rate_modulation().
    bool set_node_parameter(NodeId id, uint32_t param_id, float value);

    // Read a parameter's current value from a Plugin node (returns 0.0f if
    // the node is not a Plugin or has no slot).
    float get_node_parameter(NodeId id, uint32_t param_id) const;

private:
    struct MidiBlockSnapshot {
        MidiBlockSnapshot();
        MidiBlockSnapshot(const MidiBlockSnapshot& other);
        MidiBlockSnapshot& operator=(const MidiBlockSnapshot& other);
        bool set_from_midi(const midi::MidiBuffer& src,
                           uint64_t new_sequence,
                           bool source_incomplete = false);
        bool copy_to_midi(midi::MidiBuffer& dst) const;

        midi::MidiBuffer events;
        midi::UmpBuffer ump;
        bool incomplete = false;
        uint64_t sequence = 0;
    };

    struct MidiInputMailbox {
        runtime::TripleBuffer<MidiBlockSnapshot> published;
        MidiBlockSnapshot writer_scratch;
        std::atomic<uint64_t> next_sequence{0};
    };

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
        std::vector<const float*> input_const_ptrs;
        struct EdgeRef {
            size_t connection_index = 0;
            NodeRuntime* source_runtime = nullptr;
        };
        std::unique_ptr<std::atomic<float>> gain;
        std::vector<EdgeRef> inbound_midi_edges;
        std::vector<EdgeRef> inbound_audio_edges;
        std::vector<EdgeRef> sparse_automation_edges;
        std::vector<EdgeRef> audio_rate_modulation_edges;

        // PDC: cumulative samples of latency from AudioInput to this node's
        // input ports (input_latency) and output ports (output_latency).
        // output_latency = input_latency + (plugin->latency_samples() for
        // Plugin nodes, 0 otherwise).
        int64_t input_latency = 0;
        int64_t output_latency = 0;

        // Per-node CPU-load telemetry. Points at a SignalGraph-owned
        // AudioProcessLoadMeasurer that PERSISTS across CompiledGraph snapshots
        // (keyed by NodeId in node_load_), so re-prepare() doesn't reset a
        // node's accumulated load. process() wraps this node's work in
        // begin()/end(); the measurer is relaxed-atomic (RT-safe). Null until
        // resolved in compile_(); never owned by NodeRuntime.
        pulp::audio::AudioProcessLoadMeasurer* load = nullptr;

        struct ParamBounds {
            uint32_t id = 0;
            float min_value = 0.0f;
            float max_value = 1.0f;
        };
        std::vector<ParamBounds> param_bounds;

        struct SparseAutomationAccum {
            float v0 = 0.0f;
            float vN = 0.0f;
            float lo = 0.0f;
            float hi = 1.0f;
            bool has_add = false;
            bool touched = false;
        };
        std::vector<uint32_t> sparse_automation_param_ids;
        std::vector<SparseAutomationAccum> sparse_automation_accum;

        // MIDI scratch is audio-thread-owned. Control-thread ingress and
        // egress use the mailboxes below so inject_midi()/extract_midi() do
        // not race process() scratch mutation.
        midi::MidiBuffer midi_in;
        midi::MidiBuffer midi_out;
        midi::UmpBuffer midi_in_ump;
        midi::UmpBuffer midi_out_ump;
        bool midi_in_incomplete = false;
        bool midi_out_incomplete = false;
        std::unique_ptr<MidiInputMailbox> midi_input_mailbox;
        std::unique_ptr<runtime::TripleBuffer<MidiBlockSnapshot>> midi_output_mailbox;
        uint64_t midi_input_sequence_seen = 0;

        // Audio-rate modulation scratch. Each listed param gets one
        // max-block-sized region in audio_rate_param_data, filled immediately
        // before the destination plugin processes.
        struct DenseAutomationAccum {
            float lo = 0.0f;
            float hi = 1.0f;
            bool has_replace = false;
            bool has_add = false;
        };
        std::vector<uint32_t> audio_rate_param_ids;
        std::vector<float> audio_rate_param_data;
        std::vector<DenseAutomationAccum> audio_rate_accum;
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

        // Per-source automation slew state. Holds the value
        // we last delivered to the destination (post-slew, post range-
        // map) so the next block can resume ramping toward a new target
        // instead of snapping. `primed` tracks whether `last_value` has
        // been seeded yet; on the first block of a freshly-prepared graph
        // we snap to the source value to avoid a glide from zero.
        float slew_last_value = 0.0f;
        bool slew_primed = false;
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
        std::vector<NodeRuntime::EdgeRef> feedback_edges;
        std::vector<ConnectionDelay> connection_delays;  // parallel to connections
        // Runtime + plugin + node-info keyed by NodeId so we don't rely on
        // pointers into an outer container.
        std::unordered_map<NodeId, NodeRuntime> runtime;
        std::unordered_map<NodeId, std::shared_ptr<PluginSlot>> plugins;
        std::unordered_map<NodeId, CustomNodeProcessFn> custom_processors;
        // Transport-aware custom callbacks, populated alongside custom_processors
        // for any Custom node whose type registered a transport-aware variant.
        // The presence of an entry mirrors GraphNode::transport_sensitive for
        // that node (both resolved from the same compile-time condition), so the
        // routed binding and the anticipation analysis stay consistent.
        std::unordered_map<NodeId, CustomNodeTransportProcessFn> custom_transport_processors;
        struct NodeShape {
            NodeType type;
            int num_input_ports;
            int num_output_ports;
        };
        struct OrderedRuntime {
            NodeId id = 0;
            NodeShape shape{};
            NodeRuntime* runtime = nullptr;
        };
        std::unordered_map<NodeId, NodeShape> shapes;
        std::vector<OrderedRuntime> ordered_runtime;
        int max_block_size = 0;
        double sample_rate = 0.0;  // needed to convert automation_smoothing_ms
                                   // into samples.
        int64_t total_latency_samples = 0;
        MidiBlockSnapshot midi_publish_scratch;

        // Immutable canonical-executor routing for this snapshot, built in
        // compile_() when the topology is executor-eligible (see
        // signal_graph_topology_executor_eligible). Empty/invalid otherwise. Its
        // Gain bindings reference this snapshot's own `runtime[id].gain` atomics,
        // its Plugin/Custom bindings invoke this snapshot's own slots/callbacks,
        // so it carries no keepalive — it lives and dies exactly with this
        // CompiledGraph, published atomically with it via live_raw_. The legacy
        // walk remains the reference/fallback path for ineligible or
        // routing-failed graphs, not a retired one.
        format::GraphRuntimeSnapshot routing_snapshot;
        bool routing_valid = false;
        // Per-snapshot scratch pool driving the routed path, sized in compile_()
        // to routing_snapshot's slot count × max_block_size. Owned by THIS
        // snapshot (like the legacy per-node runtime scratch), so a re-prepare
        // builds a fresh pool on a fresh cg and never mutates the buffers an
        // in-flight audio-thread reader is using on a retired snapshot. Feedback
        // previous-block state lives here and resets with the snapshot.
        format::GraphRuntimeBufferPool exec_pool;
        // Plugin-binding storage for the routed path, owned per-snapshot like
        // exec_pool. routing_plugin_ctx is reserved in compile_() to the plugin
        // count so the snapshot's Plugin bindings can point their user_data at
        // stable elements; routing_plugin_scratch is the serial snapshot's shared
        // MIDI/param fallback scratch when no per-node executor scratch is
        // supplied. The slots themselves live in `plugins` above (same cg
        // lifetime).
        std::vector<PluginBindingContext> routing_plugin_ctx;
        PluginRoutingScratch routing_plugin_scratch;
        // Custom-binding storage for the routed path, owned per-snapshot like
        // routing_plugin_ctx. Reserved in compile_() to the custom-node count so
        // each Custom binding's user_data points at a stable element; each holds a
        // COPY of this snapshot's resolved process callback (its own keepalive on
        // any captured instance, same cg lifetime as `custom_processors`).
        std::vector<CustomBindingContext> routing_custom_ctx;
        // Per-node MIDI buffers for the routed path's event edges, owned
        // per-snapshot like exec_pool. Empty (node_count 0) for graphs with no
        // MIDI. The routed dispatch bridges the SignalGraph MIDI mailboxes to
        // these buffers around process_routed: a MidiInput node's mailbox is read
        // into its scratch output before the walk, and a MidiOutput node's
        // gathered scratch input is published to its mailbox after. `routing_midi_io`
        // lists those system nodes by (dense plan index, NodeId) so the bridge
        // avoids a per-block NodeId lookup.
        format::GraphRuntimeMidiScratch routing_midi;
        // `pending_seq` is audio-thread scratch: the mailbox sequence a MidiInput
        // would consume this block, captured during the ingress pre-fill and
        // committed to midi_input_sequence_seen only after the routed dispatch
        // succeeds (so a fallback to the legacy walk re-consumes the same block).
        struct RoutedMidiNode { std::uint32_t plan_index; NodeId id; std::uint64_t pending_seq = 0; };
        std::vector<RoutedMidiNode> routing_midi_inputs;
        std::vector<RoutedMidiNode> routing_midi_outputs;
        // Per-node parameter-event queues + per-connection slew state for routed
        // sparse automation, owned per-snapshot like exec_pool. Empty (node_count
        // 0) for graphs with no sparse automation.
        format::GraphRuntimeAutomationScratch routing_automation;

        // Levelized PARALLEL routing for this snapshot (built only when parallel
        // routing is enabled at compile time). Same plan as routing_snapshot but
        // with a reuse-free buffer assignment (parallel_safe) so concurrent
        // same-level nodes never alias a slot, plus its own (larger) scratch pool
        // and the static level schedule. The MIDI/automation scratch and the
        // MidiInput/Output node lists above are SHARED (identical plan; only ONE
        // path runs per block). routing_plugin_ctx_parallel holds the parallel
        // snapshot's Plugin bindings' stable user_data; each binding owns its
        // fallback scratch so same-level Plugin nodes do not share mutable MIDI
        // output state.
        format::GraphRuntimeSnapshot routing_snapshot_parallel;
        format::GraphRuntimeBufferPool exec_pool_parallel;
        graph::GraphRuntimeLevelization routing_levelization;
        std::vector<PluginBindingContext> routing_plugin_ctx_parallel;
        // Parallel snapshot's Custom bindings' stable user_data (mirrors
        // routing_plugin_ctx_parallel; the parallel and serial Custom callbacks
        // are independent copies, but a custom node is stateless on the audio
        // thread w.r.t. concurrency here — only the snapshot wiring differs).
        std::vector<CustomBindingContext> routing_custom_ctx_parallel;
        bool routing_parallel_valid = false;

        // Anticipative rendering for this snapshot (built only when anticipation is
        // enabled at compile time AND the routed plan has an eligible latent
        // interior). The lane pre-renders the interior off the audio thread; the
        // live path masks the interior in routing_snapshot's walk and feeds the
        // lane's pre-rendered boundary signals into the interior boundary-source
        // output slots of exec_pool. anticipation_skip_mask is indexed by
        // routing_snapshot's dense node order; anticipation_prefill maps each lane
        // output channel to the exec_pool slot carrying that boundary signal;
        // anticipation_consume_scratch is the per-snapshot capture buffer consume()
        // pops a block into before the prefill copy. The lane owns the interior
        // plugin instances' state advancement — process() never runs them.
        AnticipationLane anticipation_lane;
        std::vector<std::uint8_t> anticipation_skip_mask;
        struct AnticipationPrefill {
            std::uint32_t out_channel = 0;  // lane output channel
            std::uint32_t slot = 0;         // exec_pool mono slot to fill
        };
        std::vector<AnticipationPrefill> anticipation_prefill;
        std::vector<std::vector<float>> anticipation_consume_scratch;
        std::vector<float*> anticipation_consume_ptrs;
        bool anticipation_valid = false;
    };

    std::vector<GraphNode> nodes_;
    std::vector<Connection> connections_;
    std::unordered_map<std::string, CustomNodeType> custom_node_types_;
    NodeId next_id_ = 1;
    GraphLimits limits_;

    // Audio-thread snapshot, published by prepare() / mutators. The audio
    // thread reads live_raw_ only; live_ and retired_snapshots_ keep pointed-to
    // storage alive from the control thread until active process readers drain.
    std::shared_ptr<CompiledGraph> live_;
    std::atomic<CompiledGraph*> live_raw_{nullptr};
    std::vector<std::shared_ptr<CompiledGraph>> retired_snapshots_;

    // Canonical-executor routing (control toggle, read relaxed on the audio
    // thread). DEFAULT ON: the routed executor is the primary inter-node backend
    // for every eligible graph and is bit-identical to the legacy walk for that
    // subset (proven by the routed-vs-walk parity suite) AND now reports the same
    // per-node node_loads() telemetry, so the default-ON flip is behaviour-
    // preserving where it takes effect. Ineligible graphs (per-node automation
    // past the executor's fixed capacity, or a Plugin node with no live slot)
    // still fall back to the legacy walk, which remains the reference oracle the
    // parity tests pin OFF explicitly. One long-lived executor whose telemetry survives re-prepare;
    // it is stateless w.r.t. topology (it takes the snapshot + the snapshot's own
    // pool as arguments) and prepare() never mutates it, so the single audio
    // thread is its only writer (relaxed stat counters). The mutable scratch pool
    // is owned per-snapshot by CompiledGraph (see CompiledGraph::exec_pool) so it
    // rides the existing RCU lifetime and is never resized under a reader.
    std::atomic<bool> canonical_executor_routing_enabled_{true};
    format::GraphRuntimeExecutor executor_;
    // Levelized parallel routing opt-in + its persistent worker pool. The pool is
    // a long-lived SignalGraph member (NOT per-RCU-snapshot): started off-RT in
    // compile_() when parallel routing is enabled, joined in the destructor. The
    // parallel-safe snapshot + levelization + scratch pool ride the CompiledGraph
    // (see CompiledGraph::routing_*_parallel).
    std::atomic<bool> parallel_routing_enabled_{false};
    std::atomic<bool> anticipation_enabled_{false};
    // Guards the single-producer contract on pump_anticipation: a concurrent or
    // reentrant call degrades to a no-op instead of corrupting the lane's
    // unsynchronized executor/pool/scratch + interior plugin state.
    std::atomic<bool> anticipation_pump_busy_{false};
    // See routed_walk_fallbacks(): incremented when an eligible routed path failed
    // dispatch and process() fell back to the walk. Audio-thread writer only.
    std::atomic<std::uint64_t> routed_walk_fallbacks_{0};
    // See transport_suppressed_for_anticipation(): set by compile_() to the count
    // of transport-sensitive nodes that active anticipation forced exterior
    // (excluded from the ahead-rendered interior so they observe the live
    // transport). Control-thread writer (compile_) only.
    std::atomic<std::uint64_t> transport_suppressed_for_anticipation_{0};
    format::GraphRuntimeWorkerPool worker_pool_;
    // mutable: const snapshot readers (extract_midi / node_latency_samples) must
    // bump this reader pin via ProcessReadGuard. Pinning does not change logical
    // const-ness of the graph.
    mutable std::atomic<std::uint32_t> active_process_readers_{0};

    // RCU-style read pin for the live CompiledGraph snapshot. Any thread that
    // dereferences live_raw_ must hold one of these for the entire duration of
    // the dereference so prune_retired_snapshots_() / wait_for_retired_snapshots_()
    // can see it and defer the free. The audio process path uses it; so do the
    // control-thread snapshot readers (inject_midi / extract_midi /
    // node_latency_samples / set_node_gain) — none of which run on the
    // prepare/release thread, so without the pin a concurrent prepare()/release()
    // could retire+free the snapshot mid-dereference (use-after-free). The seq_cst
    // add/sub pairs with prune/wait's seq_cst load (see prune_retired_snapshots_()).
    struct ProcessReadGuard {
        explicit ProcessReadGuard(const SignalGraph& owner) noexcept
            : owner_(owner) {
            owner_.active_process_readers_.fetch_add(1, std::memory_order_seq_cst);
        }
        ~ProcessReadGuard() noexcept {
            owner_.active_process_readers_.fetch_sub(1, std::memory_order_seq_cst);
        }
        ProcessReadGuard(const ProcessReadGuard&) = delete;
        ProcessReadGuard& operator=(const ProcessReadGuard&) = delete;
        const SignalGraph& owner_;
    };
    std::atomic<int64_t> total_latency_samples_{0};  // reflected for const-query access
    std::atomic<std::size_t> prepared_node_count_{0};
    std::atomic<std::size_t> prepared_ordered_node_count_{0};
    std::atomic<std::size_t> prepared_connection_count_{0};
    std::atomic<std::size_t> prepared_total_ports_{0};
    std::atomic<int> prepared_max_block_size_{0};
    std::atomic<std::size_t> prepared_node_audio_buffer_bytes_{0};
    std::atomic<std::size_t> prepared_automation_buffer_bytes_{0};
    std::atomic<std::size_t> prepared_delay_buffer_bytes_{0};
    std::atomic<std::size_t> prepared_total_buffer_bytes_{0};

    // Persistent per-node load measurers, keyed by NodeId. Control-thread
    // owned; compile_() resolves each NodeRuntime::load to one of these (only
    // ever ADDS entries while a snapshot may be live, never erases, so the
    // audio thread's raw measurer pointers stay valid across snapshot swaps —
    // the measurers are heap-stable behind unique_ptr regardless of map
    // rehash). node_loads() reads their snapshots.
    std::unordered_map<NodeId, std::unique_ptr<audio::AudioProcessLoadMeasurer>>
        node_load_;
    // Guards node_load_ MAP structure (insert in compile_ vs iterate in
    // node_loads()) — those can run on different control threads (host
    // prepare() vs UI poll). Control-side only; the audio thread never takes
    // it (it touches measurer OBJECTS via NodeRuntime::load, not the map).
    mutable std::mutex node_load_mu_;

    // Serializes CONTROL-THREAD access to the source-of-truth topology — the
    // nodes_ / connections_ vectors and the plain (non-atomic) GraphNode fields
    // they own (gain, ports, plugin/custom identity, custom state) — AND the
    // snapshot-publication state (live_ / live_raw_ / retired_snapshots_) that a
    // re-prepare and a concurrent invalidate_live_() both mutate. The control
    // surface is multi-threaded: a host thread calls prepare()/compile_() (which
    // iterates nodes_/connections_ and reads GraphNode::gain) while a UI thread
    // adds/removes nodes or connections, edits a GraphNode field
    // (set_node_gain / set_node_parameter / set_custom_node_state), or reads one
    // (node_gain / get_node_parameter / custom_node_state / node_loads). Without
    // this lock those vector mutations + plain-field reads/writes race.
    //
    // Contract — EVERY public control-thread method that reads or writes
    // nodes_ / connections_ / a mutable GraphNode field, or that publishes/retires
    // a snapshot, holds this mutex for the duration of that access:
    //   mutators  : add_input_node, add_output_node, add_plugin_node(×2),
    //               add_unresolved_plugin_node, add_gain_node,
    //               add_midi_input_node, add_midi_output_node,
    //               add_unresolved_custom_node, register_custom_node_type,
    //               remove_node, connect, connect_midi, connect_sidechain,
    //               connect_automation, connect_audio_rate_modulation,
    //               connect_feedback, disconnect, clear, set_node_gain,
    //               set_node_parameter, set_custom_node_state, prepare, release.
    //   readers   : node_gain, get_node_parameter, custom_node_state, node_loads.
    // The low-level helpers node() / has_path() / would_create_cycle() /
    // processing_order() / custom_node_type() / audio_rate_modulation_lane() /
    // validate_generated_graph() / total_declared_ports_() are LOCK-FREE and
    // assume the caller already holds this mutex; the public methods above are
    // their only callers on the mutating path. Public methods that delegate to
    // another public mutator (add_custom_node -> add_unresolved_custom_node) do
    // NOT take the lock themselves — the leaf takes it once — to avoid
    // self-deadlock on this non-recursive mutex.
    //
    // The nodes() / connections() accessors hand out a reference to the live
    // vector and the live_*() snapshot getters read live_; both are governed by
    // the same caller-side keepalive/serialization contract they always were and
    // are intentionally NOT locked here (locking a reference-returning accessor
    // can't protect the caller's later iteration anyway).
    //
    // Deadlock-free by construction:
    //  - Taken ONLY on the control thread. The audio render path
    //    (process_impl / the executor / the reference walk) reads the immutable
    //    CompiledGraph snapshot — never nodes_ or GraphNode fields — so it must
    //    never take this lock. Taking a mutex on the audio thread would be an
    //    RT-safety violation.
    //  - NEVER held while acquiring a ProcessReadGuard reader-pin. The methods
    //    that both edit source state AND reflect into the live snapshot
    //    (set_node_gain) take this mutex for the source write, RELEASE it, then
    //    separately pin the snapshot — so this mutex can never invert lock order
    //    with the RCU drain (prune_retired_snapshots_ / wait_for_retired_snapshots_,
    //    the latter busy-waited under this mutex by release()).
    mutable std::mutex graph_mutation_mutex_;

    bool has_path(NodeId from, NodeId to) const;
    std::size_t total_declared_ports_() const;
    // Lock-free core of audio_rate_modulation_lane() — scans nodes_ via node() and
    // assumes the caller already holds graph_mutation_mutex_. The public
    // audio_rate_modulation_lane() locks and forwards here; connect_audio_rate_
    // modulation() (already holding the mutex) calls this core directly to avoid
    // re-locking the non-recursive mutex.
    bool audio_rate_modulation_lane_locked_(const Connection& connection,
                                            state::ModulationLane& lane) const;
    // Shared body of both process() overloads. `transport` is the host
    // transport context for the block, or nullptr for the no-transport path.
    // RT-safe (no allocation per block).
    void process_impl(audio::BufferView<float>& output,
                      const audio::BufferView<const float>& input,
                      int num_samples,
                      const format::ProcessContext* transport);
    // Legacy serial reference walk — the hand-maintained, bit-exact inter-node
    // DSP oracle and fallback for process_impl(). Lives in its own translation
    // unit (signal_graph_reference_walk.cpp) but stays a member so it can name
    // the private nested CompiledGraph / NodeRuntime types without promotion.
    // process_impl() invokes it when no routed path takes a block.
    void run_reference_walk_(audio::BufferView<float>& output,
                             const audio::BufferView<const float>& input,
                             int num_samples,
                             CompiledGraph* cg);
    std::shared_ptr<CompiledGraph> compile_(double sample_rate, int max_block_size);
    void publish_prepared_stats_(const CompiledGraph& cg);
    void clear_prepared_stats_();
    void invalidate_live_();
    void retire_snapshot_(std::shared_ptr<CompiledGraph> snapshot);
    void prune_retired_snapshots_();
    void wait_for_retired_snapshots_();
    static void compute_latencies_for_(CompiledGraph& cg,
                                       const std::vector<Connection>& connections);
};

// Drag-add helper.
//
// `add_plugin_node_from_drop` is the host-side companion to
// `pulp::view::PluginManagerPanel::on_row_drag_start`. It attempts to
// load the plugin in the supplied `PluginInfo`; if PluginSlot::load
// returns null (plugin missing on this machine, ABI mismatch, scanner
// gave us a stale path, etc.) it falls back to
// `add_unresolved_plugin_node` so the topology survives a serialize +
// reload pass and the user can resolve the plugin later.
//
// `loaded_out`, if non-null, is set to true when a live PluginSlot
// attached or false when the node landed unresolved — useful for
// driving the drop-target UI (e.g. ghost-vs-solid node rendering).
//
// The function takes the graph by reference; it is the caller's
// responsibility to call `graph.prepare(...)` afterwards before the
// next audio block. We do NOT do that here because real hosts batch
// drops and prepare once at the end of the UI tick.
NodeId add_plugin_node_from_drop(SignalGraph& graph,
                                 const PluginInfo& info,
                                 bool* loaded_out = nullptr);

} // namespace pulp::host
