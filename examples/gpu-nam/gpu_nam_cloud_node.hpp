#pragma once

// GpuNamCloudNode — the GPU audio runtime node behind GPU NAM's opt-in GPU
// engine.
//
// It wraps one GpuNam per channel (each driving the fused GPU `nam_forward` on
// its own compute device — the NAM plan is device-resident, so channels cannot
// share a device) and runs them as a GpuAudioNode on the transport's non-real-
// time worker. process_block() runs the GPU forward for exactly one fixed block
// per channel; the mono NAM model is applied independently to each channel.
//
// The GPU forward blocks on the device readback, so it is NEVER run on the audio
// thread — only the transport worker calls process_block(). A missed block uses
// MissPolicy::CpuFallback, and process_cpu_fallback() runs the exact CPU NAM
// oracle (a per-channel copy of the same model), so the plugin stays seamless
// when the worker falls behind or no device exists.

#include "gpu_nam.hpp"
#include "nam_model.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/gpu_audio/gpu_audio_node.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pulp::examples {

inline constexpr std::uint32_t kNamChannels = 2;

class GpuNamCloudNode : public gpu_audio::GpuAudioNode {
public:
    GpuNamCloudNode(std::uint32_t channels, std::uint32_t block_size,
                    std::uint32_t sample_rate, const nam::NamModel* model)
        : channels_(channels),
          block_size_(block_size),
          sample_rate_(sample_rate),
          model_(model) {}

    gpu_audio::GpuAudioNodeDescriptor descriptor() const override {
        gpu_audio::GpuAudioNodeDescriptor d;
        d.name = "NeuralAmp";
        d.input_channels = channels_;
        d.output_channels = channels_;
        d.block_size = block_size_;
        d.sample_rate = sample_rate_;
        d.latency_blocks = 1;
        // A missed block runs the exact CPU oracle so the amp character is
        // preserved (rather than passing the un-amped dry signal).
        d.miss_policy = gpu_audio::MissPolicy::CpuFallback;
        d.supports_cpu_fallback = true;
        return d;
    }

    // Non-RT: prepare one GPU NAM forward per channel (each on its own device)
    // and a per-channel CPU copy for the fallback path. Returns false (→ the
    // transport prepare fails and the processor routes the inline CPU engine) if
    // no device is available or the model shape is unsupported on the GPU.
    bool prepare() override {
        if (!model_ || channels_ == 0 || channels_ > kNamChannels) return false;
        // Prewarm both the GPU forward's on-device history and the CPU fallback to
        // the model's silence steady-state (a NAM capture's silence response is not
        // zero, and the dilated history takes a full receptive field to populate).
        // Without this the first live block is a cold-start DC transient that
        // matches neither the reference nor the prewarmed inline CPU engine. Runs
        // off-thread in prepare().
        const int warm = 2 * model_->receptive_field() + static_cast<int>(block_size_);
        const int warm_blocks =
            (warm + static_cast<int>(block_size_) - 1) / static_cast<int>(block_size_);
        std::vector<float> zeros(block_size_, 0.0f), scratch(block_size_, 0.0f);
        for (std::uint32_t ch = 0; ch < channels_; ++ch) {
            if (!gpu_[ch].prepare(*model_, block_size_)) return false;
            for (int b = 0; b < warm_blocks; ++b)
                gpu_[ch].forward(zeros.data(), scratch.data(), block_size_);
            // Per-channel CPU oracle for the CpuFallback miss policy: same silence
            // steady-state (prewarm also pre-allocates its scratch, so the RT-safe
            // fallback never resizes on the audio thread).
            cpu_[ch] = *model_;
            cpu_[ch].prewarm();
        }
        return true;
    }

    bool gpu_available() const {
        return channels_ > 0 && gpu_[0].gpu() != nullptr;
    }
    std::string backend() const {
        if (channels_ == 0 || gpu_[0].gpu() == nullptr) return std::string();
        return gpu_[0].gpu()->capabilities().backend;
    }

    // Worker context. Run the fused GPU NAM forward for one fixed block per
    // channel (the mono model applied independently per channel).
    void process_block(const audio::BufferView<const float>& input,
                       audio::BufferView<float>& output, std::uint32_t n) override {
        const std::uint32_t ch_count =
            output.num_channels() < channels_
                ? static_cast<std::uint32_t>(output.num_channels())
                : channels_;
        for (std::uint32_t ch = 0; ch < ch_count; ++ch) {
            const float* in = ch < input.num_channels() ? input.channel_ptr(ch) : nullptr;
            float* out = output.channel_ptr(ch);
            if (in == nullptr) {
                for (std::uint32_t i = 0; i < n; ++i) out[i] = 0.0f;
                continue;
            }
            gpu_[ch].forward(in, out, n);
        }
    }

    // RT-safe: run the exact CPU NAM oracle for a missed block. The per-channel
    // CPU copy was warmed in prepare(), so process_sample does not allocate.
    void process_cpu_fallback(const audio::BufferView<const float>& input,
                              audio::BufferView<float>& output,
                              std::uint32_t n) noexcept override {
        const std::uint32_t ch_count =
            output.num_channels() < channels_
                ? static_cast<std::uint32_t>(output.num_channels())
                : channels_;
        for (std::uint32_t ch = 0; ch < ch_count; ++ch) {
            const float* in = ch < input.num_channels() ? input.channel_ptr(ch) : nullptr;
            float* out = output.channel_ptr(ch);
            if (in == nullptr) {
                for (std::uint32_t i = 0; i < n; ++i) out[i] = 0.0f;
                continue;
            }
            cpu_[ch].process(in, out, n);
        }
    }

private:
    std::uint32_t channels_ = 0;
    std::uint32_t block_size_ = 0;
    std::uint32_t sample_rate_ = 0;
    const nam::NamModel* model_ = nullptr;

    std::array<nam::GpuNam, kNamChannels> gpu_{};
    std::array<nam::NamModel, kNamChannels> cpu_{};  // CpuFallback oracle (per channel)
};

} // namespace pulp::examples
