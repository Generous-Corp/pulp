#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/graph/graph_runtime_buffer_assignment.hpp>
#include <pulp/graph/graph_runtime_levelization.hpp>
#include <pulp/graph/graph_runtime_plan.hpp>
#include <pulp/graph/graph_runtime_queue.hpp>
#include <pulp/format/graph_runtime_worker_pool.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::audio {
class AudioProcessLoadMeasurer;
}

namespace pulp::format {

enum class GraphRuntimeCommandStatus : std::uint8_t {
    Accepted,
    Rejected,
};

enum class GraphRuntimeExecutorErrorCode : std::uint8_t {
    None,
    InvalidProcessBlock,
    InvalidSnapshot,
    CommandScratchTooSmall,
    MissingRequiredProcessor,
    NodeProcessorFailed,
    BufferPoolTooSmall,
    NodePortLimitExceeded,
};

struct GraphRuntimeCommandDecision {
    graph::GraphTimedCommand command;
    GraphRuntimeCommandStatus status = GraphRuntimeCommandStatus::Rejected;
};

struct GraphRuntimeNodeProcessContext {
    const graph::GraphRuntimePlan* plan = nullptr;
    const graph::GraphRuntimeNodePlan* node = nullptr;
    std::uint32_t node_index = 0;
    std::span<const GraphRuntimeCommandDecision> command_results;
    // True when this context came from process_routed(): node_inputs/node_outputs
    // are populated and a binding MUST read/write them rather than the block's
    // buses. False on the shared-block process() path, where they are empty and
    // a binding works the buses directly. A binding should assert the mode it
    // expects — the shared-block path is slated to retire once callers migrate
    // onto routing, at which point this flag and the dual mode collapse.
    bool routed = false;
    // Per-node routed I/O (valid only when `routed`). A routing binding reads
    // `node_inputs` (already gathered from upstream output slots) and writes
    // `node_outputs` (its assigned scratch slots). Channels are mono slots.
    audio::BufferView<const float> node_inputs;
    audio::BufferView<float> node_outputs;
    // Per-node MIDI (non-null only on the routing path when the call supplies a
    // GraphRuntimeMidiScratch). `node_midi_in` holds the events gathered from
    // this node's inbound event connections (the executor fills it before the
    // binding runs); a binding that emits MIDI writes `node_midi_out` (clearing
    // it first). Null when the graph carries no MIDI — a binding then falls back
    // to its own scratch.
    midi::MidiBuffer* node_midi_in = nullptr;
    midi::MidiBuffer* node_midi_out = nullptr;
    // Per-node parameter automation events (non-null only on the routing path
    // when the call supplies a GraphRuntimeAutomationScratch). The executor fills
    // it from this node's inbound automation connections before the binding runs;
    // a plugin binding passes it to PluginSlot::process. Null when the graph
    // carries no automation — a binding then uses an empty queue.
    state::ParameterEventQueue* node_param_events = nullptr;
};

using GraphRuntimeNodeProcessFn = bool (*)(
    ProcessBlock& block,
    const GraphRuntimeNodeProcessContext& context,
    void* user_data) noexcept;

using GraphRuntimeCommandApplyFn = GraphRuntimeCommandStatus (*)(
    ProcessBlock& block,
    const graph::GraphRuntimePlan& plan,
    const graph::GraphTimedCommand& command,
    void* user_data) noexcept;

struct GraphRuntimeCommandHandler {
    // Block-level side effects are applied before node processing, so this
    // handler is only invoked for block_offset == 0 commands until the executor
    // grows block-splitting/sample-accurate command application.
    GraphRuntimeCommandApplyFn apply = nullptr;
    void* user_data = nullptr;
};

struct GraphRuntimeNodeBinding {
    graph::NodeId node_id = 0;
    GraphRuntimeNodeProcessFn process = nullptr;
    void* user_data = nullptr;
    bool required = true;
    // Optional per-node CPU-load measurer. When set, the executor wraps this
    // node's per-block work in begin()/end() so routed execution attributes
    // per-node load exactly as the legacy walk does. Null = not measured. The
    // pointee is owned elsewhere (the host's persistent per-node measurer map)
    // and must outlive the snapshot; the executor only calls begin()/end().
    audio::AudioProcessLoadMeasurer* load = nullptr;
};

/// Control-thread-built graph snapshot for GraphRuntimeExecutor.
///
/// A snapshot is logically immutable after a successful reset(). Do not call
/// reset() or clear() on a snapshot object while any realtime process() call may
/// still reference it; use a publish/lifetime policy above this primitive.
class GraphRuntimeSnapshot {
public:
    // `parallel_safe` builds the buffer assignment without slot reuse so the
    // snapshot can drive process_parallel (concurrent nodes never alias a
    // recycled slot). Default false = the compact serial layout for
    // process_routed. Both layouts produce identical output; parallel just costs
    // more slots.
    bool reset(graph::GraphRuntimePlan plan,
               std::span<const GraphRuntimeNodeBinding> bindings,
               bool parallel_safe = false);
    void clear() noexcept;

