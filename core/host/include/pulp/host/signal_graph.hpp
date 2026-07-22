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
#include <pulp/audio/live_dsp_telemetry.hpp>
#include <pulp/audio/load_measurer.hpp>
#include <pulp/format/audio_workgroup_client.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/runtime/slot.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>
#include <pulp/runtime/budget_policy.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/state/modulation_lane.hpp>
#include <atomic>
#include <cassert>
#include <thread>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
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

enum class LiveSwapCurve { Smoothstep, EqualPower };

struct NodeLiveSwapPolicy {
    bool allow_live_instance_swap = false;
    // The crossfade shape for a live instance swap: the committed swap renders both the
    // old and new instance for fade_ms and blends old->new along `curve` (click-free at
    // both ends), so a swap between instances whose output differs does not step at the
    // boundary. fade_ms == 0 disables the fade (an instant atomic switch at a block
    // boundary — gap-free but a hard cut).
    int fade_ms = 30;
    LiveSwapCurve curve = LiveSwapCurve::EqualPower;
    float headroom_threshold = 0.75f;
    std::size_t max_state_bytes = 64ull * 1024ull * 1024ull;
    std::function<void(NodeId,
                       std::shared_ptr<PluginSlot> /*old_slot*/,
                       std::shared_ptr<PluginSlot> /*new_slot*/)>
        on_instance_swapped;
};

enum class LiveSwapFallbackReason : uint8_t {
    None,
    NotOptedIn,
    LoadFailed,
    PrepareFailed,
    StateRestoreFailed,
    StateTooLarge,
    ShapeMismatch,
    LatencyChanged,
    EditorOpen,
    ParamContractMismatch,
    FeedbackNotSwappable,
    OverBudget,
    NoLoadHistory,
    PredicateExcluded,
    UntrustedIdentity,
};

struct LiveSwapDiagnostics {
    LiveSwapFallbackReason reason = LiveSwapFallbackReason::None;
    NodeId offending_node = 0;
    std::string message;
};

using CustomNodeProcessFn = std::function<void(audio::BufferView<float>& output,
                                              const audio::BufferView<const float>& input,
                                              int num_samples)>;

// ── Bake-layer parameter injection ──────────────────────────────────────
//
// A separate, RT-safe path (owned by BakedGraphProcessor) that lets a control
// thread set a BAKED custom node's parameter and have it applied sample-
// accurately in process(), WITHOUT re-baking and without touching the live
// graph's parameter-ingress path. It reuses pulp::state::ParameterEvent /
// ParameterEventQueue verbatim and mirrors the live mailbox's precedence and
// per-node exclusive-claim discipline; see baked_graph_processor.hpp.
//
// A lowerable custom node opts in by declaring `baked_params` and providing a
// `process_instance_baked_param` callback. On a BAKED graph the executor calls
// that callback instead of process_instance, handing it a BakedParamView it
// queries for the ramped, sample-accurate value of each declared param. This
// channel is consulted ONLY on a baked graph; on the live graph the node runs
// via its plain process_instance/process as usual (a node that provides only
// the baked callback runs neither live — the routed executor falls through to
// input passthrough — so it is meant to be baked before use).

// Read-only per-sample parameter accessor handed to a param-aware baked custom
// node. `value_at(id, k)` returns the ramped, sample-accurate value of a
// declared param at block-relative sample offset `k`; offsets must be queried
// in non-decreasing order within a block (the backing cursor advances
// monotonically). `value(id)` reads the value at the cursor's current position.
class BakedParamView {
public:
    virtual ~BakedParamView() = default;
    virtual float value_at(state::ParamID id, int32_t sample_offset) const = 0;
    virtual float value(state::ParamID id) const = 0;
};

// Bound param-aware process callback (instance captured). Same audio signature
// as CustomNodeProcessFn plus the block's BakedParamView. The baked executor
// runs this for a param-declaring custom node.
using CustomNodeParamProcessFn =
    std::function<void(audio::BufferView<float>& output,
                       const audio::BufferView<const float>& input,
                       int num_samples,
                       const BakedParamView& params)>;

// One declared parameter of a lowerable custom node, for the bake-layer
// injection path. `id` is node-local (the framework namespaces per node, so two
// nodes of the same type never collide). Values are clamped to [min, max];
// `default_value` is the held value until the first injection arrives.
struct CustomNodeBakedParam {
    state::ParamID id = 0;
    float min_value = 0.0f;
    float max_value = 1.0f;
    float default_value = 0.0f;
};

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

    // Bake opt-in: whether this type may be LOWERED into a baked artifact.
    // Default false → bake refuses (a custom instance holds opaque state a frozen
    // topology cannot otherwise capture). Setting true is a registrar assertion
    // that the type is deterministic given (state, input), holds no ambient mutable
    // global state (only the per-instance void*), round-trips completely through
    // save_state/load_state, is real-time-safe, and is NOT transport-sensitive.
    // This is a trusted-binary / developer boundary only — NOT a shipped-artifact
    // security boundary (the framework cannot verify these properties), so the
    // on-disk load path must additionally require a signature before honoring it.
    bool lowerable = false;

    // Bake-layer parameter injection (additive opt-in). When `baked_params` is
    // non-empty AND `process_instance_baked_param` is set, a BAKED instance of
    // this node can receive sample-accurate ParameterEvents from the control
    // thread via BakedGraphProcessor::inject(); the baked executor runs
    // `process_instance_baked_param` (leading instance pointer) instead of
    // process_instance, handing it a BakedParamView over the injected values.
    // Both empty == today's node, unchanged: no injection channel, runs via
    // process_instance. This path is independent of the live-graph parameter
    // ingress and does not require re-baking to turn a knob.
    //
    // Live-graph behavior: `process_instance_baked_param` is consulted ONLY on a
    // baked graph. A node that provides ONLY this callback (no plain
    // process/process_instance) does NOT run its baked-param DSP on the live
    // graph — with no live callback the routed executor falls through to input
    // passthrough (transparent; it contributes no signal of its own, but is not
    // hard silence when fed a signal). Such a node is meant to be baked before
    // use; provide a plain process callback too if it must also run live.
    std::vector<CustomNodeBakedParam> baked_params;
    std::function<void(void* /*instance*/, audio::BufferView<float>& /*output*/,
                       const audio::BufferView<const float>& /*input*/,
                       int /*num_samples*/, const BakedParamView& /*params*/)>
        process_instance_baked_param;
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

    NodeLiveSwapPolicy live_swap_policy;
    bool hosted_editor_open = false;
};

