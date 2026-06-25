#include <pulp/host/offline_signal_graph_host.hpp>

#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <cstddef>

namespace pulp::host {

OfflineSignalGraphHost::OfflineSignalGraphHost(SignalGraph& graph) : graph_(graph) {}

bool OfflineSignalGraphHost::prepare(const OfflineSignalGraphConfig& config) {
    prepared_ = false;
    config_ = config;

    if (config_.block_frames <= 0 || config_.input_channels < 0 ||
        config_.output_channels < 0) {
        return false;
    }

    const auto in_ch = static_cast<std::size_t>(config_.input_channels);
    const auto out_ch = static_cast<std::size_t>(config_.output_channels);
    const auto block = static_cast<std::size_t>(config_.block_frames);

    // Pre-size staging buffers and capture channel pointers once, so the render
    // loop allocates nothing per block.
    input_block_.resize(in_ch, block);
    output_block_.resize(out_ch, block);
    input_ptrs_.resize(in_ch);
    output_ptrs_.resize(out_ch);
    for (std::size_t c = 0; c < in_ch; ++c) input_ptrs_[c] = input_block_.channel(c).data();
    for (std::size_t c = 0; c < out_ch; ++c) output_ptrs_[c] = output_block_.channel(c).data();

    const bool graph_ok = graph_.prepare(config_.sample_rate, config_.block_frames);
    if (!graph_ok) return false;

    // SignalGraph silently zero-fills blocks larger than its prepared max block
    // size. A prepared max below the requested block size would therefore drop
    // samples, so refuse to call render() in that state.
    if (graph_.prepared_max_block_size() < config_.block_frames) return false;

    prepared_ = true;
    return true;
}

OfflineSignalGraphResult OfflineSignalGraphHost::render(
    const OfflineSignalGraphOptions& options) {
    OfflineSignalGraphResult result;
    if (!prepared_) return result;

    const auto out_ch = static_cast<std::size_t>(config_.output_channels);
    const auto in_ch = static_cast<std::size_t>(config_.input_channels);
    const auto block = static_cast<std::uint64_t>(config_.block_frames);
    const std::uint64_t total = options.frame_count;

    // One up-front allocation for the whole output; the loop below is alloc-free.
    result.audio.resize(out_ch, static_cast<std::size_t>(total));

    const std::size_t src_channels = options.input.num_channels();
    const std::size_t src_frames = options.input.num_samples();

    for (std::uint64_t pos = 0; pos < total; pos += block) {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(block, total - pos));

        // Stage this block's input: copy the available source slice, zero-pad
        // the tail (short final block) and any channels the source lacks.
        for (std::size_t c = 0; c < in_ch; ++c) {
            float* dst = input_ptrs_[c];
            std::size_t copied = 0;
            if (c < src_channels && pos < src_frames) {
                const std::size_t avail = static_cast<std::size_t>(
                    std::min<std::uint64_t>(n, src_frames - pos));
                const float* src = options.input.channel_ptr(c) + pos;
                std::copy_n(src, avail, dst);
                copied = avail;
            }
            if (copied < n) std::fill(dst + copied, dst + n, 0.0f);
        }

        audio::BufferView<const float> in_view(input_ptrs_.data(), in_ch, n);
        audio::BufferView<float> out_view(output_ptrs_.data(), out_ch, n);
        graph_.process(out_view, in_view, static_cast<int>(n));

        for (std::size_t c = 0; c < out_ch; ++c) {
            float* dst = result.audio.channel(c).data() + pos;
            std::copy_n(output_ptrs_[c], n, dst);
        }
        ++result.blocks_rendered;
    }

    result.ok = true;
    return result;
}

} // namespace pulp::host