    // True if this snapshot's assignment is reuse-free (safe for process_parallel).
    bool parallel_safe() const noexcept { return parallel_safe_; }

    bool valid() const noexcept;
    const graph::GraphRuntimePlan& plan() const noexcept { return plan_; }
    std::span<const GraphRuntimeNodeBinding> bindings() const noexcept {
        return bindings_;
    }
    std::uint32_t node_count() const noexcept { return plan_.node_count(); }

    // Scratch-slot layout for the routing process() path, computed off-RT in
    // reset() from the plan. slot_count() is the number of mono slots a backing
    // GraphRuntimeBufferPool must provide.
    const graph::GraphRuntimeBufferAssignment& buffer_assignment() const noexcept {
        return assignment_;
    }
    std::uint32_t buffer_slot_count() const noexcept { return assignment_.slot_count; }

private:
    graph::GraphRuntimePlan plan_;
    std::vector<GraphRuntimeNodeBinding> bindings_;
    graph::GraphRuntimeBufferAssignment assignment_;
    bool parallel_safe_ = false;
};

struct GraphRuntimeEventSink {
    void* user_data = nullptr;
    bool (*push_event)(void* user_data, const graph::GraphEvent& event) noexcept = nullptr;

    bool push(const graph::GraphEvent& event) const noexcept {
        return push_event ? push_event(user_data, event) : true;
    }
};

struct GraphRuntimeExecutorStats {
    std::uint64_t blocks_processed = 0;
    std::uint64_t nodes_processed = 0;
    std::uint64_t commands_drained = 0;
    std::uint64_t commands_accepted = 0;
    std::uint64_t commands_rejected = 0;
    std::uint64_t events_dropped = 0;
    std::uint64_t invalid_blocks = 0;
    std::uint64_t invalid_snapshots = 0;
    std::uint64_t command_scratch_failures = 0;
    std::uint64_t node_failures = 0;
    // process_parallel() level routing: how many levels were dispatched across
    // the worker pool vs run serially on the audio thread (single-node levels,
    // AudioOutput-accumulation levels, or levels below the cost threshold).
    std::uint64_t parallel_levels_dispatched = 0;
    std::uint64_t serial_levels_run = 0;
};

struct GraphRuntimeExecutorResult {
    GraphRuntimeExecutorErrorCode error = GraphRuntimeExecutorErrorCode::None;
    std::uint32_t nodes_processed = 0;
    std::uint32_t commands_drained = 0;
    std::uint32_t commands_accepted = 0;
    std::uint32_t commands_rejected = 0;
    std::uint32_t events_dropped = 0;
    std::uint32_t failed_node_index = 0;

