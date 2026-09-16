#pragma once

#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/processor_block_adapter.hpp>
#include <pulp/state/store.hpp>

#include <memory>
#include <span>
#include <vector>

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
/// The binding attaches gathered sparse parameter events, MIDI, and borrowed
/// dense modulation lanes through one EventBlock. Legacy processors still enter
/// through process_processor_block(); processors that declare dense consumption
/// enter through Processor::process_block().
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
    /// configuration changes; call with the audio thread stopped. Call release()
    /// before re-preparing this non-owning adapter.
    bool prepare(const PrepareContext& context);
    bool release() noexcept;

    /// Build the only valid graph binding for this adapter. The cached
    /// capability selects dense views or the legacy event expansion.
    GraphRuntimeNodeBinding binding(graph::NodeId node_id, bool required = true) noexcept;

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
    NodeCapabilities capabilities_{};
    bool prepared_ = false;
};

/// Retained, self-contained lifetime for an authored in-process graph node.
/// Creation freezes the descriptor and parameter catalog off the audio thread;
/// published graph snapshots retain the shared instance through quiescence.
class ProcessorNodeInstance final {
  public:
    static std::shared_ptr<ProcessorNodeInstance>
    create(std::unique_ptr<Processor> processor) noexcept;

    ~ProcessorNodeInstance();
    ProcessorNodeInstance(const ProcessorNodeInstance&) = delete;
    ProcessorNodeInstance& operator=(const ProcessorNodeInstance&) = delete;

    bool prepare(const PrepareContext& context) noexcept;
    bool release() noexcept;
    GraphRuntimeNodeBinding binding(graph::NodeId node_id, bool required = true) noexcept {
        return adapter_.binding(node_id, required);
    }

    const PluginDescriptor& descriptor() const noexcept {
        return descriptor_;
    }
    std::span<const state::ParamInfo> parameter_catalog() const noexcept {
        return parameter_catalog_;
    }
    const state::ParamInfo* parameter(state::ParamID id) const noexcept;
    state::StateStore& state_store() noexcept {
        return state_store_;
    }
    const state::StateStore& state_store() const noexcept {
        return state_store_;
    }
    Processor& processor() noexcept {
        return *processor_;
    }
    const Processor& processor() const noexcept {
        return *processor_;
    }

  private:
    explicit ProcessorNodeInstance(std::unique_ptr<Processor> processor)
        : processor_(std::move(processor)), adapter_(*processor_) {}

    state::StateStore state_store_;
    // Declared after the store so Processor destruction runs first while its
    // StateStore pointer is still valid.
    std::unique_ptr<Processor> processor_;
    PluginDescriptor descriptor_;
    std::vector<state::ParamInfo> parameter_catalog_;
    ProcessorNode adapter_;
    bool prepared_ = false;
};

} // namespace pulp::format
