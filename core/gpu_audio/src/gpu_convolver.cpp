#include <pulp/gpu_audio/gpu_convolver.hpp>

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
    // 2 blocks of worker headroom for the GPU round-trip, and CpuFallback so a
    // miss is filled by the (RT-safe) CPU convolver — seamless, never a dry
    // glitch. The node has a real CPU fallback, so this is the right default:
    // the GPU contributes when it keeps up, the CPU covers it transparently
    // otherwise, and the plugin always produces correct audio.
    d.latency_blocks = 2;
    d.miss_policy = MissPolicy::CpuFallback;
    d.supports_cpu_fallback = true;
    return d;
}

bool GpuConvolver::prepare() {
    prepared_ = false;
    if (channels_ == 0 || block_ == 0 || ir_.empty()) return false;

    // fft_size = next power of two >= block + ir_length (matches signal::Convolver).
    fft_size_ = 1;
    while (fft_size_ < block_ + static_cast<uint32_t>(ir_.size())) fft_size_ <<= 1;

    const uint32_t cplx = fft_size_ * 2u;
    in_pad_.assign(cplx, 0.0f);
    time_.assign(cplx, 0.0f);
    ir_spec_.assign(cplx, 0.0f);
    carry_.assign(channels_, std::vector<float>(fft_size_, 0.0f));

    // CPU fallback path (RT-safe after load_ir).
    fallback_.clear();
    fallback_.resize(channels_);
    for (uint32_t ch = 0; ch < channels_; ++ch) {
        fallback_[ch].load_ir(ir_.data(), static_cast<int>(ir_.size()),
                              static_cast<int>(block_));
    }

    // GPU path: build the IR spectrum once. If no GPU device, the node still
    // prepares (CPU fallback only); process_block then outputs silence.
    gpu_ = render::GpuCompute::create();
    if (gpu_ && gpu_->initialize_standalone()) {
        std::fill(in_pad_.begin(), in_pad_.end(), 0.0f);
        for (uint32_t i = 0; i < ir_.size() && i < fft_size_; ++i) {
            in_pad_[2u * i] = ir_[i];  // real; imag stays 0
        }
        if (!gpu_->fft_forward(in_pad_.data(), ir_spec_.data(), fft_size_) ||
            !gpu_->prepare_convolution(fft_size_, ir_spec_.data())) {
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
    if (!prepared_ || !gpu_ || n != block_ ||
        input.num_channels() < channels_ || output.num_channels() < channels_) {
        output.clear();
        return;
    }

    for (uint32_t ch = 0; ch < channels_; ++ch) {
        const float* x = input.channel_ptr(ch);
        float* y = output.channel_ptr(ch);

        // Zero-padded complex input.
        std::fill(in_pad_.begin(), in_pad_.end(), 0.0f);
        for (uint32_t i = 0; i < n; ++i) in_pad_[2u * i] = x[i];

        // One fused GPU-resident convolution (forward FFT → complex-mul by the
        // resident IR spectrum → inverse FFT) with a single readback. If it
        // fails, emit silence for this channel and DO NOT mutate its overlap
        // carry — feeding a stale time_ into the accumulator would poison all
        // subsequent output for the channel.
        if (!gpu_->convolve(in_pad_.data(), time_.data(), fft_size_)) {
            for (uint32_t i = 0; i < n; ++i) y[i] = 0.0f;
            continue;
        }

        // Overlap-add accumulator: add this block's full result, emit the first
        // n samples, shift the carry left by n.
        std::vector<float>& c = carry_[ch];
        for (uint32_t i = 0; i < fft_size_; ++i) c[i] += time_[2u * i];
        for (uint32_t i = 0; i < n; ++i) y[i] = c[i];
        for (uint32_t i = 0; i + n < fft_size_; ++i) c[i] = c[i + n];
        for (uint32_t i = fft_size_ - n; i < fft_size_; ++i) c[i] = 0.0f;
    }
}

void GpuConvolver::process_cpu_fallback(const audio::BufferView<const float>& input,
                                        audio::BufferView<float>& output,
                                        uint32_t n) noexcept {
    // Degraded path (note: signal::Convolver has a one-block streaming latency,
    // so it is NOT sample-aligned with the GPU path — fine as a fallback, not
    // for live A/B-switching with the GPU output). Take full-buffer ownership:
    // clear every output channel first so nothing stale leaks on extra channels
    // or an unprepared call.
    const uint32_t out_ch = static_cast<uint32_t>(output.num_channels());
    for (uint32_t ch = 0; ch < out_ch; ++ch) {
        float* y = output.channel_ptr(ch);
        for (uint32_t i = 0; i < n; ++i) y[i] = 0.0f;
    }
    if (!prepared_) return;

    const uint32_t ch_count = channels_ < out_ch ? channels_ : out_ch;
    for (uint32_t ch = 0; ch < ch_count && ch < fallback_.size(); ++ch) {
        if (ch >= input.num_channels()) continue;  // leave cleared
        fallback_[ch].process(input.channel_ptr(ch), output.channel_ptr(ch),
                              static_cast<int>(n));
    }
}

} // namespace pulp::gpu_audio