    bool ok() const noexcept {
        return error == GraphRuntimeExecutorErrorCode::None;
    }
};

/// Pre-allocated scratch buffers backing the routing process() path.
///
/// One slot is a mono buffer of `max_frames` floats — the same "slot" the
/// snapshot's GraphRuntimeBufferAssignment indexes. The caller sizes the pool
/// off-RT from a snapshot's buffer_slot_count() and the maximum block size, then
/// reuses it across realtime blocks. The executor never resizes it.
///
/// Scope: this is the SHARED inter-node routing scratch for one serial walk.
/// Per-worker scratch for a parallel executor is a separate concern and must
/// not be overloaded onto this shared pool.
class GraphRuntimeBufferPool {
public:
    // Off-RT: (re)allocate storage for `slot_count` mono slots of `max_frames`.
    // Zero-fills, so the first block (and any feedback edge) reads silence.
    // Returns false on allocation failure or zero max_frames with slots.
    bool reset(std::uint32_t slot_count, std::uint32_t max_frames);

    // Off-RT: as above, plus per-connection plug-in-delay-compensation rings.
    // `connection_delay_samples` is the snapshot assignment's per-connection
    // delay (0 = no ring). Each delayed connection gets a contiguous ring of
    // (delay + max_frames) floats and a persisted write position, zero-filled so
    // the leading delay reads silence. The 2-arg reset is equivalent to passing
    // an all-zero / empty span. Returns false on allocation failure.
    bool reset(std::uint32_t slot_count, std::uint32_t max_frames,
               std::span<const std::uint32_t> connection_delay_samples);
    void clear() noexcept;

    std::uint32_t slot_count() const noexcept { return slot_count_; }
    std::uint32_t max_frames() const noexcept { return max_frames_; }

    // RT-safe: mono buffer for slot `index`, or nullptr if out of range.
    float* slot_data(std::uint32_t index) noexcept {
        if (index >= slot_count_) return nullptr;
        return storage_.data() + static_cast<std::size_t>(index) * max_frames_;
    }
    const float* slot_data(std::uint32_t index) const noexcept {
        if (index >= slot_count_) return nullptr;
        return storage_.data() + static_cast<std::size_t>(index) * max_frames_;
    }

    // A per-connection plug-in-delay-compensation ring: a contiguous float
    // buffer of `size` (= delay + max_frames) with a persisted write position the
    // executor advances by the block's frame count. `data == nullptr` when the
    // connection needs no delay.
    struct DelayRing {
        float* data = nullptr;
        std::uint32_t size = 0;
        std::uint32_t delay = 0;
        int* write_pos = nullptr;
    };

    // RT-safe: the delay ring for connection `index`, or an empty ring (data ==
    // nullptr) when the connection has no delay or the pool carries no rings.
    DelayRing delay_ring(std::uint32_t index) noexcept {
        if (index >= ring_.size()) return {};
        RingSlot& r = ring_[index];
        if (r.size == 0) return {};
        return DelayRing{ring_storage_.data() + r.offset, r.size, r.delay, &r.write_pos};
    }

    // True when the pool can back `snapshot`'s routing for a block of `frames`:
    // enough mono slots, a large-enough max_frames, and — if the snapshot needs
    // PDC — a delay-ring layout matching its connection count (so a pool sized
    // without rings falls back to the legacy walk rather than routing without
    // delay compensation and diverging). This validates ring COUNT, not each
    // ring's size; the invariant that the rings were sized from THIS snapshot's
    // per-connection delays is held by construction — reset() the pool from the
    // same buffer_assignment that built the snapshot (both travel as one
    // CompiledGraph). Pairing a pool with a same-connection-count-but-different-
    // delays snapshot is not a supported call sequence.
    bool fits(const GraphRuntimeSnapshot& snapshot, std::uint32_t frames) const noexcept {
        if (slot_count_ < snapshot.buffer_slot_count() || frames > max_frames_) {
            return false;
        }
        const auto& assignment = snapshot.buffer_assignment();
        if (assignment.has_delay &&
            ring_.size() != snapshot.plan().connections.size()) {
            return false;
        }
        return true;
    }

private:
    // Per-connection delay-ring layout (parallel to the plan's connections; empty
    // when the pool was reset without delays). size == 0 means no ring.
    struct RingSlot {
        std::uint32_t offset = 0;  // into ring_storage_
        std::uint32_t size = 0;    // delay + max_frames; 0 = no ring
        std::uint32_t delay = 0;
        int write_pos = 0;         // persisted RT state, advanced per block
    };

