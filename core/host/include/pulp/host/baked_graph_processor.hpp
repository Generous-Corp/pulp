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
// Lowerable today: AudioInput, AudioOutput, and the built-in Gain utility.
// A graph is REFUSED loudly (null processor + a reason) rather than silently
// mis-baked when it is not prepared, not executor-eligible, or carries a hosted
// Plugin node (opaque external state, not self-contained) or a Custom node.
// Custom-node lowering and on-disk plan serialization are deliberate follow-ups;
// this slice keeps the captured plan as in-memory owned data.
//
// bake() captures the graph's TOPOLOGY and Gain VALUES, not hot runtime state: the
// baked Processor builds fresh feedback/delay/scratch state in prepare() and starts
// it from zero. If the source graph has already processed blocks (non-zero feedback
// history), that history is not cloned — the baked Processor begins a fresh stream.

#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
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
    CustomNotYetLowerable,         // a Custom node has no lowering yet
    NonAudioLaneNotLowerable,      // a MIDI node, or a MIDI/automation/sidechain edge
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

// A SignalGraph frozen into a shippable Processor. Owns the reconstructed plan
// (nodes + connections), its own heap-stable Gain atomics, and the canonical
// executor's serialized routing snapshot + scratch pool. process() bridges the
// host's main in/out buffers into a ProcessBlock and runs the frozen plan via
// GraphRuntimeExecutor::process_routed(). It must NOT define process_routed /
// process_parallel (single-backend invariant) — only call them.
class BakedGraphProcessor : public pulp::format::Processor {
public:
    BakedGraphProcessor(std::vector<GraphNode> nodes,
                        std::vector<Connection> connections,
                        int input_channels,
                        int output_channels,
                        std::string name,
                        std::string bundle_id);

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

} // namespace pulp::host
