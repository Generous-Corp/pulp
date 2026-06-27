#include <pulp/gpu_audio/gpu_audio_transport.hpp>

namespace pulp::gpu_audio {

bool GpuAudioTransport::prepare(GpuAudioNode* node, const Config& config) {
    release();
    if (node == nullptr) return false;

    // The descriptor is the single source of truth for channels/block/latency.
    const GpuAudioNodeDescriptor desc = node->descriptor();  // host thread — may allocate
    if (desc.input_channels == 0 || desc.output_channels == 0 ||
        desc.input_channels != desc.output_channels) {
        return false;  // 3a: symmetric channel count only
    }
    if (desc.block_size == 0 || desc.latency_blocks == 0) return false;
    // CpuFallback miss policy requires a real fallback.
    if (desc.miss_policy == MissPolicy::CpuFallback && !desc.supports_cpu_fallback) {
        return false;
    }
    // Need the primed latency plus one in-flight block and one free slot.
    if (config.ring_blocks < desc.latency_blocks + 2) return false;

    node_ = node;
    channels_ = desc.output_channels;
    block_size_ = desc.block_size;
    latency_blocks_ = desc.latency_blocks;
    ring_blocks_ = config.ring_blocks;
    miss_policy_ = desc.miss_policy;

    const std::uint64_t cap = static_cast<std::uint64_t>(ring_blocks_) * block_size_;
    if (!input_ring_.prepare(channels_, cap) || !output_ring_.prepare(channels_, cap)) {
        release();
        return false;
    }

    worker_in_.resize(channels_, block_size_);
    worker_out_.resize(channels_, block_size_);
    worker_in_.clear();
    worker_out_.clear();

    in_fptrs_.resize(channels_);
    in_cptrs_.resize(channels_);
    out_fptrs_.resize(channels_);
    auto iv = worker_in_.view();
    auto ov = worker_out_.view();
    for (uint32_t ch = 0; ch < channels_; ++ch) {
        in_fptrs_[ch] = iv.channel_ptr(ch);
        in_cptrs_[ch] = iv.channel_ptr(ch);
        out_fptrs_[ch] = ov.channel_ptr(ch);
    }

    // Prime the output ring with `latency_blocks` of silence (worker_out_ is
    // zeroed) to establish the fixed output delay.
    audio::BufferView<float> silence(out_fptrs_.data(), channels_, block_size_);
    for (uint32_t b = 0; b < latency_blocks_; ++b) {
        output_ring_.write(silence, block_size_);
    }

    produced_blocks_.store(0, std::memory_order_relaxed);
    miss_blocks_.store(0, std::memory_order_relaxed);
    prepared_ = true;
    return true;
}

void GpuAudioTransport::release() noexcept {
    input_ring_.release();
    output_ring_.release();
    in_fptrs_.clear();
    in_cptrs_.clear();
    out_fptrs_.clear();
    node_ = nullptr;
    channels_ = block_size_ = latency_blocks_ = ring_blocks_ = 0;
    prepared_ = false;
}

void GpuAudioTransport::process(const audio::BufferView<const float>& input,
                                audio::BufferView<float>& output, uint32_t n) noexcept {
    // Cheap RT-path validation: shape must match what the node was prepared
    // for, and the views must actually hold n frames / channels_ channels.
    // Anything off → silence (never read/write past a view).
    if (!prepared_ || n != block_size_ ||
        input.num_channels() < channels_ || output.num_channels() < channels_ ||
        input.num_samples() < n || output.num_samples() < n) {
        output.clear();
        return;
    }

    // Hand the input block to the worker. A full ring drops it (telemetry in
    // ring stats) rather than blocking.
    input_ring_.write(input, n);

    // Read the latency-delayed output produced earlier by the worker.
    if (output_ring_.read(output, n)) return;

    // Miss: the worker has not produced this block in time. Apply the policy.
    miss_blocks_.fetch_add(1, std::memory_order_relaxed);
    switch (miss_policy_) {
        case MissPolicy::Silence:
            output.clear();
            break;
        case MissPolicy::PassthroughDry:
            for (uint32_t c = 0; c < channels_; ++c) {
                const float* src = input.channel_ptr(c);
                float* dst = output.channel_ptr(c);
                for (uint32_t i = 0; i < n; ++i) dst[i] = src[i];
            }
            break;
        case MissPolicy::CpuFallback:
            node_->process_cpu_fallback(input, output, n);
            break;
    }
}

void GpuAudioTransport::pump(uint32_t max_blocks) noexcept {
    if (!prepared_) return;
    const uint32_t bs = block_size_;

    audio::BufferView<float> in_w(in_fptrs_.data(), channels_, bs);
    audio::BufferView<const float> in_c(in_cptrs_.data(), channels_, bs);
    audio::BufferView<float> out_w(out_fptrs_.data(), channels_, bs);

    uint32_t done = 0;
    while ((max_blocks == 0 || done < max_blocks) &&
           input_ring_.available_frames() >= bs &&
           output_ring_.free_frames() >= bs) {
        if (!input_ring_.read(in_w, bs)) break;
        node_->process_block(in_c, out_w, bs);
        if (output_ring_.write(out_w, bs) < bs) break;
        produced_blocks_.fetch_add(1, std::memory_order_relaxed);
        ++done;
    }
}

GpuAudioTransport::Stats GpuAudioTransport::stats() const noexcept {
    Stats s;
    s.produced_blocks = produced_blocks_.load(std::memory_order_relaxed);
    s.miss_blocks = miss_blocks_.load(std::memory_order_relaxed);
    s.input_dropped_frames = input_ring_.stats().dropped_write_frames;
    return s;
}

} // namespace pulp::gpu_audio
