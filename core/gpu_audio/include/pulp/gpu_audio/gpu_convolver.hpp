#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <pulp/gpu_audio/gpu_audio_node.hpp>
#include <pulp/render/gpu_compute.hpp>
#include <pulp/signal/convolver.hpp>

namespace pulp::gpu_audio {

/// First real GPU audio node: FFT-based (overlap-add) convolution of the input
/// with a fixed impulse response, computed on the GPU via render::GpuCompute on
/// the transport's non-RT worker.
///
/// Each block issues ONE `GpuCompute::convolve_batch` — forward FFT, complex
/// multiply by the resident IR spectrum, and inverse FFT for every channel,
/// fused into a single submit with a single readback. The per-call GPU
/// round-trip (~0.5 ms of map latency) is therefore paid once per block rather
/// than once per channel.
///
/// CPU fallback (MissPolicy::CpuFallback). The fallback is a zero-latency
/// `signal::PartitionedConvolver` per channel that the transport keeps
/// CONTINUOUSLY fed via prime_fallback() — one cheap partitioned block per audio
/// block, every block, not only on misses. Feeding it only on misses (the old
/// behaviour) left its overlap-add history full of gaps, so a substituted block
/// was NOT a correct continuation of the convolution (its IR tail energy from the
/// preceding audio was missing) — an audible discontinuity, not merely lower
/// precision. Because the priming path is zero-latency but the transport reports
/// `latency_blocks` of PDC for the GPU pipeline, the fallback's output is pushed
/// through a matching `latency_blocks`-deep delay so a miss substitute equals the
/// wet block the worker ring would have returned (input(t-L) convolved) — the
/// seam is sample-aligned, never a dry glitch and never a block early/late.
///
/// It still blocks on the GPU readback, which is why `process_block` runs on the
/// transport's non-RT worker and never on the audio thread. Live use at small
/// block sizes should still prefer the CPU fallback: a partitioned CPU
/// convolution costs microseconds, and no amount of batching beats a map
/// round-trip for a single stereo pair.
class GpuConvolver : public GpuAudioNode {
public:
    /// Fixed worker/round-trip latency, in host blocks, reported to the host as
    /// PDC. The continuously-fed CPU fallback delays its output by this many
    /// blocks so a miss substitute lands on the exact timeline slot the GPU ring
    /// would have filled.
    static constexpr uint32_t kLatencyBlocks = 2;

    GpuConvolver(uint32_t channels, uint32_t block_size, uint32_t sample_rate,
                 std::vector<float> impulse_response);

    GpuAudioNodeDescriptor descriptor() const override;
    bool prepare() override;
    void process_block(const audio::BufferView<const float>& input,
                       audio::BufferView<float>& output, uint32_t n) override;

    /// RT-safe. Feed the continuously-fed CPU fallback one block so its history
    /// stays current, and stage the latency-aligned substitute for a possible
    /// miss this block. Called by the transport on every block (hit or miss).
    void prime_fallback(const audio::BufferView<const float>& input,
                        uint32_t n) noexcept override {
        if (!prepared_ || n != block_ || fallback_delay_blocks_ == 0) return;
        float* slot_base = fb_delay_.data() +
                           static_cast<std::size_t>(fb_delay_idx_) * block_ * channels_;
        for (uint32_t ch = 0; ch < channels_ && ch < fallback_.size(); ++ch) {
            float* slot = slot_base + static_cast<std::size_t>(ch) * block_;
            float* due = fb_out_.data() + static_cast<std::size_t>(ch) * block_;
            // The slot currently holds conv(input(t-L)) — this block's due
            // substitute. Stash it, then overwrite with conv(input(t)).
            std::copy_n(slot, block_, due);
            if (ch >= input.num_channels()) {
                std::fill_n(slot, block_, 0.0f);
                continue;
            }
            const float* x = input.channel_ptr(ch);
            // Guard the fallback against a non-finite input block too: without it
            // a single NaN would poison the partitioned convolver's own overlap
            // state (and the delay ring) for the whole IR tail, not just one block.
            bool finite = true;
            for (uint32_t i = 0; i < n; ++i) {
                if (!std::isfinite(x[i])) { finite = false; break; }
            }
            if (!finite) {
                fallback_[ch].reset();
                std::fill_n(slot, block_, 0.0f);
            } else {
                fallback_[ch].process(x, slot, static_cast<std::size_t>(n));
            }
        }
        fb_delay_idx_ = (fb_delay_idx_ + 1) % fallback_delay_blocks_;
    }