    std::vector<float> storage_;
    std::uint32_t slot_count_ = 0;
    std::uint32_t max_frames_ = 0;
    std::vector<float> ring_storage_;
    std::vector<RingSlot> ring_;
};

/// Pre-allocated per-node MIDI buffers backing the routing path's event edges.
///
/// One `in`/`out` MidiBuffer pair per plan node (indexed by dense node index),
/// each with an attached UmpBuffer and reserved to a fixed realtime capacity so
/// add()/sysex append never allocate on the audio thread. The executor clears a
/// node's `in` and gathers its inbound event connections into it before the
/// node runs; a MIDI-emitting binding writes the node's `out`. A node's `out`
/// also serves as the source other nodes gather from, so it persists for the
/// block. The caller (host) bridges MidiInput/MidiOutput system nodes to its own
/// mailboxes around process_routed by writing/reading these buffers directly.
///
/// Slots are heap-stable (each pair is a unique_ptr) because a MidiBuffer holds
/// a pointer to its attached UmpBuffer; a reallocating vector would dangle it.
class GraphRuntimeMidiScratch {
public:
    // Off-RT: (re)allocate `node_count` in/out buffer pairs, each reserved to the
    // fixed realtime capacities. Returns false on allocation failure.
    bool reset(std::uint32_t node_count);
    void clear() noexcept;

    std::uint32_t node_count() const noexcept { return node_count_; }

    // RT-safe: this node's gathered MIDI input / produced MIDI output buffer, or
    // nullptr if the node index is out of range.
    midi::MidiBuffer* in(std::uint32_t node_index) noexcept {
        return node_index < node_count_ ? &slots_[node_index]->in_buffer : nullptr;
    }
    midi::MidiBuffer* out(std::uint32_t node_index) noexcept {
        return node_index < node_count_ ? &slots_[node_index]->out_buffer : nullptr;
    }

    // Per-node incompleteness flags (a buffer dropped events, or carried an
    // upstream drop). Tracked so the host egress can report the same incomplete
    // state through extract_midi() that the legacy walk does.
    bool in_incomplete(std::uint32_t node_index) const noexcept {
        return node_index < node_count_ && slots_[node_index]->in_incomplete;
    }
    void set_in_incomplete(std::uint32_t node_index, bool v) noexcept {
        if (node_index < node_count_) slots_[node_index]->in_incomplete = v;
    }
    bool out_incomplete(std::uint32_t node_index) const noexcept {
        return node_index < node_count_ && slots_[node_index]->out_incomplete;
    }
    void set_out_incomplete(std::uint32_t node_index, bool v) noexcept {
        if (node_index < node_count_) slots_[node_index]->out_incomplete = v;
    }

    bool fits(std::uint32_t node_count) const noexcept {
        return node_count_ >= node_count;
    }

private:
    struct Slot {
        midi::MidiBuffer in_buffer;
        midi::UmpBuffer in_ump;
        midi::MidiBuffer out_buffer;
        midi::UmpBuffer out_ump;
        bool in_incomplete = false;
        bool out_incomplete = false;
    };
    std::vector<std::unique_ptr<Slot>> slots_;
    std::uint32_t node_count_ = 0;
};

/// Pre-allocated scratch backing the routing path's parameter-automation edges.
///
/// Per node: one ParameterEventQueue (the automation events handed to the
/// plugin). Per connection: the persisted per-source slew state (last delivered
/// value + primed flag) a sparse automation edge ramps from across blocks. Both
/// are sized off-RT from the snapshot; the executor's automation gather fills a
/// node's queue from its inbound automation connections before the node runs.
class GraphRuntimeAutomationScratch {
public:
    // Per-node SPARSE (control-rate) automation accumulator. The gather dedups a
    // node's inbound sparse-automation edges by parameter id and accumulates two
    // control points (v0 at sample 0, vN at sample N-1) plus the parameter's
    // clamp bounds and whether any edge mixed Add. One per distinct sparse
    // parameter a node receives; held in per-node scratch (see sparse_accum) so
    // an arbitrary per-node parameter count is handled without any on-stack cap,
    // and disjoint per-node slices keep the parallel gather race-free.
    struct SparseAccum {
        std::uint32_t param_id = 0;
        float v0 = 0.0f;
        float vN = 0.0f;
        float lo = 0.0f;
        float hi = 0.0f;
        bool has_add = false;
    };

