#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <pulp/gpu_audio/gpu_audio_node.hpp>
#include <pulp/gpu_audio/flow_pans.hpp>
#include <pulp/render/gpu_compute.hpp>

namespace pulp::gpu_audio {

/// Massive multi-IR ("multi-room") convolution: convolves one mono input
/// against MANY distinct impulse responses at once and pans the results into a
/// stereo field — the whole bank in ONE batched GPU submit per block.
///
/// This is the regime the batched GPU path exists FOR. On the CPU each room is an
/// independent convolution; doing N of them per block scales the cost linearly. On
/// the GPU the forward FFT runs once and is shared, every room's complex-multiply +
/// inverse FFT is batched, and the num_ir results are reduced to a stereo pair ON
/// the GPU — so only one stereo block is read back regardless of how many rooms
/// there are.
///
/// It is NOT a speed claim, and it must not be written as one. A spike measured on
/// 2026-06-29 (planning/2026-06-29-superconvolver-irreducible-rooms-and-multi-ir.md)
/// found a competent real-FFT CPU convolver matching or beating this path at every
/// musically plausible setting measured (<= 256 rooms); the earlier "5x GPU win" was
/// an artifact of a naive CPU baseline. What the batch buys is a different COST
/// SHAPE — one shared forward transform, one readback, N resident IR spectra — and
/// the headroom that comes with it. Where the crossover actually falls is a matter
/// for measurement, per machine, not for a comment.
///
/// Reducibility caveat: with CONSTANT per-room pans this batch is mathematically
/// collapsible. Every room convolves the same input and is summed with fixed
/// weights, so the whole bank folds to two convolutions — one combined IR per
/// stereo channel (A·x + B·x = (A+B)·x) — and a folding CPU implementation wins.
/// So a static-pan N-room batch measures raw N-way parallel-convolution
/// throughput, not a result that musically requires the GPU. The genuinely
/// irreducible regime is TIME-VARYING per-room weights (pan/level that change
/// per block): the combined IR would change every block, so folding costs
/// O(N·IR_len) per block and the CPU can no longer pre-sum. That is where the
/// batched GPU — N resident IR spectra, one flat MAC, a cheap per-block reweight
/// — structurally beats the CPU. Pass fresh pans per block to drive it.
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
    /// These are the STATIC (Flow=0) pans; with Flow>0 the live per-block pans
    /// drift around them (see set_flow).
    const std::vector<float>& pan_l() const { return pan_l_; }
    const std::vector<float>& pan_r() const { return pan_r_; }

    /// Flow: 0 = static field (bit-for-bit the pan_l()/pan_r() layout); >0 makes
    /// each room's pan drift per block on its own rate, turning the reverb into a
    /// moving, irreducible field. Thread-safe (atomic); call from any thread —
    /// the transport worker applies it on the next block. `spread` shapes the
    /// wander depth. Both clamped to [0,1].
    void set_flow(float depth, float spread = 1.0f) noexcept {
        flow_depth_.store(depth < 0.f ? 0.f : depth > 1.f ? 1.f : depth,
                          std::memory_order_relaxed);
        flow_spread_.store(spread < 0.f ? 0.f : spread > 1.f ? 1.f : spread,
                           std::memory_order_relaxed);
    }

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

    // Flow (time-varying pans). flow_depth_/flow_spread_ are set from any thread
    // (atomic); base_theta_ + pan_norm_ are the prepared static layout the drift
    // orbits; block_counter_ advances on the transport worker (owns the phase).
    std::atomic<float> flow_depth_{0.0f};
    std::atomic<float> flow_spread_{1.0f};
    std::vector<float> base_theta_;   // per-room base azimuth, from prepare()
    std::vector<float> pan_l_live_;   // per-block drifting pans (Flow>0 only)
    std::vector<float> pan_r_live_;
    float pan_norm_ = 1.0f;           // 1/sqrt(num_ir)
    std::uint64_t block_counter_ = 0;

    bool prepared_ = false;
};

} // namespace pulp::gpu_audio
