#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <pulp/gpu_audio/gpu_audio_node.hpp>
#include <pulp/render/gpu_compute.hpp>
#include <pulp/signal/fft.hpp>

namespace pulp::gpu_audio {

/// First real GPU audio node: FFT-based (overlap-add) convolution of the input
/// with a fixed impulse response, computed on the GPU via render::GpuCompute on
/// the transport's non-RT worker. CPU fallback uses signal::Convolver.
///
/// Each block issues ONE `GpuCompute::convolve_batch` — forward FFT, complex
/// multiply by the resident IR spectrum, and inverse FFT for every channel,
/// fused into a single submit with a single readback. The per-call GPU
/// round-trip (~0.5 ms of map latency) is therefore paid once per block rather
/// than once per channel.
///
/// It still blocks on that readback, which is why `process_block` runs on the
/// transport's non-RT worker and never on the audio thread. Live use at small
/// block sizes should still prefer the CPU fallback: a partitioned CPU
/// convolution costs microseconds, and no amount of batching beats a map
/// round-trip for a single stereo pair.
class GpuConvolver : public GpuAudioNode {
public:
    GpuConvolver(uint32_t channels, uint32_t block_size, uint32_t sample_rate,
                 std::vector<float> impulse_response);

    GpuAudioNodeDescriptor descriptor() const override;
    bool prepare() override;
    void process_block(const audio::BufferView<const float>& input,
                       audio::BufferView<float>& output, uint32_t n) override;
    void process_cpu_fallback(const audio::BufferView<const float>& input,
                              audio::BufferView<float>& output,
                              uint32_t n) noexcept override;

    /// True if the GPU compute device initialized (process_block uses the GPU).
    bool gpu_available() const { return gpu_ != nullptr; }
    /// The live compute backend ("Metal"/"D3D12"/"Vulkan"), or "" if CPU-only.
    std::string backend() const { return gpu_ ? gpu_->capabilities().backend : std::string(); }
    uint32_t fft_size() const { return fft_size_; }

private:
    void process_cpu_fallback_with(std::vector<signal::Convolver>& fallback,
                                   const audio::BufferView<const float>& input,
                                   audio::BufferView<float>& output,
                                   uint32_t n) noexcept;

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

    std::vector<signal::Convolver> fallback_;         // realtime miss fallback
    std::vector<signal::Convolver> worker_fallback_;  // non-RT no-GPU worker path
    bool prepared_ = false;
};

} // namespace pulp::gpu_audio