    /// RT-safe miss substitute: emit the latency-aligned block that
    /// prime_fallback() already computed for this timeline slot.
    void process_cpu_fallback(const audio::BufferView<const float>& /*input*/,
                              audio::BufferView<float>& output,
                              uint32_t n) noexcept override {
        const uint32_t out_ch = static_cast<uint32_t>(output.num_channels());
        for (uint32_t ch = 0; ch < out_ch; ++ch)
            std::fill_n(output.channel_ptr(ch), n, 0.0f);
        if (!prepared_ || n != block_) return;
        const uint32_t ch_count = std::min<uint32_t>(channels_, out_ch);
        for (uint32_t ch = 0; ch < ch_count; ++ch) {
            const float* due = fb_out_.data() + static_cast<std::size_t>(ch) * block_;
            std::copy_n(due, n, output.channel_ptr(ch));
        }
    }

    /// True if the GPU compute device initialized (process_block uses the GPU).
    bool gpu_available() const { return gpu_ != nullptr; }
    /// The live compute backend ("Metal"/"D3D12"/"Vulkan"), or "" if CPU-only.
    std::string backend() const { return gpu_ ? gpu_->capabilities().backend : std::string(); }
    uint32_t fft_size() const { return fft_size_; }

protected:
    /// Allocate + load the CPU fallback machinery (both the RT miss fallback and
    /// the no-GPU worker fallback) and the latency-alignment delay ring. Shared
    /// by the render and no-render prepare() paths so they can never drift.
    void init_fallback() {
        fallback_.clear();
        worker_fallback_.clear();
        fallback_.resize(channels_);
        worker_fallback_.resize(channels_);
        for (uint32_t ch = 0; ch < channels_; ++ch) {
            fallback_[ch].load_ir(ir_.data(), ir_.size(), block_);
            worker_fallback_[ch].load_ir(ir_.data(), ir_.size(), block_);
        }
        fallback_delay_blocks_ = kLatencyBlocks;
        fb_delay_idx_ = 0;
        fb_delay_.assign(static_cast<std::size_t>(fallback_delay_blocks_) * block_ * channels_,
                         0.0f);
        fb_out_.assign(static_cast<std::size_t>(block_) * channels_, 0.0f);
    }

    /// No-GPU worker path: run the (continuously-fed) worker fallback for every
    /// prepared channel into `output`, zeroing anything else. Zero-latency
    /// partitioned convolution, so it matches the GPU path's algorithmic timing.
    void render_worker_fallback(const audio::BufferView<const float>& input,
                                audio::BufferView<float>& output,
                                uint32_t n) noexcept {
        const uint32_t out_ch = static_cast<uint32_t>(output.num_channels());
        for (uint32_t ch = 0; ch < out_ch; ++ch)
            std::fill_n(output.channel_ptr(ch), n, 0.0f);
        if (!prepared_) return;
        const uint32_t ch_count = std::min<uint32_t>(channels_, out_ch);
        for (uint32_t ch = 0; ch < ch_count && ch < worker_fallback_.size(); ++ch) {
            if (ch >= input.num_channels()) continue;
            worker_fallback_[ch].process(input.channel_ptr(ch), output.channel_ptr(ch),
                                         static_cast<std::size_t>(n));
        }
    }

    uint32_t channels_;
    uint32_t block_;
    uint32_t sample_rate_;
    std::vector<float> ir_;
    uint32_t fft_size_ = 0;

    std::unique_ptr<render::GpuCompute> gpu_;
    std::vector<float> ir_spec_;     // 2*fft_size interleaved IR spectrum
    std::vector<std::vector<float>> carry_;   // per-channel OLA accumulator (fft_size)

    // Per-block host scratch (allocated in prepare()). The FFT/mul/inverse
    // intermediates stay GPU-resident inside GpuCompute::convolve().
    std::vector<float> in_pad_;      // 2*fft_size interleaved complex input
    std::vector<float> time_;        // 2*fft_size inverse result (one readback)

    // Continuously-fed zero-latency CPU fallbacks.
    std::vector<signal::PartitionedConvolver> fallback_;         // RT miss fallback
    std::vector<signal::PartitionedConvolver> worker_fallback_;  // no-GPU worker path

    // Latency-alignment delay for the RT miss fallback. `fb_delay_` is a
    // `fallback_delay_blocks_`-deep circular buffer of block-sized, per-channel
    // slots; `fb_out_` holds this block's due (input(t-L)-convolved) substitute.
    uint32_t fallback_delay_blocks_ = 0;
    uint32_t fb_delay_idx_ = 0;
    std::vector<float> fb_delay_;    // [delay_blocks][channels][block]
    std::vector<float> fb_out_;      // [channels][block]

    bool prepared_ = false;
};

} // namespace pulp::gpu_audio
