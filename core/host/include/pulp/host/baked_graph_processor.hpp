#pragma once

// Lowering a fixed SignalGraph into a single shippable Processor.
//
// `bake()` freezes a prepared, fully-lowerable SignalGraph into one
// BakedGraphProcessor: a self-contained pulp::format::Processor that drives a
// frozen plan through the SAME canonical GraphRuntimeExecutor::process_routed()
// the live graph uses, so its output is bit-identical to the live graph's walk
// for the lowerable subset. There is no codegen and no second routing backend —
// the baked Processor only CALLS process_routed, never defines it.
//
// Lowerable: AudioInput, AudioOutput, the built-in Gain utility, and a Custom
// node whose registered type opts in with `lowerable = true`, matches the node's
// port shape, and is transport-independent (the baked process() drops the host
// transport, so a transport-sensitive node would diverge).
// A graph is REFUSED loudly (null processor + a reason) rather than silently
// mis-baked when it is not prepared, not executor-eligible, or carries a hosted
// Plugin node (opaque external state, not self-contained).
// A plan can be kept in memory (bake) or serialized to a signed .pulpbake
// envelope and read back (bake_to_plan -> write_baked_signed -> load_baked); see
// baked_codec.hpp for the on-disk format and its trust gate.
//
// bake() captures the graph's TOPOLOGY and Gain VALUES, not hot runtime state: the
// baked Processor builds fresh feedback/delay/scratch state in prepare() and starts
// it from zero. If the source graph has already processed blocks (non-zero feedback
// history), that history is not cloned — the baked Processor begins a fresh stream.

#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_codec.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>

#include <atomic>
#include <cstddef>
#include <span>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::host {

// Why a graph could not be lowered into a self-contained BakedGraphProcessor.
enum class LowerRejectReason {
    None,
    NotPrepared,                   // graph.prepare() has not published a snapshot
    NotExecutorEligible,           // outside the routed executor's bit-exact subset
    HostedPluginNotSelfContained,  // a Plugin node carries opaque external state
    CustomNotYetLowerable,         // a Custom node is unresolved, has no lowering yet,
                                   // or its shape does not match its registered type
    CustomNotLowerable,            // a Custom type is not opted into baking (lowerable=false)
    CustomTransportNotLowerable,   // a transport-sensitive Custom node (baked process
                                   // drops the transport, so it would diverge)
    NonAudioLaneNotLowerable,      // a MIDI node, or a MIDI/automation/sidechain edge
    CodecRejected,                 // load_baked: the .pulpbake bytes failed the signed
                                   // envelope (bad magic/length/manifest/trust/signature/
                                   // hash) or the bounded plan parse
    StatefulCustomNotYetLoadable,  // load_baked v1: a Custom node carries opaque state; the
                                   // on-disk stateful-custom path is not supported yet
};

// Result of bake(): on success `processor` is non-null and `accepted` is true;
// on refusal `processor` is null, `accepted` is false, and `reason` /
// `offending_node` / `message` explain the refusal loudly.
struct LowerResult {
    std::unique_ptr<pulp::format::Processor> processor;
    bool accepted = false;
    LowerRejectReason reason = LowerRejectReason::None;
    NodeId offending_node = 0;
    std::string message;
};

// Proof that a graph topology is lowerable into a self-contained baked Processor.
// The single gate both bake() and (later) the on-disk load path consult, so the
// trusted and untrusted paths can never diverge on what is bakeable. accepted==true
// means every node kind is bakeable (audio I/O + Gain; Plugin/Custom/MIDI refused
// with a specific reason), every connection is plain audio, and the topology is
// executor-eligible. Pure — no allocation, no graph state. bake() calls it after
// its is_prepared() precondition.
struct LowerabilityProof {
    bool accepted = false;
    NodeId offending_node = 0;
    LowerRejectReason reason = LowerRejectReason::None;
    std::string message;
};

