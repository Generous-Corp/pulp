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
    d.latency_blocks = kLatencyBlocks;
    d.miss_policy = MissPolicy::CpuFallback;
    d.supports_cpu_fallback = true;
    return d;
}

bool GpuConvolver::prepare() {
    prepared_ = false;
    if (channels_ == 0 || block_ == 0 || ir_.empty()) return false;

    // The fallback is a signal::PartitionedConvolver loaded at `block_`, and
    // load_ir() rounds a non-power-of-two block UP to the next power of two for
    // its radix-2 FFT. It would then be partitioned for a block size the
    // transport never delivers, so every block would be a block-size violation.
    // Here that is fatal rather than degrading: with no render backend the
    // fallback is the ONLY audio path, so the node would emit nothing but
    // silence. Refuse to prepare instead.
    if ((block_ & (block_ - 1u)) != 0u) return false;

    fft_size_ = 1;
    while (fft_size_ < block_ + static_cast<uint32_t>(ir_.size())) fft_size_ <<= 1;

    // Same continuously-fed fallback + latency-alignment delay ring as the render
    // build; here it is the ONLY audio path (no GPU device).
    init_fallback();

    gpu_.reset();
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
    render_worker_fallback(input, output, n);
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
