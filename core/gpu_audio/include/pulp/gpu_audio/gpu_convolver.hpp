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
/// Correctness-first: each block runs three GpuCompute calls (FFT,
/// complex-multiply, inverse FFT) each with its own blocking readback, so it is
/// validated by golden test but is NOT yet real-time fast — the Phase-2 finding
/// (readback dominates) means the real-time win needs a GPU-resident pipeline
/// (no per-call readback), tracked as 3b-2. Until then run it offline / via
/// pump(), or rely on the CPU fallback for live use.
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

    std::vector<signal::Convolver> fallback_;  // per-channel CPU fallback
    bool prepared_ = false;
};

} // namespace pulp::gpu_audio