// `resolve_custom`, when set, resolves a Custom node's registered type (by
// custom_type_id + custom_type_version) so the gate can accept a lowerable,
// shape-matched, non-transport-sensitive Custom node. When empty (the default) any
// Custom node is refused as CustomNotYetLowerable — the in-process bake path passes
// a resolver over the graph's registry; a caller with no registry omits it.
LowerabilityProof lowerability_of(
    std::span<const GraphNode> nodes,
    std::span<const Connection> connections,
    const std::function<const CustomNodeType*(std::string_view type_id, int version)>&
        resolve_custom = {});

// Per-Custom-node lifecycle hooks captured at bake() from the node's registered
// type + live instance. prepare() runs them (control thread) so a re-prepare is a
// real re-init boundary: the instance is re-prepared at the HOST's actual
// rate/block (not whatever the source graph — or load_baked's nominal
// 48k/512 — last used) and its DSP state is reset, honoring the "starts a
// fresh stream" contract above. Either fn may be empty (type registered no
// such hook); each closure holds the instance shared_ptr, so it is also a
// keepalive.
struct CustomNodeLifecycle {
    std::function<void(double sample_rate, int max_block)> prepare;
    std::function<void()> reset;
};

// A SignalGraph frozen into a shippable Processor. Owns the reconstructed plan
// (nodes + connections), its own heap-stable Gain atomics, and the canonical
// executor's serialized routing snapshot + scratch pool. process() bridges the
// host's main in/out buffers into a ProcessBlock and runs the frozen plan via
// GraphRuntimeExecutor::process_routed(). It must NOT define process_routed /
// process_parallel (single-backend invariant) — only call them.
//
// In-place hosts: process() supports input and output views over the SAME
// memory (Logic-style aliased buffers). process_routed zeroes the main output
// bus before gathering AudioInput, which would destroy an aliased input — so
// process() detects overlap and reads the input from a prepare()-sized scratch
// copy instead (audio thread does only the memcpy; no allocation).
class BakedGraphProcessor : public pulp::format::Processor {
public:
    BakedGraphProcessor(std::vector<GraphNode> nodes,
                        std::vector<Connection> connections,
                        int input_channels,
                        int output_channels,
                        std::string name,
                        std::string bundle_id,
                        // Resolved Custom-node process callbacks captured from the
                        // live graph at bake() (NodeId → fn). Each fn is a COPY that
                        // captured the custom instance shared_ptr by value, so the
                        // baked Processor OWNS the instance keepalive — no external
                        // state. Empty for a custom-free graph.
                        std::unordered_map<NodeId, CustomNodeProcessFn>
                            custom_processors = {},
                        // Per-node lifecycle hooks (NodeId → prepare/reset) for
                        // stateful Custom instances, run by prepare() so a
                        // re-prepare re-inits the instance's DSP state at the
                        // host's real rate/block. Empty for stateless nodes.
                        std::unordered_map<NodeId, CustomNodeLifecycle>
                            custom_lifecycles = {});

    pulp::format::PluginDescriptor descriptor() const override;
    void define_parameters(pulp::state::StateStore& store) override;
    void prepare(const pulp::format::PrepareContext& context) override;
    void process(pulp::audio::BufferView<float>& audio_output,
                 const pulp::audio::BufferView<const float>& audio_input,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::format::ProcessContext& context) override;

private:
    // The frozen plan, captured at bake() time and owned for the Processor's
    // lifetime. Gain values ride GraphNode::gain; prepare() seeds the atomics.
    std::vector<GraphNode> nodes_;
    std::vector<Connection> conns_;

    // Heap-stable per-Gain-node atomics (one unique_ptr each) so the routed Gain
    // binding's user_data pointer survives gains_ growth. Built in prepare().
    std::vector<std::unique_ptr<std::atomic<float>>> gains_;
    std::unordered_map<NodeId, std::size_t> gain_index_;