    // Off-RT: allocate per-node event queues + per-connection slew state; for each
    // node a max_frames accumulation buffer per distinct DENSE (audio-rate)
    // automation parameter plus its transient gather flags/bounds; and a per-node
    // SPARSE accumulator slice — all precomputed from the plan in first-seen
    // connection order. Returns false on allocation failure.
    bool reset(const graph::GraphRuntimePlan& plan, std::uint32_t max_frames);
    void clear() noexcept;

    std::uint32_t node_count() const noexcept { return node_count_; }
    std::uint32_t connection_count() const noexcept { return connection_count_; }
    std::uint32_t max_frames() const noexcept { return max_frames_; }

    state::ParameterEventQueue* events(std::uint32_t node_index) noexcept {
        return node_index < node_count_ ? events_[node_index].get() : nullptr;
    }
    // Persisted per-connection slew state (RT-mutable, sparse). last() is the
    // previous block's post-slew value; primed() guards the first-block snap.
    float& slew_last(std::uint32_t conn_index) noexcept { return slew_last_[conn_index]; }
    bool slew_primed(std::uint32_t conn_index) const noexcept {
        return slew_primed_[conn_index] != 0;
    }
    void set_slew_primed(std::uint32_t conn_index, bool v) noexcept {
        slew_primed_[conn_index] = v ? 1 : 0;
    }

    // Per-node DENSE (audio-rate) parameter accumulation buffers. A node receives
    // `dense_param_count(n)` distinct audio-rate parameters; dense_param_id(n,i)
    // is the i-th and dense_buffer(n,i) its max_frames accumulation region.
    std::uint32_t dense_param_count(std::uint32_t node_index) const noexcept {
        return node_index < node_count_ ? node_dense_count_[node_index] : 0;
    }
    std::uint32_t dense_param_id(std::uint32_t node_index, std::uint32_t i) const noexcept {
        return dense_params_[node_dense_first_[node_index] + i].param_id;
    }
    float* dense_buffer(std::uint32_t node_index, std::uint32_t i) noexcept {
        return dense_storage_.data() + dense_params_[node_dense_first_[node_index] + i].offset;
    }

    // Per-node DENSE transient gather state — one slot per distinct audio-rate
    // parameter the node receives (same i-indexing as dense_param_id /
    // dense_buffer). dense_replace/dense_add are the per-param "saw a Replace /
    // Add edge" flags; dense_lo/dense_hi the clamp bounds. These hold no state
    // between blocks (the gather re-zeroes its node slice each call); they live in
    // per-node scratch only so the gather needs no on-stack arrays, and disjoint
    // per-node slices keep the parallel gather race-free.
    std::span<float> dense_lo(std::uint32_t node_index) noexcept {
        return dense_span(dense_lo_, node_index);
    }
    std::span<float> dense_hi(std::uint32_t node_index) noexcept {
        return dense_span(dense_hi_, node_index);
    }
    std::span<std::uint8_t> dense_replace(std::uint32_t node_index) noexcept {
        return dense_span(dense_replace_, node_index);
    }
    std::span<std::uint8_t> dense_add(std::uint32_t node_index) noexcept {
        return dense_span(dense_add_, node_index);
    }

