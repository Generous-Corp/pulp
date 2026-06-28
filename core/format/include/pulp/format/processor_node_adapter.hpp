#pragma once

#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/processor_block_adapter.hpp>

namespace pulp::format {

/// Runs a real pulp::format::Processor as a node inside the routed graph.
///
/// ProcessorNode is the in-process adapter between a routed graph node and the
/// legacy Processor::process() ABI. It owns nothing about routing: the graph's
/// GraphRuntimeExecutor still gathers a node's inputs and assigns its output
/// slots; ProcessorNode only translates the per-node routed I/O the executor
/// hands it into a ProcessBlock and forwards to process_processor_block(). There
/// is no second routing path — a ProcessorNode is wired into a snapshot exactly
/// like any other GraphRuntimeNodeBinding.
///
/// Current scope: one Main mono audio input and one Main mono
/// audio output. Parameters, MIDI, latency, and state are deliberately out of
/// scope; the binding presents no EventBlock to the processor, so a processor
/// that emits MIDI on this path sees the bridge's discard sink.
///
/// Lifetime: the wrapped Processor is non-owning and must outlive the node and
/// every routed block that references it. prepare() runs off the realtime thread
/// and must precede the first process_binding() call.
class ProcessorNode {
public:
    /// Wrap a non-owning processor. The processor must already have its
    /// StateStore bound (see HeadlessHost / format adapters) before prepare().
    explicit ProcessorNode(Processor& processor) noexcept
        : processor_(&processor) {}

    /// Off-RT preparation: prepares the wrapped processor for the given audio
    /// configuration and sizes the block-adapter scratch so process_binding()
    /// stays allocation-free. Idempotent enough to re-run when the
    /// configuration changes; call with the audio thread stopped.
    bool prepare(const PrepareContext& context);

    /// Routed node binding entry point. `user_data` must be the owning
    /// ProcessorNode. Reads the executor's gathered mono input view
    /// (ctx.node_inputs) and writes this node's mono output view
    /// (ctx.node_outputs) by aliasing them into a stack-local single-bus
    /// ProcessBlock, then forwards to process_processor_block(). Allocation-free
    /// per block and only valid on the routed path (ctx.routed == true).
    static bool process_binding(ProcessBlock& block,
                                const GraphRuntimeNodeProcessContext& ctx,
                                void* user_data) noexcept;

    Processor& processor() noexcept { return *processor_; }
    const Processor& processor() const noexcept { return *processor_; }

private:
    Processor* processor_;
    // Caller-owned MIDI/parameter fallback storage for the block bridge. Sized
    // once in prepare() and reused across blocks; never resized on the RT path.
    ProcessorBlockAdapterScratch scratch_;
};

} // namespace pulp::format
