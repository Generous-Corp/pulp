#pragma once

// GpuSpectralCloudNode — the GPU audio runtime node behind Spectral Lab's
// opt-in GPU engine.
//
// It wraps a per-channel GpuSpectralStack + SpectralFreezeFramer (one freeze
// framer and one N-layer spectral stack per channel) and runs them as a
// GpuAudioNode on the transport's non-real-time worker. process_block() reads
// the live freeze / morph / smear / jitter controls from a long-lived,
// processor-owned SpectralControlBlock (lock-free atomics) and drives each
// channel's framer for exactly one fixed block. Capture happens on a Freeze
// rising edge (round-robin into the next layer); render advances + smears +
// weighted-sums every captured layer on the GPU and reads back one frame.
//
// The GPU path blocks on the device readback, so it is NEVER run on the audio
// thread — only the transport worker calls process_block(). A missed block uses
// MissPolicy::PassthroughDry, and process_cpu_fallback() is a dry passthrough,
// so the plugin stays seamless when the worker falls behind or no device exists.

#include <pulp/audio/buffer.hpp>
#include <pulp/gpu_audio/gpu_audio_node.hpp>
#include <pulp/gpu_audio/spectral_stack.hpp>
#include <pulp/render/gpu_compute.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulp::examples {

// Spectral Lab's spectral framing constants. The fft_size / hop are shared by
// the CPU and GPU engines so the framing math is identical regardless of where
// the per-bin work runs.
inline constexpr std::uint32_t kSpectralFft = 2048;
inline constexpr std::uint32_t kSpectralHop = 512;
inline constexpr std::uint32_t kSpectralChannels = 2;

// Live per-hop controls, published lock-free from the audio thread and read on
// whatever thread drives the framer (the audio thread for the CPU engine, the
// transport worker for the GPU engine). One instance is owned by the processor
// for its whole prepared lifetime, so every engine — built and freed across
// live Engine/Layers rebuilds — reads the same stable block.
struct SpectralControlBlock {
    std::atomic<float> morph{0.5f};   // 0..1 — scrubs the loud layer across the chord
    std::atomic<float> smear{0.2f};   // 0..1 — magnitude blur across frequency
    std::atomic<float> jitter{0.45f}; // 0..1 — per-hop phase wander; high enough to
                                      // break the FFT-period buzz (smooth freeze)
    std::atomic<int> freeze{0};       // capture-trigger level (rising edge captures)
};

// Fill `w` (length num_layers) with a normalized gaussian weighting centered at
// morph across the captured layers, so morph smoothly scrubs which frozen
// "moment" is loud. Layers past the captured count stay zero (they are inactive
// anyway). Normalizing to unit sum keeps the output level steady while scrubbing.
inline void spectral_morph_weights(float* w, int num_layers, std::uint32_t captured,
                                   float morph) {
    for (int i = 0; i < num_layers; ++i) w[i] = 0.0f;
    if (captured == 0) return;
    const int cap = static_cast<int>(captured) < num_layers
                        ? static_cast<int>(captured)
                        : num_layers;
    const float center = morph * static_cast<float>(cap - 1);
    const float sigma = std::max(0.75f, static_cast<float>(cap) * 0.18f);
    float sum = 0.0f;
    for (int i = 0; i < cap; ++i) {
        const float d = (static_cast<float>(i) - center) / sigma;
        const float g = std::exp(-0.5f * d * d);
        w[i] = g;
        sum += g;
    }
    if (sum > 1e-6f)
        for (int i = 0; i < cap; ++i) w[i] /= sum;
}

class GpuSpectralCloudNode : public gpu_audio::GpuAudioNode {
public:
    GpuSpectralCloudNode(std::uint32_t channels, std::uint32_t block_size,
                         std::uint32_t sample_rate, int num_layers,
                         const SpectralControlBlock* controls)
        : channels_(channels),
          block_size_(block_size),
          sample_rate_(sample_rate),
          num_layers_(num_layers > 0 ? num_layers : 1),
          controls_(controls) {}

    gpu_audio::GpuAudioNodeDescriptor descriptor() const override {
        gpu_audio::GpuAudioNodeDescriptor d;
        d.name = "SpectralCloud";
        d.input_channels = channels_;
        d.output_channels = channels_;
        d.block_size = block_size_;
        d.sample_rate = sample_rate_;
        d.latency_blocks = 1;
        d.miss_policy = gpu_audio::MissPolicy::PassthroughDry;
        d.supports_cpu_fallback = true;
        return d;
    }

    // Non-RT: spin up one shared GPU device for the channel stacks, prepare the
    // per-channel stacks + framers. Returns false (→ CPU fallback) with no device.
    bool prepare() override {
        weights_.assign(static_cast<std::size_t>(num_layers_), 0.0f);
        device_ = render::GpuCompute::create();
        if (!device_ || !device_->initialize_standalone()) {
            device_.reset();
            return false;
        }
        for (std::uint32_t ch = 0; ch < channels_; ++ch) {
            if (!stack_[ch].prepare(kSpectralFft, kSpectralHop,
                                    static_cast<std::uint32_t>(num_layers_), device_.get()))
                return false;
            if (!framer_[ch].prepare(&stack_[ch], kSpectralFft, kSpectralHop))
                return false;
        }
        return true;
    }

    bool gpu_available() const {
        return channels_ > 0 && stack_[0].available();
    }
    std::string backend() const {
        return channels_ > 0 ? stack_[0].backend() : std::string();
    }
    std::uint32_t captured_layers() const {
        return channels_ > 0 ? framer_[0].captured_layers() : 0u;
    }

    // Worker context. Drive one fixed block per channel through its freeze framer,
    // reading the live controls and the morph weighting derived from the captured
    // layer count.
    void process_block(const audio::BufferView<const float>& input,
                       audio::BufferView<float>& output, std::uint32_t n) override {
        const float morph = controls_->morph.load(std::memory_order_relaxed);
        const float smear = controls_->smear.load(std::memory_order_relaxed);
        const float jitter = controls_->jitter.load(std::memory_order_relaxed);
        const bool freeze = controls_->freeze.load(std::memory_order_relaxed) != 0;
        const std::uint32_t ch_count =
            output.num_channels() < channels_ ? static_cast<std::uint32_t>(output.num_channels())
                                              : channels_;
        for (std::uint32_t ch = 0; ch < ch_count; ++ch) {
            spectral_morph_weights(weights_.data(), num_layers_,
                                   framer_[ch].captured_layers(), morph);
            gpu_audio::SpectralFreezeControls ctl;
            ctl.weights = weights_.data();
            ctl.smear = smear;
            ctl.jitter = jitter;
            ctl.freeze = freeze;
            ctl.active = true;
            const float* in = ch < input.num_channels() ? input.channel_ptr(ch) : nullptr;
            framer_[ch].process(in, output.channel_ptr(ch), n, ctl);
        }
    }

    // process_cpu_fallback() is left as the base-class dry passthrough — exactly
    // the PassthroughDry behavior we want for a missed block.

private:
    std::uint32_t channels_ = 0;
    std::uint32_t block_size_ = 0;
    std::uint32_t sample_rate_ = 0;
    int num_layers_ = 1;
    const SpectralControlBlock* controls_ = nullptr;

    std::unique_ptr<render::GpuCompute> device_;  // shared across the channel stacks
    std::array<gpu_audio::GpuSpectralStack, kSpectralChannels> stack_{};
    std::array<gpu_audio::SpectralFreezeFramer, kSpectralChannels> framer_{};
    std::vector<float> weights_;  // num_layers scratch, rebuilt per block from morph
};

} // namespace pulp::examples