    // Per-node SPARSE accumulator slice (one SparseAccum per distinct control-rate
    // parameter the node receives). Disjoint per node => parallel-safe.
    std::span<SparseAccum> sparse_accum(std::uint32_t node_index) noexcept {
        if (node_index >= node_count_) return {};
        return {sparse_accum_storage_.data() + node_sparse_first_[node_index],
                node_sparse_count_[node_index]};
    }

    bool fits(std::uint32_t node_count, std::uint32_t connection_count,
              std::uint32_t frames) const noexcept {
        return node_count_ >= node_count && connection_count_ >= connection_count &&
               frames <= max_frames_;
    }

private:
    struct DenseParam {
        std::uint32_t param_id = 0;
        std::uint32_t offset = 0;  // into dense_storage_ (floats)
    };
    // Slice a per-(node,denseparam) flat vector to this node's transient dense
    // gather state — same first/count layout as dense_params_.
    template<typename T>
    std::span<T> dense_span(std::vector<T>& v, std::uint32_t node_index) noexcept {
        if (node_index >= node_count_) return {};
        return {v.data() + node_dense_first_[node_index], node_dense_count_[node_index]};
    }
    // ParameterEventQueue is large and non-copyable; hold it via unique_ptr so a
    // reallocating vector never needs to move/copy it.
    std::vector<std::unique_ptr<state::ParameterEventQueue>> events_;  // per node
    std::vector<float> slew_last_;                    // per connection
    std::vector<std::uint8_t> slew_primed_;           // per connection
    std::vector<DenseParam> dense_params_;            // flattened per-node
    std::vector<std::uint32_t> node_dense_first_;     // per node: index into dense_params_
    std::vector<std::uint32_t> node_dense_count_;     // per node
    std::vector<float> dense_storage_;                // total dense params × max_frames
    // Per-(node,denseparam) transient dense gather state (keyed by
    // node_dense_first_[n] + i, same as dense_params_): clamp bounds + the
    // Replace/Add edge flags. Sized total_dense; re-zeroed per gather.
    std::vector<float> dense_lo_;
    std::vector<float> dense_hi_;
    std::vector<std::uint8_t> dense_replace_;
    std::vector<std::uint8_t> dense_add_;
    // Per-node SPARSE accumulators, flattened (one slice per node).
    std::vector<SparseAccum> sparse_accum_storage_;
    std::vector<std::uint32_t> node_sparse_first_;    // per node: index into storage
    std::vector<std::uint32_t> node_sparse_count_;    // per node
    std::uint32_t node_count_ = 0;
    std::uint32_t connection_count_ = 0;
    std::uint32_t max_frames_ = 0;
};

class GraphRuntimeExecutor {
public:
    static constexpr std::size_t kMaxInlineCommandCapacity = 256;
    // Upper bound on ports per node for the routing path's on-stack channel
    // pointer arrays. Matches GraphRuntimeLimits::max_ports_per_node.
    static constexpr std::uint32_t kMaxRoutedPortsPerNode = 64;

    GraphRuntimeExecutor() = default;

    GraphRuntimeExecutor(const GraphRuntimeExecutor&) = delete;
    GraphRuntimeExecutor& operator=(const GraphRuntimeExecutor&) = delete;

    GraphRuntimeExecutorResult process(
        ProcessBlock& block,
        const GraphRuntimeSnapshot& snapshot,
        std::span<const graph::GraphTimedCommand> commands = {},
        std::span<GraphRuntimeCommandDecision> command_results = {},
        GraphRuntimeCommandHandler command_handler = {},
        GraphRuntimeEventSink event_sink = {}) noexcept;

