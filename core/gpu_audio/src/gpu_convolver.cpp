#include <pulp/gpu_audio/gpu_convolver.hpp>

#include <pulp/gpu_audio/detail/gpu_ola.hpp>

#include <algorithm>
#include <utility>

namespace pulp::gpu_audio {

GpuConvolver::GpuConvolver(uint32_t channels, uint32_t block_size, uint32_t sample_rate,
                           std::vector<float> impulse_response)
    : channels_(channels), block_(block_size), sample_rate_(sample_rate),
      ir_(std::move(impulse_response)) {}

GpuAudioNodeDescriptor GpuConvolver::descriptor() const {
    GpuAudioNodeDescriptor d;
    d.name = "gpu-convolver";
    d.input_channels = channels_;
    d.output_channels = channels_;
    d.block_size = block_;
    d.sample_rate = sample_rate_;
    // kLatencyBlocks of worker headroom for the GPU round-trip, and CpuFallback so
    // a miss is filled by the continuously-fed, latency-aligned CPU convolver —
    // seamless, never a dry glitch. The node has a real CPU fallback, so this is
    // the right default: the GPU contributes when it keeps up, the CPU covers it
    // transparently otherwise, and the plugin always produces correct audio.
    d.latency_blocks = kLatencyBlocks;
    d.miss_policy = MissPolicy::CpuFallback;
    d.supports_cpu_fallback = true;
    return d;
}

bool GpuConvolver::prepare() {
    prepared_ = false;
    if (channels_ == 0 || block_ == 0 || ir_.empty()) return false;

    // The CPU fallback is a signal::PartitionedConvolver loaded at `block_`, and
    // load_ir() rounds a non-power-of-two block UP to the next power of two for
    // its radix-2 FFT. The fallback would then be partitioned for a block size
    // the transport never delivers, so every fallback block would be a block-size
    // violation. Refuse to prepare rather than run a convolver whose fallback can
    // only ever fail closed.
    if ((block_ & (block_ - 1u)) != 0u) return false;

    // fft_size = next power of two >= block + ir_length (matches signal::Convolver).
    fft_size_ = 1;
    while (fft_size_ < block_ + static_cast<uint32_t>(ir_.size())) fft_size_ <<= 1;

    const uint32_t cplx = fft_size_ * 2u;
    // Batched across channels: one submit, one readback per block. The IR is
    // mono and resident, so every channel convolves against the same spectrum.
    in_pad_.assign(static_cast<std::size_t>(cplx) * channels_, 0.0f);
    time_.assign(static_cast<std::size_t>(cplx) * channels_, 0.0f);
    ir_spec_.assign(cplx, 0.0f);
    carry_.assign(channels_, std::vector<float>(fft_size_, 0.0f));

    // CPU fallback path (RT-safe after load): the continuously-fed RT miss
    // fallback + latency-alignment delay ring, and the no-GPU worker fallback.
    init_fallback();

    // GPU path: build the IR spectrum once. If no GPU device, the node still
    // prepares and the worker path uses the CPU fallback.
    gpu_ = render::GpuCompute::create();
    if (gpu_ && gpu_->initialize_standalone()) {
        std::fill(in_pad_.begin(), in_pad_.end(), 0.0f);
        for (uint32_t i = 0; i < ir_.size() && i < fft_size_; ++i) {
            in_pad_[2u * i] = ir_[i];  // real; imag stays 0
        }
        // fft_forward reads only the first block; the rest of in_pad_ is the
        // batch scratch used per process_block.
        if (!gpu_->fft_forward(in_pad_.data(), ir_spec_.data(), fft_size_) ||
            !gpu_->prepare_convolution_batch(fft_size_, ir_spec_.data(), channels_)) {
            gpu_.reset();
        }
    } else {
        gpu_.reset();
    }

    prepared_ = true;
    return true;
}

void GpuConvolver::process_block(const audio::BufferView<const float>& input,
                                 audio::BufferView<float>& output, uint32_t n) {
    if (!prepared_ || n != block_ ||
        input.num_channels() < channels_ || output.num_channels() < channels_) {
        output.clear();
        return;
    }
    if (!gpu_) {
        render_worker_fallback(input, output, n);
        return;
    }

    const uint32_t cplx = fft_size_ * 2u;

    // Pack every channel's zero-padded complex block back to back.
    std::fill(in_pad_.begin(), in_pad_.end(), 0.0f);
    for (uint32_t ch = 0; ch < channels_; ++ch) {
        const float* x = input.channel_ptr(ch);
        float* slot = in_pad_.data() + static_cast<std::size_t>(ch) * cplx;
        for (uint32_t i = 0; i < n; ++i) slot[2u * i] = x[i];
    }

    // ONE fused, GPU-resident convolution for all channels — forward FFT,
    // complex multiply by the resident IR spectrum, inverse FFT — in a single
    // submit with a single readback. The ~0.5 ms map round trip is paid once
    // per block, not once per channel.
    //
    // On failure emit silence and DO NOT mutate any overlap carry: feeding a
    // stale time_ into the accumulators would poison every subsequent block.
    if (!gpu_->convolve_batch(in_pad_.data(), time_.data(), fft_size_, channels_)) {
        output.clear();
        return;
    }

    for (uint32_t ch = 0; ch < channels_; ++ch) {
        // Guarded overlap-add: add this block's result, emit the first n, shift
        // the carry left by n. A non-finite readback resets the carry and emits
        // silence for this block instead of poisoning the channel forever.
        detail::overlap_add_block(carry_[ch].data(),
                                  time_.data() + static_cast<std::size_t>(ch) * cplx,
                                  /*src_stride=*/2, output.channel_ptr(ch), fft_size_, n);
    }
}

} // namespace pulp::gpu_audio
