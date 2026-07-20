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

namespace detail {
// Per-node injection substrate; full definitions live in the .cpp so this
// widely-included header stays free of param_cursor/store.
struct BakedParamMailbox;
struct BakedParamNodeState;
}  // namespace detail

// Bake-layer parameter-injection binding captured at bake() for one param-
// declaring custom node: the bound param-aware process callback (instance
// keepalive captured) plus the node's declared params. Handed to the
// BakedGraphProcessor so it can size the node's mailbox + build its cursor.
struct BakedCustomParamBinding {
    CustomNodeParamProcessFn process;
    std::vector<CustomNodeBakedParam> params;
};

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

// Outcome of a ParamInjector::inject() call. Distinguishes the two failure
// modes the old bool return conflated: a dead handle (never call inject() on an
// invalid() injector) versus a source queue that overflowed before it reached
// us (the retained prefix is still published — a real, usable partial success).
enum class InjectStatus {
    Ok,               // published in full
    PartialOverflow,  // the source queue overflowed; its retained prefix is published
    InvalidHandle,    // this injector holds no live claim (valid() == false); nothing published
};

// Exclusive control-side handle for injecting parameters into ONE baked custom
// node. Obtain from BakedGraphProcessor::claim_param_injection(); move-only;
// releases the node's exclusive claim on destruction. inject() is RT-safe and
// allocation-free — it publishes into the node's single-writer mailbox, which
// the baked process() drains on its next block. Precedence mirrors the live
// mailbox: latest-published-queue wins; WITHIN a queue events are sample-
// accurate and may ramp (ParamCursor over sample_offset +
// ramp_duration_sample_frames), and a ramp longer than one block continues to
// completion across subsequent blocks. Exactly one injector may hold a node at
// a time (two owners injecting into one node is the hazard this claim prevents).
class ParamInjector {
public:
    ParamInjector() noexcept = default;
    ~ParamInjector();
    ParamInjector(ParamInjector&& other) noexcept;
    ParamInjector& operator=(ParamInjector&& other) noexcept;
    ParamInjector(const ParamInjector&) = delete;
    ParamInjector& operator=(const ParamInjector&) = delete;

    // True iff this handle holds a live exclusive claim on a node's mailbox.
    bool valid() const noexcept { return mailbox_ != nullptr; }

    // Publish a whole queue of sample-accurate events (the general path).
    // Returns InvalidHandle iff !valid() (nothing published — this is a caller
    // error, inject() is meant to be called only on a valid() handle), or
    // PartialOverflow when `events` already reports source-side overflow (the
    // retained prefix is still published — a usable partial success), otherwise
    // Ok. RT-safe / allocation-free.
    InjectStatus inject(const state::ParameterEventQueue& events) noexcept;
    // Convenience: publish a single immediate (ramp_duration==0) or ramped
    // event. A single event never overflows, so this returns only Ok or
    // InvalidHandle.
    InjectStatus inject(const state::ParameterEvent& event) noexcept;

private:
    friend class BakedGraphProcessor;
    explicit ParamInjector(std::shared_ptr<detail::BakedParamMailbox> mailbox) noexcept;
    void release() noexcept;
    std::shared_ptr<detail::BakedParamMailbox> mailbox_;
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
                            custom_lifecycles = {},
                        // Bake-layer param-injection bindings (NodeId → bound
                        // param-aware process + declared params). Empty for a
                        // graph with no param-declaring custom nodes.
                        std::unordered_map<NodeId, BakedCustomParamBinding>
                            param_bindings = {});

    ~BakedGraphProcessor() override;

    pulp::format::PluginDescriptor descriptor() const override;
    void define_parameters(pulp::state::StateStore& store) override;
    void prepare(const pulp::format::PrepareContext& context) override;
    void process(pulp::audio::BufferView<float>& audio_output,
                 const pulp::audio::BufferView<const float>& audio_input,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::format::ProcessContext& context) override;

    // Claim exclusive parameter-injection rights for a baked custom node. Returns
    // a valid ParamInjector iff `node` is a param-declaring baked custom node AND
    // no injector currently holds it; otherwise returns an invalid handle. The
    // returned handle survives prepare()/re-prepare (the mailbox persists across
    // both). Control thread only.
    ParamInjector claim_param_injection(NodeId node) noexcept;

private:
    // Build per-node injection state and install the draining wrapper into
    // custom_processors_ for each param-declaring custom node. Called by prepare()
    // before the executor snapshot is built; off the audio thread.
    void prepare_param_injection();

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

    // Bake-layer parameter injection. `param_bindings_` is captured at bake().
    // `param_mailboxes_` (one single-writer mailbox per param node) is built in
    // the constructor and PERSISTS across prepare() so a control-side claim is
    // never dropped by a re-prepare. `param_states_` (per-node store + cursor
    // scratch + held values) is (re)built in prepare(); prepare() also installs a
    // draining wrapper into custom_processors_ for each param node BEFORE the
    // executor snapshot is built, so the routed path invokes the injection path.
    // A4 (should-fix-soon, not this slice): a param node is currently keyed
    // across four NodeId-indexed maps (custom_processors_, param_bindings_,
    // param_mailboxes_, param_states_). Consider consolidating into one
    // NodeId → struct map once the shape settles.
    std::unordered_map<NodeId, BakedCustomParamBinding> param_bindings_;
    std::unordered_map<NodeId, std::shared_ptr<detail::BakedParamMailbox>>
        param_mailboxes_;
    std::unordered_map<NodeId, std::unique_ptr<detail::BakedParamNodeState>>
        param_states_;

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
