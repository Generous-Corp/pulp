#include <pulp/gpu_audio/gpu_convolver.hpp>
#include <pulp/gpu_audio/gpu_multi_convolver.hpp>

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
    d.latency_blocks = 2;
    d.miss_policy = MissPolicy::CpuFallback;
    d.supports_cpu_fallback = true;
    return d;
}

bool GpuConvolver::prepare() {
    prepared_ = false;
    if (channels_ == 0 || block_ == 0 || ir_.empty()) return false;

    fft_size_ = 1;
    while (fft_size_ < block_ + static_cast<uint32_t>(ir_.size())) fft_size_ <<= 1;

    fallback_.clear();
    fallback_.resize(channels_);
    for (uint32_t ch = 0; ch < channels_; ++ch) {
        fallback_[ch].load_ir(ir_.data(), static_cast<int>(ir_.size()),
                              static_cast<int>(block_));
    }

    gpu_.reset();
    prepared_ = true;
    return true;
}

void GpuConvolver::process_block(const audio::BufferView<const float>& input,
                                 audio::BufferView<float>& output, uint32_t n) {
    process_cpu_fallback(input, output, n);
}

void GpuConvolver::process_cpu_fallback(const audio::BufferView<const float>& input,
                                        audio::BufferView<float>& output,
                                        uint32_t n) noexcept {
    const uint32_t out_ch = static_cast<uint32_t>(output.num_channels());
    for (uint32_t ch = 0; ch < out_ch; ++ch) {
        float* y = output.channel_ptr(ch);
        for (uint32_t i = 0; i < n; ++i) y[i] = 0.0f;
    }
    if (!prepared_) return;

    const uint32_t ch_count = std::min(channels_, out_ch);
    for (uint32_t ch = 0; ch < ch_count && ch < fallback_.size(); ++ch) {
        if (ch >= input.num_channels()) continue;
        fallback_[ch].process(input.channel_ptr(ch), output.channel_ptr(ch),
                              static_cast<int>(n));
    }
}

GpuMultiConvolver::GpuMultiConvolver(uint32_t block_size, uint32_t sample_rate,
                                     std::vector<std::vector<float>> irs)
    : block_(block_size), sample_rate_(sample_rate), irs_(std::move(irs)),
      num_ir_(static_cast<uint32_t>(irs_.size())) {}

GpuAudioNodeDescriptor GpuMultiConvolver::descriptor() const {
    GpuAudioNodeDescriptor d;
    d.name = "gpu-multi-convolver";
    d.input_channels = 2;
    d.output_channels = 2;
    d.block_size = block_;
    d.sample_rate = sample_rate_;
    d.latency_blocks = 2;
    d.miss_policy = MissPolicy::Silence;
    d.supports_cpu_fallback = false;
    return d;
}

bool GpuMultiConvolver::prepare() {
    prepared_ = false;
    gpu_.reset();
    return false;
}

bool GpuMultiConvolver::convolve_stereo(const float*, float*, float*, uint32_t) {
    return false;
}

void GpuMultiConvolver::process_block(const audio::BufferView<const float>&,
                                      audio::BufferView<float>& output, uint32_t) {
    output.clear();
}

void GpuMultiConvolver::process_cpu_fallback(const audio::BufferView<const float>&,
                                             audio::BufferView<float>& output,
                                             uint32_t) noexcept {
    output.clear();
}

} // namespace pulp::gpu_audio