    template<std::size_t CommandCapacity,
             std::size_t EventCapacity,
             std::size_t MidiOutputCapacity>
    GraphRuntimeExecutorResult process(
        ProcessBlock& block,
        const GraphRuntimeSnapshot& snapshot,
        graph::GraphRuntimeQueues<CommandCapacity, EventCapacity, MidiOutputCapacity>& queues,
        GraphRuntimeCommandHandler command_handler = {}) noexcept {
        static_assert(CommandCapacity <= kMaxInlineCommandCapacity,
                      "Use a smaller queue capacity or an explicit scratch path");
        if (!block.validate()) return fail_invalid_block();
        if (!snapshot.valid()) return fail_invalid_snapshot();

        std::array<graph::GraphTimedCommand, CommandCapacity> commands{};
        std::array<GraphRuntimeCommandDecision, CommandCapacity> command_results{};
        const auto command_count = queues.drain_commands_for_block(commands, block.frame_count);
        GraphRuntimeEventSink sink{
            &queues,
            [](void* user_data, const graph::GraphEvent& event) noexcept {
                auto* typed = static_cast<
                    graph::GraphRuntimeQueues<CommandCapacity, EventCapacity, MidiOutputCapacity>*>(
                        user_data);
                return typed->push_event_from_realtime(event);
            },
        };
        return process(block, snapshot,
                       std::span<const graph::GraphTimedCommand>(
                                 commands.data(), command_count),
                       std::span<GraphRuntimeCommandDecision>(
                           command_results.data(), command_count),
                       command_handler,
                       sink);
    }

    // Routing process() path: routes inter-node audio through `pool` per the
    // snapshot's buffer_assignment(). AudioInput nodes read the block's main
    // input bus into their output slots; AudioOutput nodes write their gathered
    // input slots to the main output bus; all other nodes have their
    // node_inputs gathered (summed) from upstream output slots and their
    // node_outputs pointed at their scratch slots before the binding runs.
    // Allocation-free: `pool` must already fit() the snapshot for this block.
    //
    // Feedback connections are honored as a one-block delay: gather adds each
    // feedback edge's previous-block slot, and after the walk the source's
    // output is captured into that slot for the next block (matching
    // host::SignalGraph's feedback_prev). The pool's zero-init gives the first
    // block silent feedback.
    // When `midi` is non-null it must fit() the snapshot's node count: the
    // executor clears each node's MIDI input, gathers its inbound event
    // connections into it, and exposes the per-node in/out buffers to bindings
    // via the process context. MidiInput/MidiOutput system nodes carry no audio
    // and no binding; the caller pre-fills a MidiInput node's `out` and drains a
    // MidiOutput node's `in` around this call. Pass null for audio-only graphs.
    GraphRuntimeExecutorResult process_routed(
        ProcessBlock& block,
        const GraphRuntimeSnapshot& snapshot,
        GraphRuntimeBufferPool& pool,
        GraphRuntimeMidiScratch* midi = nullptr,
        GraphRuntimeAutomationScratch* automation = nullptr,
        std::span<const graph::GraphTimedCommand> commands = {},
        std::span<GraphRuntimeCommandDecision> command_results = {},
        GraphRuntimeCommandHandler command_handler = {},
        GraphRuntimeEventSink event_sink = {},
        std::span<const std::uint8_t> skip_mask = {}) noexcept;

    // `skip_mask` (optional, indexed by dense node index; empty = run everything):
    // a non-zero entry means "do not run this node — its output slots are already
    // filled by the caller". Used by anticipative rendering, where a pre-rendered
    // sub-graph's boundary outputs are written into the skipped interior nodes'
    // output slots before the call so the rest of the graph reads them instead of
    // re-running the interior (which advances plugin state that the anticipation
    // producer already owns). A masked node must NOT be a feedback endpoint (source
    // or destination — the post-walk feedback capture would read its prefilled slot
    // and feed stale history) nor an AudioOutput (skipping it drops its accumulate
    // into the shared output bus, which no pool prefill can restore). The
    // anticipation interior satisfies both — 6a excludes feedback endpoints and the
    // interior never contains a live sink. Debug builds assert the contract.

