#pragma once

#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/audio_input_provider.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/processor_block_adapter.hpp>
#include <pulp/state/store.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <array>
#include <span>
#include <string>
#include <vector>

namespace pulp::format {

/// Asked on the audio thread at the start of a node's block: "does the owner
/// of this node say it has nothing to render right now?". Must be
/// allocation-free, lock-free and must not throw.
struct ProcessorIdleProbe {
    bool (*is_idle)(void* context) noexcept = nullptr;
    void* context = nullptr;
    explicit operator bool() const noexcept {
        return is_idle != nullptr;
    }
};

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

    /// Install a host-owned route provider before prepare(). The handle is
    /// copied as a small immutable POD and read from the audio thread without
    /// allocation or locking. Clearing it requires the audio thread to be
    /// stopped and the node to be prepared again.
    void set_audio_input_provider(AudioInputBusProvider provider) noexcept {
        input_provider_ = provider;
    }

    /// Opt this node into idle skipping (off unless a probe is installed).
    /// Install before prepare(), like set_audio_input_provider().
    ///
    /// While the probe reports idle, the node may skip calling the wrapped
    /// Processor and write silence (and an empty MIDI output) instead. The
    /// node is skipped only after it has run while idle and produced an
    /// all-zero-bits block, repeated until the declared latency plus tail has
    /// elapsed; any non-zero sample, any inbound event or audio, a failed
    /// block, or reset_idle_confirmation() starts the confirmation over.
    ///
    /// Contract the caller takes on: at every point the probe flips from idle
    /// to busy, the processor's output must not depend on how many blocks it
    /// was run while idle. Processors with free-running state (an LFO, noise,
    /// a clock, a parameter smoother that keeps moving, a transport follower)
    /// break that contract and must not be given a probe.
    ///
    /// Eligibility is decided in prepare(): the descriptor must declare no
    /// input bus, the processor must not consume audio-rate modulation, its
    /// tail must be bounded (tail_samples >= 0) and latency_samples() must be
    /// non-negative. An ineligible node is simply never skipped; check
    /// idle_skip_armed() after prepare().
    ///
    /// Do not install a probe on a node that can be rendered ahead of time on
    /// another thread: the probe is read on the node's own render thread.
    void set_idle_probe(ProcessorIdleProbe probe) noexcept {
        idle_probe_ = probe;
    }
    /// True after prepare() when a probe is installed and the node is eligible.
    bool idle_skip_armed() const noexcept {
        return idle_armed_;
    }
    /// Blocks skipped since prepare(). Relaxed read; safe from any thread.
    std::uint64_t idle_skipped_blocks() const noexcept {
        return idle_skipped_blocks_.load(std::memory_order_relaxed);
    }
    /// Forget that silence was confirmed, so the next block runs the
    /// processor. Call with the audio thread stopped or between blocks on the
    /// render thread (for example after a parameter write).
    void reset_idle_confirmation() noexcept {
        idle_confirmed_ = false;
        idle_silent_frames_ = 0;
    }

    /// Build the only valid graph binding for this adapter. The cached
    /// capability selects dense views or the legacy event expansion.
    GraphRuntimeNodeBinding binding(graph::NodeId node_id, bool required = true) noexcept;

    /// Routed node binding entry point. `user_data` must be the owning
    /// ProcessorNode. Reads the executor's gathered mono input view
    /// (ctx.node_inputs) and writes this node's mono output view
    /// (ctx.node_outputs) by aliasing them into a block-local ProcessBlock
    /// over the node's reused bus set, then forwards to
    /// process_processor_block(). Allocation-free
    /// per block and only valid on the routed path (ctx.routed == true).
    static bool process_binding(ProcessBlock& block,
                                const GraphRuntimeNodeProcessContext& ctx,
                                void* user_data) noexcept;

    Processor& processor() noexcept { return *processor_; }
    const Processor& processor() const noexcept { return *processor_; }

