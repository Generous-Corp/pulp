#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <pulp/gpu_audio/gpu_audio_node.hpp>
#include <pulp/render/gpu_compute.hpp>

namespace pulp::gpu_audio {

/// Massive multi-IR ("multi-room") convolution: convolves one mono input
/// against MANY distinct impulse responses at once and pans the results into a
/// stereo field — the whole bank in ONE batched GPU submit per block.
///
/// This is the regime where the GPU structurally beats the CPU. On the CPU each
/// room is an independent convolution; doing N of them per block scales the cost
/// linearly until it exceeds the real-time budget. On the GPU the forward FFT
/// runs once and is shared, every room's complex-multiply + inverse FFT is
/// batched, and the num_ir results are reduced to a stereo pair ON the GPU — so
/// only one stereo block is read back regardless of how many rooms there are.
///
/// The rooms are NOT collapsible to a single summed IR: each is panned to its
/// own stereo position (distinct L/R weights), so the two output channels are
/// genuinely different linear combinations of the rooms. That is what makes the
/// CPU baseline (N panned convolutions) a fair, irreducible comparison.
///
/// Implements GpuAudioNode so it can run RT-safe behind GpuAudioTransport (GPU
/// work on the non-RT worker; the audio thread reads a fixed-latency result).
/// The miss policy is Silence: the multi-room mode is by design beyond what the
/// CPU can sustain at scale, so there is no real-time CPU fallback to fall to —
/// a (rare) worker miss outputs a silent block rather than pretending otherwise.
class GpuMultiConvolver : public GpuAudioNode {
public:
    /// `irs` is the bank of impulse responses (each a mono IR). They may differ
    /// in length; all are zero-padded to the shared FFT size. block_size is the
    /// fixed processing block (power of two recommended).
    GpuMultiConvolver(uint32_t block_size, uint32_t sample_rate,
                      std::vector<std::vector<float>> irs);

    GpuAudioNodeDescriptor descriptor() const override;
    bool prepare() override;
    void process_block(const audio::BufferView<const float>& input,
                       audio::BufferView<float>& output, uint32_t n) override;
    void process_cpu_fallback(const audio::BufferView<const float>& input,
                              audio::BufferView<float>& output,
                              uint32_t n) noexcept override;

    /// True if the GPU compute device initialized and the multi-IR plan built.
    bool gpu_available() const { return gpu_ != nullptr; }
    /// The live compute backend ("Metal"/"D3D12"/"Vulkan"), or "" if CPU-only.
    std::string backend() const {
        return gpu_ ? gpu_->capabilities().backend : std::string();
    }
    uint32_t num_ir() const { return num_ir_; }
    uint32_t fft_size() const { return fft_size_; }

    /// Per-room constant-power pan gains (linear), available after prepare().
    /// Exposed so a reference/golden can reproduce the exact stereo combination.
    const std::vector<float>& pan_l() const { return pan_l_; }
    const std::vector<float>& pan_r() const { return pan_r_; }

    /// Convolve one mono input block against all rooms and write the panned
    /// stereo result (out_l / out_r, each `n` samples). RT-UNSAFE (GPU readback);
    /// call from the transport worker or offline. Returns false on GPU failure
    /// (output left untouched by the GPU path — caller decides the miss policy).
    bool convolve_stereo(const float* mono_in, float* out_l, float* out_r, uint32_t n);

private:
    uint32_t block_;
    uint32_t sample_rate_;
    std::vector<std::vector<float>> irs_;
    uint32_t num_ir_ = 0;
    uint32_t fft_size_ = 0;

    std::unique_ptr<render::GpuCompute> gpu_;

    // Per-room constant-power pan gains (linear), built at prepare().
    std::vector<float> pan_l_;
    std::vector<float> pan_r_;

    // Per-block host scratch (allocated in prepare()).
    std::vector<float> in_pad_;     // 2*fft_size interleaved complex input
    std::vector<float> out_lr_;     // 2*fft_size: L block then R block (readback)
    std::vector<float> carry_l_;    // overlap-add accumulator, fft_size
    std::vector<float> carry_r_;

    bool prepared_ = false;
};

} // namespace pulp::gpu_audio