    // Levelized PARALLEL routing path: same per-node work as process_routed, but
    // each topological level's independent nodes are dispatched across `workers`
    // (the audio thread is participant 0), barriered between levels. Output is
    // bit-identical to process_routed: each node owns disjoint scratch, its
    // fan-in sum is single-threaded, and the level barrier orders producers
    // before consumers. A level that contains an AudioOutput node (which
    // accumulates into the shared output bus) or has a single node runs serially.
    // `levels` must be the levelization of `snapshot`'s plan and `workers` must be
    // started; pool/midi/automation must fit() the snapshot as for process_routed.
    GraphRuntimeExecutorResult process_parallel(
        ProcessBlock& block,
        const GraphRuntimeSnapshot& snapshot,
        const graph::GraphRuntimeLevelization& levels,
        GraphRuntimeBufferPool& pool,
        GraphRuntimeWorkerPool& workers,
        GraphRuntimeMidiScratch* midi = nullptr,
        GraphRuntimeAutomationScratch* automation = nullptr,
        std::span<const graph::GraphTimedCommand> commands = {},
        std::span<GraphRuntimeCommandDecision> command_results = {},
        GraphRuntimeCommandHandler command_handler = {},
        GraphRuntimeEventSink event_sink = {}) noexcept;

    GraphRuntimeExecutorStats stats() const noexcept;
    void reset_stats() noexcept;

    // Cost threshold (off-RT setter): process_parallel dispatches a level across
    // the worker pool only when its static work-weight x frame_count reaches this
    // many "channel-samples"; below it the level runs serially to avoid losing the
    // fork/join overhead on trivial work (the break-even guard — fork/join can
    // lose at 32-64 frames). DEFAULT 0 = parallelize every eligible (width>1,
    // no-AudioOutput) level; worker-pool dispatch is the executor's job and the
    // real safety gate is the caller's opt-in (SignalGraph parallel routing is
    // default-OFF). Set a positive break-even (e.g. 4096 ~= 16 stereo nodes x 128
    // frames) to keep trivial levels serial once parallel routing is enabled on
    // small graphs.
    void set_parallel_min_work_units(std::uint64_t channel_samples) noexcept {
        parallel_min_work_units_.store(channel_samples, std::memory_order_relaxed);
    }
    std::uint64_t parallel_min_work_units() const noexcept {
        return parallel_min_work_units_.load(std::memory_order_relaxed);
    }

private:
    GraphRuntimeExecutorResult fail_invalid_block() noexcept;
    GraphRuntimeExecutorResult fail_invalid_snapshot() noexcept;
    GraphRuntimeExecutorResult fail_command_scratch_too_small() noexcept;

    // Shared command-drain used by both process() paths; fills command_results
    // and updates result + stats. Returns false if command_results is too small.
    bool drain_commands(
        ProcessBlock& block,
        const graph::GraphRuntimePlan& plan,
        std::span<const graph::GraphTimedCommand> commands,
        std::span<GraphRuntimeCommandDecision> command_results,
        GraphRuntimeCommandHandler command_handler,
        GraphRuntimeEventSink event_sink,
        GraphRuntimeExecutorResult& result) noexcept;

    std::atomic<std::uint64_t> blocks_processed_{0};
    std::atomic<std::uint64_t> nodes_processed_{0};
    std::atomic<std::uint64_t> commands_drained_{0};
    std::atomic<std::uint64_t> commands_accepted_{0};
    std::atomic<std::uint64_t> commands_rejected_{0};
    std::atomic<std::uint64_t> events_dropped_{0};
    std::atomic<std::uint64_t> invalid_blocks_{0};
    std::atomic<std::uint64_t> invalid_snapshots_{0};
    std::atomic<std::uint64_t> command_scratch_failures_{0};
    std::atomic<std::uint64_t> node_failures_{0};
    std::atomic<std::uint64_t> parallel_levels_dispatched_{0};
    std::atomic<std::uint64_t> serial_levels_run_{0};
    // 0 = no cost gate (parallelize every eligible level); see the setter.
    std::atomic<std::uint64_t> parallel_min_work_units_{0};
};

} // namespace pulp::format