  private:
    // The routed block for a node with one main input and one main output and
    // no dense modulation: the views and context are prepared, so a block only
    // points them at this block's buffers (see process_binding).
    bool process_main_bus_block(const ProcessBlock& block,
                                const GraphRuntimeNodeProcessContext& ctx) noexcept;
    // The routed block body without idle skipping.
    bool process_routed_block(ProcessBlock& block,
                              const GraphRuntimeNodeProcessContext& ctx) noexcept;

    Processor* processor_;
    // Caller-owned MIDI/parameter fallback storage for the block bridge. Sized
    // once in prepare() and reused across blocks; never resized on the RT path.
    ProcessorBlockAdapterScratch scratch_;
    // The block's bus set, rebuilt per block by clear() and add_*(). Reusing it
    // avoids constructing all kMaxBuses entries for the one to three buses a
    // node uses.
    BusBufferSet buses_;
    // Chosen at prepare(): true when the node has at most one (main) input bus
    // and no dense modulation, so process_binding takes process_main_bus_block.
    bool main_bus_fast_path_ = false;
    // The context handed to the processor when the block carries no complete
    // transport; only sample_rate and num_samples change per block.
    ProcessContext block_context_{};
    bool block_context_from_transport_ = false;
    NodeCapabilities capabilities_{};
    AudioInputBusProvider input_provider_{};
    std::array<BusRole, BusBufferSet::kMaxBuses> input_roles_{};
    std::array<int, BusBufferSet::kMaxBuses> input_channels_{};
    std::array<bool, BusBufferSet::kMaxBuses> input_optional_{};
    // Descriptor-owned names are copied during prepare() so the routed block
    // can publish stable, non-owning string_views without allocating on RT.
    std::array<std::string, BusBufferSet::kMaxBuses> input_names_{};
    std::size_t input_bus_count_ = 0;
    bool prepared_ = false;
    // Idle skipping. Plain members are touched only from the node's render
    // thread (a node never runs on two threads at once); the counter is atomic
    // so another thread can read it.
    ProcessorIdleProbe idle_probe_{};
    bool idle_armed_ = false;
    bool idle_confirmed_ = false;
    std::uint64_t idle_silent_frames_ = 0;
    std::uint64_t idle_hold_frames_ = 0;
    std::atomic<std::uint64_t> idle_skipped_blocks_{0};
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
    void set_audio_input_provider(AudioInputBusProvider provider) noexcept {
        adapter_.set_audio_input_provider(provider);
    }
    void set_idle_probe(ProcessorIdleProbe probe) noexcept {
        adapter_.set_idle_probe(probe);
    }
    bool idle_skip_armed() const noexcept {
        return adapter_.idle_skip_armed();
    }
    std::uint64_t idle_skipped_blocks() const noexcept {
        return adapter_.idle_skipped_blocks();
    }
    void reset_idle_confirmation() noexcept {
        adapter_.reset_idle_confirmation();
    }
    GraphRuntimeNodeBinding binding(graph::NodeId node_id, bool required = true) noexcept {
        return adapter_.binding(node_id, required);
    }

    const PluginDescriptor& descriptor() const noexcept {
        return descriptor_;
    }
    int graph_type_version() const noexcept {
        return graph_type_version_;
    }
    void set_graph_type_version(int version) noexcept {
        graph_type_version_ = version;
    }
    std::string_view graph_type_id() const noexcept {
        return graph_type_id_;
    }
    void set_graph_type_id(std::string id) {
        graph_type_id_ = std::move(id);
    }
    int graph_state_schema_version() const noexcept {
        return graph_state_schema_version_;
    }
    void set_graph_state_schema_version(int version) noexcept {
        graph_state_schema_version_ = version;
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
    int graph_type_version_ = 1;
    std::string graph_type_id_;
    int graph_state_schema_version_ = 1;
    ProcessorNode adapter_;
    bool prepared_ = false;
};

} // namespace pulp::format
