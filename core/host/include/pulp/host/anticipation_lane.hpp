#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/planar_audio_ring_buffer.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/host/anticipation_subgraph.hpp>
#include <pulp/host/graph_types.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace pulp::host {

class PluginSlot;

// Renders an anticipation sub-graph AHEAD of the audio deadline into a ring, so
// the live graph reads the pre-computed boundary signals instead of paying the
// sub-graph's cost on the critical block. Single-producer /
// single-consumer:
//   - render_ahead() runs OFF the audio thread (a background/idle worker). It
//     advances the interior's plugin state and pushes whole blocks into the ring.
//   - consume() runs ON the audio thread. It pops one pre-rendered block, or
//     reports underrun so the caller can fall back to a synchronous render.
// The interior plugins are advanced ONLY here; a live splice that uses a lane
// must not also process those nodes, or their state double-advances.
//
// Determinism: the producer renders the sub-graph for blocks in order, so the
// block sequence the consumer pops is bit-identical to rendering the same
// sub-graph synchronously block by block.
class AnticipationLane {
public:
    AnticipationLane() = default;
    AnticipationLane(const AnticipationLane&) = delete;
    AnticipationLane& operator=(const AnticipationLane&) = delete;

    // Off-RT, and requires no concurrent render_ahead/consume (quiescent, like the
    // underlying ring). Build the executor snapshot for `sub` (resolving each
    // interior node's gain atomic / plugin slot), size the scratch pool for
    // `block_frames`, and size an output-channel ring holding `lead_blocks` blocks.
    // The lane operates on ONE fixed block size — pinning it here (before any
    // producer/consumer thread exists) keeps the producer and consumer in lockstep
    // so the consumed sequence is deterministic, and avoids a cross-thread block-
    // size field. Returns false if `sub` isn't renderable (ineligible interior, no
    // outputs) or any sizing fails. block_frames and lead_blocks must be >= 1.
    bool prepare(const AnticipationSubgraph& sub,
                 const std::function<std::atomic<float>*(NodeId)>& gain_for,
                 const std::function<PluginSlot*(NodeId)>& plugin_for,
                 double sample_rate, int block_frames, int lead_blocks,
                 const std::function<ParameterEventInjectionBinding(NodeId)>&
                     parameter_events_for = {});

    bool prepared() const noexcept { return prepared_; }
    std::size_t output_channels() const noexcept { return out_channels_; }
    int block_frames() const noexcept { return block_frames_; }
    std::uint64_t buffered_frames() const noexcept { return ring_.available_frames(); }
    std::uint64_t lead_capacity_frames() const noexcept { return ring_.capacity_frames(); }

    // PRODUCER (off the audio thread): render fixed-size blocks until the ring
    // can't hold another whole block or `max_blocks` have been rendered, whichever
    // first. Advances interior plugin state. Returns the number of blocks rendered.
    // Runs off the realtime thread (the per-block bus setup may allocate); never
    // call it from the audio callback. SINGLE PRODUCER: all render_ahead calls
    // (including any priming) must be serialized — they share unsynchronized
    // executor/pool/scratch + plugin state with each other (only the ring mediates
    // against the consumer).
    int render_ahead(int max_blocks);

    // CONSUMER (audio thread): pop one block into `out`, which must have
    // output_channels() channels and at least block_frames() samples. Returns true
    // on a hit; false on underrun (a whole block isn't buffered — the caller should
    // render synchronously this block) or a malformed `out`. Real-time-safe: no
    // allocation, no locks.
    bool consume(pulp::audio::BufferView<float>& out) noexcept;

private:
    bool render_one_block();

    format::GraphRuntimeSnapshot snapshot_;
    std::vector<PluginBindingContext> plugin_ctx_;
    PluginRoutingScratch scratch_;
    format::GraphRuntimeBufferPool pool_;
    format::GraphRuntimeExecutor exec_;
    pulp::audio::PlanarAudioRingBuffer ring_;

    // Producer-side scratch (pre-allocated): a zeroed main input bus the interior
    // ignores, and the capture buffers process_routed writes the boundary channels
    // into before they go to the ring.
    std::vector<std::vector<float>> prod_in_;
    std::vector<std::vector<float>> prod_out_;
    std::vector<const float*> prod_in_ptrs_;
    std::vector<float*> prod_out_ptrs_;

    double sample_rate_ = 0.0;
    int block_frames_ = 0;
    std::size_t out_channels_ = 0;
    bool prepared_ = false;
};

} // namespace pulp::host