// ── Signal Graph ────────────────────────────────────────────────────────

class SignalGraph : public format::AudioWorkgroupClient {
public:
    class PreparedTopologyEdit;
    class ExecutionSnapshot;

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

    struct PluginCatalogToken {
        std::uint64_t value = 0;
        explicit operator bool() const noexcept { return value != 0; }
        friend bool operator==(PluginCatalogToken a,
                               PluginCatalogToken b) noexcept {
            return a.value == b.value;
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
    // and per-format adapters can recognize the role.
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
    // smoothing_ms applies a per-source linear slew before mix/clamp.
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

    // Inject a MIDI buffer into a MidiInput source node. Call before process();
    // the latest published events become that node's MIDI output exactly once.
    // Audio-callback-safe after prepare(): the mailbox is fixed-capacity and the
    // call never allocates or locks. Exactly one thread may inject into a given
    // MidiInput node; it may be the audio thread immediately before process(), or
    // a control-side producer, but never concurrent writers. A false return means
    // the live node is unavailable or the bounded mailbox retained only a prefix;
    // that retained prefix is still published. A preserved MidiInput NodeId keeps
    // its unconsumed publication across a gap-free prepare_swap().
    bool inject_midi(NodeId midi_input_node,
                     const midi::MidiBuffer& events) noexcept;

    // Inject sample-accurate parameter events into a Plugin node. Call before
    // process(); the latest published batch is consumed exactly once by that
    // node on the next block it processes. Events use block-relative sample
    // offsets and are delivered after graph-generated automation, so an
    // injected event wins an equal-offset tie while existing automation keeps
    // capacity priority. Returns false for an unprepared/non-Plugin/unresolved
    // node or when `events` already reports source-side overflow; the retained
    // prefix is still published in the overflow case.
    bool inject_parameter_events(NodeId plugin_node,
                                 const state::ParameterEventQueue& events);

    // Drain queued nonempty MIDI blocks from a MidiOutput sink node in process
    // order. Appends to `out`; returns false if bounded egress overflowed, a
    // queued block was already incomplete, or `out` lacks capacity/UMP sidecar
    // storage. A destination-copy failure retains the undelivered suffix;
    // provide more storage and call again to resume without replaying the
    // delivered prefix.
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

    // Begin an isolated topology transaction. The returned edit owns a private
    // authoring graph and custom-type registry; its mutations never invalidate
    // this graph's live snapshot. prepare() compiles that candidate off-side and
    // commit() installs the authoring graph and compiled snapshot together.
    // Destroying an uncommitted edit is a complete rollback.
    std::unique_ptr<PreparedTopologyEdit> begin_prepared_topology_edit();

    // Live swap transaction outcomes.
    enum class SwapResult {
        Swapped,           // the new topology was published with no silent block.
        Staged,            // a replacement instance is staged for prepare_swap().
        NeedsEagerPrepare, // not reinit-free (or a latency change) — caller must
                           // call prepare() under the usual no-process/no-pump
                           // contract. Ordinary failures invalidate the live
                           // snapshot; the MidiOutput pending-egress refusal keeps
                           // it live for extraction until that eager prepare.
        NotInSwapEdit,     // prepare_swap called without a matching begin_swap_edit.
    };
    // Begin a transactional topology edit that MAY publish with no silence. Between
    // begin_swap_edit() and prepare_swap()/abort_swap_edit(), the CALLING thread's
    // graph mutations (connect/disconnect/gain/add/remove) do NOT invalidate the
    // live snapshot — it keeps playing the old compiled graph until prepare_swap
    // atomically publishes the new one. A mutation from any OTHER thread, or a
    // lifecycle/limits/registry mutation from any thread, cancels the no-silence
    // attempt (the live snapshot is invalidated as usual). Single-owner: nesting is
    // refused. Caller must pair with prepare_swap or abort_swap_edit.
    void begin_swap_edit();
    // Publish the staged topology with no silent block if it is reinit-free (same
    // instances, no re-init, unchanged latency, no per-snapshot state to glitch);
    // otherwise return NeedsEagerPrepare so the caller eager-prepares. Ordinary
    // refusals invalidate the live snapshot. The MidiOutput pending-egress refusal
    // deliberately leaves it live so the caller can drain extract_midi() first.
    SwapResult prepare_swap(double sample_rate, int max_block_size);
    // Abandon the no-silence attempt: invalidate the live snapshot (the staged
    // edits remain in the graph and take effect on the next prepare()).
    void abort_swap_edit();
    void release();

    bool set_node_live_swap_policy(NodeId id, NodeLiveSwapPolicy policy);
    bool set_node_hosted_editor_open(NodeId id, bool open);
    PluginCatalogToken register_scanned_plugin(const PluginInfo& info);
    void clear_scanned_plugin_catalog();
    // Stage a live instance replacement for a node, resolved through the scanned
    // catalog (never an arbitrary path). To capture the outgoing instance's state
    // without a silent block, this reads the OLD, still-live plugin's save_state()
    // and get_parameter() on the control thread while the audio thread may be inside
    // its process(). CONTRACT: a hosted plugin that opts into live swap must make
    // save_state()/get_parameter() safe to call concurrently with process() (they are
    // read-only snapshots for most plugins). Parameter capture goes through the cached
    // contract + atomic get_parameter; full-state capture is best-effort — a plugin
    // whose getState is strictly main-thread-only may capture a torn state, so keep it
    // concurrency-tolerant or rely on parameter re-sync alone.
    SwapResult stage_plugin_replacement(NodeId id, PluginCatalogToken token);
    LiveSwapDiagnostics last_swap_diagnostics() const;

    using PluginLoaderForTest =
        std::function<std::unique_ptr<PluginSlot>(const PluginInfo&)>;
    void set_live_swap_plugin_loader_for_test(PluginLoaderForTest loader);

    // Test seam for the non-zero MIDI-ingress publication sequence. The value
    // becomes the predecessor consumed by the next inject_midi() call.
    bool seed_midi_input_sequence_for_test(NodeId midi_input_node,
                                           std::uint64_t predecessor) noexcept;
    std::uint64_t midi_input_sequence_for_test(
        NodeId midi_input_node) const noexcept;

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

    // Whole-graph process-load snapshot (load == callback-budget fullness; 1.0 == a
    // full buffer). callback_count == 0 means no measurement yet. Safe to poll while
    // the audio thread runs. Feeds the live-swap admission gate.
    audio::AudioProcessLoadSnapshot graph_load() const;

    // Per-node live-DSP telemetry (fixed-slot p50/p95/p99 + jitter + over-budget
    // attribution; see audio/live_dsp_telemetry.hpp). Disabled by default; the
    // audio path is a single predicted-not-taken branch when off. The store lives
    // per-CompiledGraph so it rides the RCU snapshot lifetime — telemetry resets on
    // a topology recompile (a new topology is a new timing baseline). Per-node
    // timing is recorded on the legacy reference-walk path (the default when routed
    // dispatch is off); routed-executor blocks still contribute graph-level timing.
    // Control/UI thread only; serialized with snapshot publication.
    void set_live_dsp_telemetry_enabled(bool enabled);
    bool live_dsp_telemetry_enabled() const;
    // Drain the live snapshot's ring and return a copy of the latest summary.
    // Non-real-time; single poller (control/UI thread). Pins the live snapshot for
    // the duration of the drain. Returns an empty (available == false) snapshot when
    // no graph is prepared.
    audio::LiveDspTelemetrySnapshot poll_live_dsp_telemetry();

    // Test-only: run compile_() once and DISCARD the result, WITHOUT prepare()'s
    // null-first prologue. Exists so a TSan/ASan test can run compile_() on one
    // thread while process() runs the live snapshot on another — proving compile_()
    // never mutates state the audio thread reads (the 2.2a no-silence-swap
    // contract; planning/2026-07-02-signalgraph-prepared-swap-design.md). Publishes
    // nothing, so it has no audible effect and no production caller; it only
    // exercises the compilation path for the race test. Control-thread only.
    void compile_snapshot_for_test(double sample_rate, int max_block_size);

    // ── Canonical-executor migration ─────────────────────────────────────
    // Hooks the SignalGraph→GraphRuntimeExecutor translation needs without
    // exposing the private CompiledGraph. The translation lives in
    // signal_graph_executor_routing.{hpp,cpp}; these are control-thread only.

    // True once prepare() has published a live compiled snapshot.
    bool is_prepared() const noexcept { return live_slot_.live() != nullptr; }
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
    // The live compiled snapshot's bound param-aware custom callback for a Custom
    // node whose type declared baked_params + process_instance_baked_param, or
    // nullptr otherwise. The pointee captured the stateful instance shared_ptr by
    // value (keepalive), so a bake() that copies it stays self-contained. Same
    // lifetime contract as live_custom_processor. Used only by the bake-layer
    // parameter-injection path — never by live-graph processing.
    const CustomNodeParamProcessFn* live_custom_param_processor(
        NodeId id) const noexcept;
    // Opaque keepalive for the live compiled snapshot so a translated routing
    // can pin the lifetime of the gain atomics + plugin slots it references.
    std::shared_ptr<const void> live_snapshot_handle() const noexcept;

    /// Control-thread view of the actual routed paths embedded in the live
    /// compiled snapshot. Unlike topology eligibility, this reports whether
    /// snapshot construction and its fixed scratch pools both succeeded for the
    /// requested block size.
    struct RoutedExecutionStatus {
        bool prepared = false;
        bool serial_selected = false;
        bool serial_snapshot_valid = false;
        bool serial_pool_fits = false;
        bool parallel_selected = false;
        bool parallel_snapshot_valid = false;
        bool parallel_pool_fits = false;
        bool worker_pool_running = false;
        bool reference_walk_permitted = true;

        constexpr bool routed_path_ready() const noexcept {
            const bool serial_ready =
                serial_selected && serial_snapshot_valid && serial_pool_fits;
            const bool parallel_ready = parallel_selected && parallel_snapshot_valid &&
                                        parallel_pool_fits && worker_pool_running;
            return prepared && (serial_ready || parallel_ready);
        }
        constexpr bool strict_routed_ready() const noexcept {
            return routed_path_ready() && !reference_walk_permitted;
        }
    };
    RoutedExecutionStatus routed_execution_status(int block_size) const noexcept;
    std::uint64_t routed_only_execution_failures() const noexcept {
        return routed_only_execution_failures_.load(std::memory_order_relaxed);
    }

    /// Require compiled routed execution while at least one owner holds a lease.
    /// If no prepared routed path can process a block, process() clears output
    /// instead of entering the legacy reference walk. Control-thread only.
    void acquire_routed_only_execution() noexcept {
        routed_only_execution_owners_.fetch_add(1, std::memory_order_relaxed);
    }
    void release_routed_only_execution() noexcept {
        auto owners = routed_only_execution_owners_.load(std::memory_order_relaxed);
        while (owners != 0 && !routed_only_execution_owners_.compare_exchange_weak(
                                  owners, owners - 1, std::memory_order_relaxed)) {
        }
        assert(owners != 0 && "unbalanced routed-only execution lease");
    }

    // Controls whether the audio callback drives the canonical
    // GraphRuntimeExecutor when the live snapshot is executor-eligible. Default
    // ON: the routed executor is the primary inter-node backend. Set it OFF to
    // force the legacy walk — the parity tests do this to keep the walk as an
    // independent reference oracle. Ineligible graphs always fall back to the
    // legacy walk regardless of this flag. The flag is a control-thread toggle read
    // relaxed on the audio thread. The two paths produce bit-identical output
    // per block for eligible zero-PDC graphs, so toggling mid-stream is RT-safe
    // and seamless there. A prepared snapshot with active feed-forward PDC pins
    // its execution domain until the next prepare: the legacy, serial, and
    // parallel paths own independent delay history, so switching between them
    // mid-stream would select a stale ring. For graphs with FEEDBACK, the legacy walk
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
    // Publish the native device or AU render workgroup to the persistent
    // parallel executor. Safe before prepare and on render-context changes.
    void set_audio_workgroup(void* workgroup) noexcept override {
        worker_pool_.set_audio_workgroup(workgroup);
    }
    void set_audio_workgroup_from_render_context(
        void* workgroup) noexcept override {
        worker_pool_.set_audio_workgroup_from_render_context(workgroup);
    }
    bool prepare_audio_workgroup_for_render() noexcept override {
        return worker_pool_.prepare_audio_workgroup_for_render();
    }
    void wait_for_audio_workgroup_update() noexcept override {
        worker_pool_.wait_for_audio_workgroup_update();
    }
    void* configured_audio_workgroup() const noexcept {
        return worker_pool_.configured_audio_workgroup();
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

    // Control-thread diagnostic for transactional registry-GC tests and hosts
    // that generate short-lived custom types.
    std::size_t custom_node_type_count() const;

private:
    friend class ExecutionSnapshot;
    struct PrepareLifecycleObserver {
        void* context = nullptr;
        void (*plugin_will_prepare)(void*, PluginSlot*) noexcept = nullptr;
        void (*custom_will_prepare)(void*, void*) noexcept = nullptr;
    };
    struct MidiBlockSnapshot {
        MidiBlockSnapshot();
        MidiBlockSnapshot(const MidiBlockSnapshot& other);
        MidiBlockSnapshot& operator=(const MidiBlockSnapshot& other) noexcept;
        bool set_from_midi(const midi::MidiBuffer& src,
                           uint64_t new_sequence,
                           bool source_incomplete = false) noexcept;
        bool copy_to_midi(midi::MidiBuffer& dst) const noexcept;
        bool append_to_midi(midi::MidiBuffer& dst,
                            std::size_t& event_index,
                            std::size_t& sysex_index,
                            std::size_t& ump_index) const noexcept;
        bool has_payload() const noexcept;

        midi::MidiBuffer events;
        midi::UmpBuffer ump;
        bool incomplete = false;
        uint64_t sequence = 0;
    };

    struct MidiInputMailbox {
        static_assert(std::atomic<uint64_t>::is_always_lock_free,
                      "MIDI ingress sequence atomics must be lock-free");

        runtime::TripleBuffer<MidiBlockSnapshot> published;
        MidiBlockSnapshot writer_scratch;
        std::atomic<uint64_t> next_sequence{0};
        std::atomic<uint64_t> sequence_seen{0};
    };

    struct MidiOutputMailbox {
        static constexpr std::size_t kBlockCapacity = 4;
        static_assert(std::atomic<bool>::is_always_lock_free,
                      "MIDI egress overflow state must be lock-free");

        runtime::SpscQueue<MidiBlockSnapshot, kBlockCapacity> pending;
        std::atomic<bool> incomplete{false};
        MidiBlockSnapshot consumer_scratch;
        bool consumer_has_retry = false;
        bool consumer_incomplete = false;
        std::size_t consumer_event_index = 0;
        std::size_t consumer_sysex_index = 0;
        std::size_t consumer_ump_index = 0;
    };

    struct ParameterBlockSnapshot {
        std::array<state::ParameterEvent,
                   state::ParameterEventQueue::kCapacity> events{};
        std::size_t size = 0;
        std::uint64_t sequence = 0;
        bool source_incomplete = false;

        bool set_from_queue(const state::ParameterEventQueue& src,
                            std::uint64_t new_sequence) noexcept;
        bool append_to(state::ParameterEventQueue& dst) const noexcept;
    };

    struct ParameterInputMailbox {
        runtime::TripleBuffer<ParameterBlockSnapshot> published;
        ParameterBlockSnapshot writer_scratch;
        std::atomic<std::uint64_t> next_sequence{0};
        std::atomic<std::uint64_t> sequence_seen{0};
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

        // MIDI scratch is audio-thread-owned. Single-writer ingress and
        // control-thread egress use the mailboxes below so inject_midi()/
        // extract_midi() do not race process() scratch mutation. The shared
        // ingress mailbox preserves an unconsumed publication when a stable
        // MidiInput node crosses a gap-free snapshot swap. Egress is an ordered
        // bounded drain queue so later blocks cannot overwrite a pending
        // note-off before the control thread extracts it.
        midi::MidiBuffer midi_in;
        midi::MidiBuffer midi_out;
        midi::UmpBuffer midi_in_ump;
        midi::UmpBuffer midi_out_ump;
        bool midi_in_incomplete = false;
        bool midi_out_incomplete = false;
        std::shared_ptr<MidiInputMailbox> midi_input_mailbox;
        std::unique_ptr<MidiOutputMailbox> midi_output_mailbox;

        // Control/audio-thread parameter-event ingress for Plugin nodes. The
        // mailbox is single-writer/latest-wins like MIDI ingress; the sequence
        // is committed only after the selected processing path succeeds.
        std::shared_ptr<ParameterInputMailbox> parameter_input_mailbox;

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
        struct State {
            // Ring buffer: delay_samples + max_block_size_ frames per source
            // channel, plus the audio-thread-owned cursor. Shared only between
            // identity-matched live snapshots with equal delay structure.
            std::vector<float> ring;
            int write_pos = 0;
        };
        std::shared_ptr<State> state;
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
    enum class PdcExecutionDomain : std::uint8_t {
        Dynamic,  // no PDC: runtime routing toggles remain live
        Legacy,
        RoutedSerial,
        RoutedParallel,
    };

    struct CompiledGraph {
        std::vector<NodeId> order;
        std::vector<Connection> connections;
        std::vector<std::uint64_t> connection_identities;
        std::vector<NodeRuntime::EdgeRef> feedback_edges;
        std::vector<ConnectionDelay> connection_delays;  // parallel to connections
        // Runtime + plugin + node-info keyed by NodeId so we don't rely on
        // pointers into an outer container.
        std::unordered_map<NodeId, NodeRuntime> runtime;
        std::unordered_map<NodeId, std::shared_ptr<PluginSlot>> plugins;
        std::unordered_map<NodeId, CustomNodeProcessFn> custom_processors;
        // Bound param-aware custom callbacks (bake-layer injection). Populated for
        // any Custom node whose type declared baked_params +
        // process_instance_baked_param. Consumed only by bake(); the live routed
        // path never reads this map.
        std::unordered_map<NodeId, CustomNodeParamProcessFn> custom_param_processors;
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
        // Per-node live-DSP timing, prepared at compile in ordered_runtime order
        // (slot i == ordered_runtime[i]). Non-movable (holds atomics); CompiledGraph
        // is only ever make_shared'd and referenced, never copied, so an in-place
        // member is safe. Rides this snapshot's RCU lifetime.
        audio::LiveDspTelemetryStore live_dsp_telemetry;
        int max_block_size = 0;
        double sample_rate = 0.0;  // needed to convert automation_smoothing_ms
                                   // into samples.
        int64_t total_latency_samples = 0;
        // Non-Dynamic when this snapshot owns feed-forward delay history. The
        // selected path is captured at prepare time so a relaxed runtime toggle
        // cannot jump to another domain's stale ring mid-stream.
        PdcExecutionDomain pdc_execution_domain = PdcExecutionDomain::Dynamic;
        MidiBlockSnapshot midi_publish_scratch;
        // 2.2b reinit-free-swap predicate inputs (captured at compile so a
        // prepare_swap candidate can be compared against this live snapshot):
        //  - custom_instances: NodeId → raw custom-instance identity for each
        //    Custom node (set-equality vs the current nodes_ detects an added /
        //    removed / re-instantiated custom node → not reinit-free).
        //  - custom_registry_generation: the SignalGraph custom-type registry
        //    generation at compile time (a re-register would rebind callbacks to
        //    instances built by the old factory → not reinit-free).
        std::unordered_map<NodeId, const void*> custom_instances;
        std::uint64_t custom_registry_generation = 0;

        // ONE routed-executor path: the plan snapshot, the scratch pool sized for
        // exactly that snapshot, the stable binding storage its bindings' user_data
        // points into, and whether the build succeeded. They are grouped because
        // they are only ever valid TOGETHER — driving one path's snapshot against
        // the other path's pool is a bug that field naming alone could not prevent.
        //
        // `pool` is sized in compile_() to `snapshot`'s slot count × max_block_size
        // and owned by THIS snapshot (like the legacy per-node runtime scratch), so
        // a re-prepare builds a fresh pool on a fresh cg and never mutates the
        // buffers an in-flight audio-thread reader is using on a retired snapshot.
        // Feedback previous-block state lives there and resets with the snapshot.
        // `plugin_ctx` / `custom_ctx` are reserved in compile_() to the plugin /
        // custom-node count so each binding's user_data points at a stable element;
        // each CustomBindingContext holds a COPY of this snapshot's resolved process
        // callback (its own keepalive on any captured instance, same cg lifetime as
        // `custom_processors`). The plugin slots themselves live in `plugins` above.
        // `levelization` is the static level schedule the parallel path runs; it
        // stays empty on the serial path.
        struct RoutedPath {
            format::GraphRuntimeSnapshot snapshot;
            format::GraphRuntimeBufferPool pool;
            std::vector<PluginBindingContext> plugin_ctx;
            std::vector<CustomBindingContext> custom_ctx;
            graph::GraphRuntimeLevelization levelization;
            bool valid = false;
        };

        // `pending_seq` is audio-thread scratch: the mailbox sequence a MidiInput
        // would consume this block, captured during the ingress pre-fill and
        // committed to the shared mailbox's sequence_seen only after routed dispatch
        // succeeds (so a fallback to the legacy walk re-consumes the same block).
        struct RoutedMidiNode { std::uint32_t plan_index; NodeId id; std::uint64_t pending_seq = 0; };

        // Immutable canonical-executor routing for this snapshot, built in
        // compile_() when the topology is executor-eligible (see
        // signal_graph_topology_executor_eligible). Empty/invalid otherwise. The
        // Gain bindings reference this snapshot's own `runtime[id].gain` atomics
        // and the Plugin/Custom bindings invoke this snapshot's own slots /
        // callbacks, so this carries no keepalive — it lives and dies exactly with
        // this CompiledGraph, published atomically with it via live_slot_. The
        // legacy walk remains the reference/fallback path for ineligible or
        // routing-failed graphs, not a retired one.
        struct RoutedPaths {
            // The default path: compact buffer assignment, slots reused.
            RoutedPath serial;
            // Levelized PARALLEL routing (built only when parallel routing is
            // enabled at compile time). Same plan as `serial` but with a reuse-free
            // buffer assignment (parallel_safe) so concurrent same-level nodes never
            // alias a slot, plus its own (larger) pool and the level schedule. Each
            // of its Plugin bindings owns its fallback scratch so same-level Plugin
            // nodes do not share mutable MIDI output state.
            RoutedPath parallel;
            // The serial path's shared MIDI/param fallback scratch, used when no
            // per-node executor scratch is supplied.
            PluginRoutingScratch plugin_scratch;
            // Per-node MIDI buffers for the routed path's event edges. Empty
            // (node_count 0) for graphs with no MIDI. The routed dispatch bridges
            // the SignalGraph MIDI mailboxes to these buffers around
            // process_routed: a MidiInput node's mailbox is read into its scratch
            // output before the walk, and a MidiOutput node's gathered scratch
            // input is published to its mailbox after. `midi_inputs` / `midi_outputs`
            // list those system nodes by (dense plan index, NodeId) so the bridge
            // avoids a per-block NodeId lookup.
            format::GraphRuntimeMidiScratch midi;
            std::vector<RoutedMidiNode> midi_inputs;
            std::vector<RoutedMidiNode> midi_outputs;
            // Per-node parameter-event queues + per-connection slew state for
            // routed sparse automation. Empty (node_count 0) for graphs with no
            // sparse automation.
            format::GraphRuntimeAutomationScratch automation;
            // The MIDI/automation scratch and the MidiInput/Output node lists are
            // deliberately SHARED across the two paths: the plans are identical and
            // only ONE path runs per block.
        };
        RoutedPaths routed;

        // Anticipative rendering for this snapshot (built only when anticipation is
        // enabled at compile time AND the routed plan has an eligible latent
        // interior). The lane pre-renders the interior off the audio thread; the
        // live path masks the interior in routed.serial's walk and feeds the lane's
        // pre-rendered boundary signals into the interior boundary-source output
        // slots of routed.serial.pool. `skip_mask` is indexed by
        // routed.serial.snapshot's dense node order; `prefill` maps each lane output
        // channel to the routed.serial.pool slot carrying that boundary signal;
        // `consume_scratch` is the per-snapshot capture buffer consume() pops a
        // block into before the prefill copy. The lane owns the interior plugin
        // instances' state advancement — process() never runs them.
        struct AnticipationSplice {
            AnticipationLane lane;
            std::vector<std::uint8_t> skip_mask;
            struct Prefill {
                std::uint32_t out_channel = 0;  // lane output channel
                std::uint32_t slot = 0;         // routed.serial.pool mono slot to fill
            };
            std::vector<Prefill> prefill;
            std::vector<std::vector<float>> consume_scratch;
            std::vector<float*> consume_ptrs;
            bool valid = false;
        };
        AnticipationSplice anticipation;
    };

    std::vector<GraphNode> nodes_;
    std::vector<Connection> connections_;
    // Private authoring identity parallel to connections_. Public Connection
    // stays a value-only routing description; disconnect+reconnect mints a new
    // identity so stale PDC history can never attach to a logically new edge.
    std::vector<std::uint64_t> connection_identities_;
    std::uint64_t next_connection_identity_{1};
    std::unordered_map<std::string, CustomNodeType> custom_node_types_;
    // Bumped on every register_custom_node_type; captured into each CompiledGraph
    // so the 2.2b reinit-free-swap predicate can reject a candidate compiled after
    // the custom registry changed (M6 — prevents binding new callbacks to
    // instances the old factory built). Guarded by graph_mutation_mutex_.
    std::uint64_t custom_registry_generation_{0};
    // Invalidates an outstanding PreparedTopologyEdit when any direct authoring
    // or lifecycle operation touches this graph. Guarded by graph_mutation_mutex_.
    std::uint64_t authoring_generation_{0};
    NodeId next_id_ = 1;
    GraphLimits limits_;

    // 2.2b prerequisite (planning/2026-07-07-signalgraph-swap-and-bake-implementation-plan.md,
    // H2): plugin metadata captured ONCE at prepare() time so compile_() and the
    // executor-routing build never call live PluginSlot metadata methods
    // (parameters()/latency_samples()/wants_transport()). Those reach into the
    // live plugin object (e.g. VST3 getLatencySamples()) and are unsafe to call
    // concurrent with process() — which a swap-time recompile (prepare_swap, 2.2b)
    // would do. Captured after n.plugin->prepare() succeeds (null-first prepare,
    // graph_mutation_mutex_ held, no concurrent process on these instances).
    // NodeId-keyed; cleared + rebuilt on every prepare().
    struct PreparedPluginMetadata {
        std::vector<HostParamInfo> parameters;
        int latency_samples = 0;
        bool wants_transport = false;
    };
    // Captured once per prepare() (cleared + rebuilt); read by compile_ / the
    // routing build / edit-time param validation instead of the live PluginSlot.
    // STALENESS INVARIANT: an entry is valid ONLY while the node set and per-node
    // slot identities are unchanged since the last prepare(). A 2.2b swap never
    // recaptures it — the reinit-free predicate (snapshot_is_plugin_reinit_free_locked_)
    // enforces exactly that invariant, and a non-reinit-free edit falls back to a
    // full prepare() which rebuilds this map. Do NOT read it after a mutation that
    // could add/remove/re-instantiate a node without a matching re-prepare.
    std::unordered_map<NodeId, PreparedPluginMetadata> prepared_plugin_meta_;

    struct StagedReplacement {
        NodeId id = 0;
        PluginInfo info;
        std::shared_ptr<PluginSlot> slot;
        PreparedPluginMetadata metadata;
        pulp::audio::AudioProcessLoadSnapshot warmed_load;
        bool same_identity = false;
    };
    std::unordered_map<NodeId, StagedReplacement> staged_replacements_;

    std::unordered_map<std::uint64_t, PluginInfo> scanned_plugin_catalog_;
    std::unordered_map<std::string, std::uint64_t> scanned_plugin_identity_to_token_;
    std::uint64_t next_scanned_plugin_token_{1};
    LiveSwapDiagnostics last_swap_diagnostics_;
    PluginLoaderForTest live_swap_plugin_loader_for_test_;

    // Audio-thread snapshot, published by prepare() / mutators through the
    // shared reader-pinned RCU primitive. `runtime::Slot` owns the atomic
    // pointer the audio thread loads, the reader count, and the retire list;
    // the pointed-to CompiledGraph stays alive until every pinned reader
    // leaves. See runtime/slot.hpp — a pin guarantees LIFETIME, not constness:
    // the audio thread writes this snapshot's scratch buffers every block.
    runtime::Slot<CompiledGraph> live_slot_;

    // Canonical-executor routing (control toggle, read relaxed on the audio
    // thread). DEFAULT ON: the routed executor is the primary inter-node backend
    // for every eligible graph and is bit-identical to the legacy walk for that
    // subset (proven by the routed-vs-walk parity suite) AND now reports the same
    // per-node node_loads() telemetry, so the default-ON flip is behavior-
    // preserving where it takes effect. Ineligible graphs (per-node automation
    // past the executor's fixed capacity, or a Plugin node with no live slot)
    // still fall back to the legacy walk, which remains the reference oracle the
    // parity tests pin OFF explicitly. One long-lived executor whose telemetry survives re-prepare;
    // it is stateless w.r.t. topology (it takes the snapshot + the snapshot's own
    // pool as arguments) and prepare() never mutates it, so the single audio
    // thread is its only writer (relaxed stat counters). The mutable scratch pool
    // is owned per-snapshot by CompiledGraph (see CompiledGraph::RoutedPath::pool) so it
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

    // Non-null only on the private shadow graph owned by PreparedTopologyEdit.
    // compile_ uses the origin's persistent load measurers for unchanged nodes
    // and keeps newly allocated measurers local until commit transfers them.
    SignalGraph* prepared_edit_origin_{nullptr};

    // 2.2b swap-edit transaction state (guarded by graph_mutation_mutex_).
    // in_swap_edit_ is set between begin_swap_edit() and prepare_swap()/
    // abort_swap_edit(); swap_edit_owner_ is the thread that opened it. While set,
    // invalidate_live_locked_() no-ops for the OWNER thread's allow-set mutations (so
    // the live snapshot keeps playing); any direct invalidate_live_locked_() from a non-owner
    // thread, and the lifecycle/limits/registry mutators which force-abort at their
    // top, cancel the transaction.
    bool in_swap_edit_{false};
    std::thread::id swap_edit_owner_{};
    // Guards the single-producer contract on pump_anticipation: a concurrent or
    // reentrant call degrades to a no-op instead of corrupting the lane's
    // unsynchronized executor/pool/scratch + interior plugin state.
    std::atomic<bool> anticipation_pump_busy_{false};
    // See routed_walk_fallbacks(): incremented when an eligible routed path failed
    // dispatch and process() fell back to the walk. Audio-thread writer only.
    std::atomic<std::uint64_t> routed_walk_fallbacks_{0};
    std::atomic<std::uint32_t> routed_only_execution_owners_{0};
    std::atomic<std::uint64_t> routed_only_execution_failures_{0};
    // See transport_suppressed_for_anticipation(): set by compile_() to the count
    // of transport-sensitive nodes that active anticipation forced exterior
    // (excluded from the ahead-rendered interior so they observe the live
    // transport). Control-thread writer (compile_) only.
    std::atomic<std::uint64_t> transport_suppressed_for_anticipation_{0};
    format::GraphRuntimeWorkerPool worker_pool_;
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

    // Whole-graph process load — one measurer that PERSISTS across snapshots (like
    // node_load_). process_impl brackets each block with begin()/end() via an RAII
    // scope; the control thread reads graph_load() for the live-swap admission gate.
    // Heap-stable behind unique_ptr; snapshot() is concurrency-safe.
    std::unique_ptr<audio::AudioProcessLoadMeasurer> graph_load_ =
        std::make_unique<audio::AudioProcessLoadMeasurer>();

    // Desired per-node live-DSP telemetry enable state. Read by compile_ to seed a
    // new snapshot's store and written by set_live_dsp_telemetry_enabled(); both are
    // control-thread but may be different control threads (host prepare vs UI
    // toggle), so it is atomic.
    std::atomic<bool> desired_live_dsp_telemetry_enabled_{false};

    // Serializes CONTROL-THREAD access to the source-of-truth topology — the
    // nodes_ / connections_ vectors and the plain (non-atomic) GraphNode fields
    // they own (gain, ports, plugin/custom identity, custom state) — AND the
    // snapshot-publication state (live_slot_) that a
    // re-prepare and a concurrent invalidate_live_locked_() both mutate. The control
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
    // The low-level helpers node() / has_path_locked_() / would_create_cycle() /
    // processing_order() / custom_node_type() / audio_rate_modulation_lane() /
    // validate_generated_graph() / total_declared_ports_locked_() do NOT lock and
    // assume the caller already holds this mutex; the public methods above are
    // their only callers on the mutating path. Public methods that delegate to
    // another public mutator (add_custom_node -> add_unresolved_custom_node) do
    // NOT take the lock themselves — the leaf takes it once — to avoid
    // self-deadlock on this non-recursive mutex.
    //
    // ENFORCEMENT, not just prose: a helper that assumes the lock carries a
    // `_locked_` suffix, and every one whose call graph is entirely internal
    // opens with assert_graph_mutation_locked_() — which checks the debug-only
    // owner record GraphMutationLock maintains, so a missed lock fails loudly in
    // a debug/CI build instead of racing silently. Acquire the mutex through
    // GraphMutationLock (never a bare lock_guard) so that record stays accurate.
    // The two helpers reachable from a public method that does NOT lock —
    // has_path_locked_ (via would_create_cycle) and total_declared_ports_locked_
    // (via validate_generated_graph / estimate_generated_graph_work_units) —
    // carry the suffix for their internal contract but cannot assert until those
    // public entry points become locking wrappers over `_locked_` cores.
    //
    // The nodes() / connections() accessors hand out a reference to the live
    // vector and the live_slot_.live()*() snapshot getters read live_slot_.live(); both are governed by
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
    //  - NEVER held while acquiring a Slot reader-pin. The methods
    //    that both edit source state AND reflect into the live snapshot
    //    (set_node_gain) take this mutex for the source write, RELEASE it, then
    //    separately pin the snapshot — so this mutex can never invert lock order
    //    with the RCU drain (Slot::reclaim_if_quiescent / Slot::wait_and_clear,
    //    the latter busy-waited under this mutex by release()).
    mutable std::mutex graph_mutation_mutex_;

#ifndef NDEBUG
    // The thread currently holding graph_mutation_mutex_, recorded by
    // GraphMutationLock and read by assert_graph_mutation_locked_(). Debug-only:
    // it exists to catch a missed lock, so release builds carry neither the field
    // nor the check.
    mutable std::atomic<std::thread::id> graph_mutation_owner_{};
#endif

    // Scoped acquisition of graph_mutation_mutex_. Records the owning thread in a
    // debug build so assert_graph_mutation_locked_() can prove a `_locked_`
    // helper's caller really holds the lock; in a release build it is a plain
    // unique_lock over the same mutex. unlock() releases early for the one caller
    // (prepare_swap) that drops the lock before invoking user callbacks.
    class GraphMutationLock {
    public:
        explicit GraphMutationLock(const SignalGraph& graph)
            : lock_(graph.graph_mutation_mutex_)
#ifndef NDEBUG
            , owner_(&graph.graph_mutation_owner_)
#endif
        {
            note_acquired_();
        }
        ~GraphMutationLock() {
            if (lock_.owns_lock()) note_released_();
        }
        GraphMutationLock(const GraphMutationLock&) = delete;
        GraphMutationLock& operator=(const GraphMutationLock&) = delete;
        void unlock() {
            note_released_();
            lock_.unlock();
        }

    private:
        void note_acquired_() noexcept {
#ifndef NDEBUG
            owner_->store(std::this_thread::get_id(), std::memory_order_relaxed);
#endif
        }
        void note_released_() noexcept {
#ifndef NDEBUG
            owner_->store(std::thread::id{}, std::memory_order_relaxed);
#endif
        }

        std::unique_lock<std::mutex> lock_;
#ifndef NDEBUG
        std::atomic<std::thread::id>* owner_ = nullptr;
#endif
    };

    // Fails a debug build when a `_locked_` helper is reached without this thread
    // holding graph_mutation_mutex_. Compiles to nothing in release.
    void assert_graph_mutation_locked_() const noexcept {
#ifndef NDEBUG
        assert(graph_mutation_owner_.load(std::memory_order_relaxed)
                   == std::this_thread::get_id()
               && "graph_mutation_mutex_ must be held by the calling thread");
#endif
    }

    // Writable counterpart to the const node() lookup — the ONE place the source
    // topology's constness is shed, so the mutators do not each hand-roll a
    // const_cast. Caller holds graph_mutation_mutex_ (same contract as node()).
    GraphNode* node_mut_locked_(NodeId id);
    void append_connection_locked_(Connection connection);
    void erase_connection_at_locked_(std::size_t index);

    bool has_path_locked_(NodeId from, NodeId to) const;
    std::size_t total_declared_ports_locked_() const;
    // Core of audio_rate_modulation_lane() — scans nodes_ via node() and
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
    void process_snapshot_impl(audio::BufferView<float>& output,
                               const audio::BufferView<const float>& input,
                               int num_samples,
                               const format::ProcessContext* transport,
                               CompiledGraph* snapshot);
    static bool inject_midi_into_snapshot_(CompiledGraph& snapshot, NodeId id,
                                           const midi::MidiBuffer& events) noexcept;
    static bool inject_parameter_events_into_snapshot_(
        CompiledGraph& snapshot, NodeId id,
        const state::ParameterEventQueue& events) noexcept;
    static std::uint64_t append_parameter_mailbox_events_(
        void* runtime,
        state::ParameterEventQueue& destination) noexcept;
    // Legacy serial reference walk — the hand-maintained, bit-exact inter-node
    // DSP oracle and fallback for process_impl(). Lives in its own translation
    // unit (signal_graph_reference_walk.cpp) but stays a member so it can name
    // the private nested CompiledGraph / NodeRuntime types without promotion.
    // process_impl() invokes it when no routed path takes a block.
    void run_reference_walk_(audio::BufferView<float>& output,
                             const audio::BufferView<const float>& input,
                             int num_samples,
                             CompiledGraph* cg);
    // compile_ mode. Normal is the full build (prepare()). SwapNoAnticipation is
    // used by (2.2b) prepare_swap: it HARD-skips the anticipation-lane build so a
    // swap-time recompile can never construct an AnticipationLane over the live
    // interior instances (M5 — the reinit-free predicate already requires
    // anticipation off, but this closes the TOCTOU where the host flips the atomic
    // between the predicate check and compile).
    enum class CompileMode { Normal, SwapNoAnticipation };
    std::shared_ptr<CompiledGraph> compile_(double sample_rate, int max_block_size,
                                            CompileMode mode = CompileMode::Normal);
    bool prepare_impl_(double sample_rate, int max_block_size,
                       const PrepareLifecycleObserver* lifecycle_observer);
    // Build one routed-executor snapshot for compile_, defining the live-value
    // resolver set (gain / plugin slot / custom process / custom transport / load
    // measurer / plugin latency / plugin params) ONCE. compile_ builds two
    // snapshots — the serial (parallel_safe=false) and the parallel-safe
    // (parallel_safe=true) path — and both go through here, so the resolvers can
    // never drift between them. The plugin-metadata resolvers read
    // prepared_plugin_meta_, never the live PluginSlot, so a swap-time recompile
    // makes no live metadata call. Called from compile_ (caller holds
    // graph_mutation_mutex_); load resolution takes node_load_mu_ internally.
    bool build_routing_snapshot_locked_(
        CompiledGraph& cg, bool parallel_safe,
        std::vector<PluginBindingContext>& plugin_ctx,
        std::vector<CustomBindingContext>& custom_ctx,
        format::GraphRuntimeSnapshot& out);
    // Shared preflight (generated-graph limits + audio-rate automation
    // event-capacity gate) for prepare() and prepare_swap(). PURE — mutates no
    // live state. Caller holds graph_mutation_mutex_. Returns false (logged) on
    // any rejection.
    bool preflight_locked_(int max_block_size);
    // Reinit-free-swap predicate (PRE-compile half). True iff a live topology
    // swap to the current nodes_/connections_ needs NO plugin/custom re-init and
    // carries no per-snapshot mutable state a fresh snapshot would glitch: same
    // sr/block; unchanged custom registry; anticipation off both sides; no
    // MidiOutput in either the candidate or live snapshot; no smoothed sparse-
    // automation edge; identical node set + plugin/custom instance identity; every
    // resolved plugin node has a cached-metadata entry. Ingress-only MIDI is
    // eligible because a stable MidiInput shares its mailbox and consumed-sequence
    // state across snapshots.
    // The latency-equality gate is checked POST-compile in prepare_swap(). Pure
    // const read; caller holds the mutation mutex.
    bool snapshot_is_plugin_reinit_free_locked_(const CompiledGraph& old_cg,
                                                double sr, int bs) const;
    // Edit-time plugin parameter list for connection validation. Prefers the
    // prepare-time cache (prepared_plugin_meta_) so a connect during a swap-edit
    // does NOT call the live PluginSlot::parameters() concurrently with process()
    // on that slot; falls back to the live slot for a not-yet-prepared node.
    // Edit-path only (not real-time), so the by-value copy is fine.
    std::vector<HostParamInfo> cached_or_live_params_locked_(const GraphNode& n) const;
    std::unique_ptr<PluginSlot> load_live_swap_plugin_locked_(const PluginInfo& info) const;
    pulp::audio::AudioProcessLoadSnapshot warm_staged_slot_locked_(PluginSlot& slot,
                                                                   NodeId id) const;
    bool node_has_feedback_edge_locked_(NodeId id) const;
    bool node_has_parameter_or_automation_contract_locked_(NodeId id) const;
    bool staged_replacement_relaxes_identity_locked_(NodeId id) const;
    void set_live_swap_diagnostics_locked_(LiveSwapFallbackReason reason,
                                           NodeId node,
                                           std::string message);
    SwapResult fail_swap_edit_locked_(LiveSwapFallbackReason reason,
                                      NodeId node,
                                      std::string message);
    void cancel_swap_edit_locked_();
    std::vector<audio::AudioProcessLoadSnapshot>
    staged_node_loads_locked_() const;
    void publish_prepared_stats_locked_(const CompiledGraph& cg);
    void clear_prepared_stats_locked_();
    void invalidate_live_locked_();
    static void compute_latencies_for_(CompiledGraph& cg,
                                       const std::vector<Connection>& connections,
                                       const std::unordered_map<NodeId, PreparedPluginMetadata>&
                                           plugin_meta);
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

// Live-swap admission decision: whether a live plugin-instance swap can proceed
// without risking an xrun, plus a stable reason for diagnostics.
struct LiveSwapAdmission {
    bool admit = false;
    const char* reason = "no history";
};

// Decide whether a live plugin-instance swap is admissible under the current audio
// load. CONSERVATIVE (deny on uncertainty): if the whole-graph OR any staged node has
// fewer than min_callbacks of measured history, deny with "no history" — an unmeasured
// path could xrun and the eager-silence fallback is the safe outcome. Otherwise admit
// iff max(graph.load, graph.last_load) + the sum over staged nodes of
// max(node.load, node.last_load) (the fade's added second-render cost) is within
// headroom_threshold. Pure — no graph state, so it is unit-testable in isolation.
LiveSwapAdmission evaluate_live_swap_admission(
    const audio::AudioProcessLoadSnapshot& graph,
    const std::vector<audio::AudioProcessLoadSnapshot>& staged_nodes,
    float headroom_threshold, std::uint64_t min_callbacks);

} // namespace pulp::host
