#pragma once

// Offline render host for pulp::host::SignalGraph.
//
// OfflineSignalGraphHost is a control-thread/test harness layered ON TOP of the
// public SignalGraph API — it never touches SignalGraph internals. It owns the
// staging + output buffers and steps a fixed block size across a frame range,
// feeding each block through SignalGraph::process(). Its purpose is to render a
// graph the same way a real-time host would, but deterministically and without
// a live audio device, so the same graph can be rendered at different block
// partitionings and compared.
//
// Scope note (invariant "offline equals online"): this host drives the
// no-transport SignalGraph::process() overload, so no node receives host
// transport here regardless of whether it opts in (routed nodes can consume
// transport via the transport-bearing overload, but this host never supplies
// one). The only thing that distinguishes an "online" (live, block-by-block)
// render from an "offline" one here is therefore the block partitioning. For
// DETERMINISTIC nodes whose output depends only on the current block's samples,
// rendering the same input at any block size must produce the same result.
// This host makes that comparison cheap to set up.
//
// Usage:
//   SignalGraph graph;
//   ... build + connect nodes ...
//   OfflineSignalGraphHost host(graph);
//   OfflineSignalGraphConfig cfg; cfg.block_frames = 128;
//   host.prepare(cfg);
//   OfflineSignalGraphOptions opt; opt.frame_count = 4096; opt.input = in_view;
//   auto result = host.render(opt);  // result.audio holds the rendered output

#include <pulp/audio/buffer.hpp>

#include <cstdint>
#include <vector>

namespace pulp::host {

class SignalGraph;

/// Prepare-time configuration for an offline SignalGraph render.
struct OfflineSignalGraphConfig {
    double sample_rate = 48000.0;
    int block_frames = 512;
    int input_channels = 2;
    int output_channels = 2;
};

/// Per-render inputs. `frame_count` is the total length to render; `input`
/// supplies the source audio (shorter inputs are zero-padded, longer inputs are
/// truncated to `frame_count`).
struct OfflineSignalGraphOptions {
    std::uint64_t frame_count = 0;
    audio::BufferView<const float> input;
};

/// Result of an offline render. `audio` has `output_channels` channels and
/// `frame_count` frames when `ok` is true.
struct OfflineSignalGraphResult {
    bool ok = false;
    audio::Buffer<float> audio;
    int blocks_rendered = 0;
};

/// Renders a SignalGraph offline at a fixed block size, using only the public
/// SignalGraph API. Holds the graph by reference; the caller owns the graph and
/// must keep it alive (and not re-prepare it concurrently) across render().
class OfflineSignalGraphHost {
public:
    explicit OfflineSignalGraphHost(SignalGraph& graph);

    /// Prepare the graph for `config.block_frames` blocks and pre-size the
    /// staging buffers so render() never allocates per block. Returns false if
    /// SignalGraph::prepare() fails or refuses the requested block size (the
    /// graph silently zero-fills blocks larger than its prepared max, so a
    /// prepared max below the requested block would corrupt the render).
    bool prepare(const OfflineSignalGraphConfig& config);

    bool prepared() const noexcept { return prepared_; }
    const OfflineSignalGraphConfig& config() const noexcept { return config_; }

    /// Render `options.frame_count` frames in `config().block_frames` steps. The
    /// final block is shortened to the remainder. No heap allocation occurs
    /// inside the block loop; the output buffer is allocated once up front.
    OfflineSignalGraphResult render(const OfflineSignalGraphOptions& options);

private:
    SignalGraph& graph_;
    OfflineSignalGraphConfig config_;
    bool prepared_ = false;

    audio::Buffer<float> input_block_;
    audio::Buffer<float> output_block_;
    // Channel-pointer arrays over the staging buffers, rebuilt in prepare() so
    // render() can construct BufferViews without per-block allocation. Held as
    // float* (writable) and converted to const for the input view.
    std::vector<float*> input_ptrs_;
    std::vector<float*> output_ptrs_;
};

} // namespace pulp::host
