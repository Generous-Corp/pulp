#pragma once

// Stable node vocabulary and retained node state for the Pulp host signal graph.

#include <pulp/host/graph_types.hpp>
#include <pulp/host/plugin_slot.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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

}  // namespace pulp::host