    // Canonical executor + the serialized fused plan it runs. The snapshot and
    // pool are sized once in prepare() (off the audio thread); process() only
    // calls process_routed, which is allocation-free for a fitting pool.
    pulp::format::GraphRuntimeExecutor executor_;
    pulp::format::GraphRuntimeSnapshot snapshot_;
    pulp::format::GraphRuntimeBufferPool pool_;
    // Empty for this slice's lowerable subset (no Plugin nodes), but the
    // snapshot builder requires the storage to exist.
    std::vector<PluginBindingContext> plugin_ctx_;
    PluginRoutingScratch plugin_scratch_;
    // Resolved Custom-node process callbacks (NodeId → fn, each holding its
    // instance keepalive) + the executor's Custom binding storage. Empty for a
    // custom-free graph; prepare() binds custom_ctx_ from custom_processors_.
    std::unordered_map<NodeId, CustomNodeProcessFn> custom_processors_;
    std::vector<CustomBindingContext> custom_ctx_;
    // Per-node stateful-Custom lifecycle hooks captured at bake(); prepare()
    // runs each (prepare at the host's real rate/block, then reset) so stale
    // instance state — e.g. a delay line's contents — never survives a
    // re-prepare. Control-thread only, like the graph's own instance prepare.
    std::unordered_map<NodeId, CustomNodeLifecycle> custom_lifecycles_;

    // In-place-host guard scratch: when the host's input channels alias its
    // output channels, process() copies the input here BEFORE process_routed
    // zeroes the output bus. Sized in prepare() (input_channels_ × max block);
    // the audio thread only detects overlap and memcpys — no allocation.
    std::vector<float> input_alias_scratch_;
    std::vector<float*> input_alias_ptrs_;

    std::string name_;
    std::string bundle_id_;
    int input_channels_ = 2;
    int output_channels_ = 2;
    int prepared_max_block_ = 0;
    bool prepared_ = false;
};

// Lower a prepared, fully-lowerable SignalGraph into a BakedGraphProcessor.
// Returns LowerResult{accepted=true, processor=...} on success, or a loud
// refusal (processor=nullptr) with a reason for any non-lowerable graph.
LowerResult bake(const SignalGraph& graph);

// Verify + load a signed .pulpbake artifact into a BakedGraphProcessor. `trust` is
// the set of accepted signer keys; `custom_types` are the host's registered custom
// node types (a Custom record's process code is re-resolved from these, never from
// the file). The bytes go through verify_and_extract_plan (signature-before-parse),
// then the plan is rebuilt into a SignalGraph and run through bake() — so bake()'s
// full lowerability re-proof applies to the reconstructed topology; the file's
// implicit claim is never trusted. Returns CodecRejected if the envelope/parse fails,
// or bake()'s refusal reason if the reconstructed graph is not lowerable. v1 supports
// stateless custom nodes; a Custom record carrying opaque state is refused
// (StatefulCustomNotYetLoadable).
LowerResult load_baked(std::span<const std::uint8_t> bytes, const BakedTrust& trust,
                       const std::vector<CustomNodeType>& custom_types);

// Result of bake_to_plan(): `plan` is set iff `accepted`; on refusal the reason /
// offending_node / message explain why (mirrors bake()'s LowerResult so the write
// front door can report a non-shippable graph as precisely as the in-memory path).
struct BakePlanResult {
    std::optional<BakedPlan> plan;
    bool accepted = false;
    LowerRejectReason reason = LowerRejectReason::None;
    NodeId offending_node = 0;
    std::string message;
};

// Extract a serializable BakedPlan from a prepared, lowerable graph — the write-path
// front door: bake_to_plan(graph).plan -> write_baked_signed(*plan, key) -> bytes.
// Refuses (accepted=false, plan=nullopt) with a reason if the graph is not prepared
// or not lowerable (same proof as bake()).
BakePlanResult bake_to_plan(const SignalGraph& graph);

} // namespace pulp::host
