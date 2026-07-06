#pragma once

#include <cstdint>
#include <vector>

#include <pulp/gpu_audio/gpu_stft.hpp>
#include <pulp/signal/windowing.hpp>

namespace pulp::gpu_audio {

/// Hyper-Freeze (Phase 5, hardcore): a stack of N independently-captured
/// phase-vocoder spectral freezes summed into one output, with a spectral
/// "smear" (magnitude blur across frequency) for mangled/blurred textures.
/// Freeze a chord of moments and sustain them all at once; smear softens each
/// into a wash — things a single CPU freeze can't stack. Each layer advances
/// every bin's phase by its nominal per-hop frequency (seamless, bin-locked);
/// jitter and smear stay conjugate-symmetric so the output is real. Built on
/// GpuStft; not real-time-safe (blocks on readback) — for the worker / offline.
class GpuHyperFreeze {
public:
    bool prepare(uint32_t fft_size, uint32_t hop, uint32_t num_layers,
                 signal::WindowFunction::Type window = signal::WindowFunction::Type::hann);

    bool gpu_available() const { return stft_.gpu_available(); }
    uint32_t fft_size() const { return stft_.fft_size(); }
    uint32_t hop() const { return hop_; }
    uint32_t num_layers() const { return static_cast<uint32_t>(layers_.size()); }
    bool layer_active(uint32_t layer) const {
        return layer < layers_.size() && layers_[layer].active;
    }
    void clear(uint32_t layer) { if (layer < layers_.size()) layers_[layer].active = false; }

    /// Analyze `frame_in` (fft_size real samples) into layer `layer`, activating
    /// it as part of the frozen stack. Returns false on bad args / not ready.
    bool capture(uint32_t layer, const float* frame_in);

    /// Render the weighted stack into `frame_out` (fft_size real samples).
    /// `layer_weights` (length num_layers, or null = all 1) scales each layer —
    /// drive it from an XY pad / timeline to MORPH through the frozen stack;
    /// muted layers keep advancing phase so fading one back in stays seamless.
    /// `smear` (0..1) blurs each layer's magnitude across frequency (circular,
    /// symmetry-preserving); `jitter` (0..1) adds conjugate-symmetric phase
    /// wander. Returns false if no layer is active or not GPU-ready.
    bool render(float* frame_out, const float* layer_weights = nullptr,
                float smear = 0.0f, float jitter = 0.0f);

private:
    struct Layer {
        std::vector<float> mag;
        std::vector<float> phase;
        bool active = false;
    };

    GpuStft stft_;
    uint32_t hop_ = 0;
    std::vector<Layer> layers_;
    std::vector<float> spectrum_;   // 2*fft_size rebuild buffer
    std::vector<float> frame_tmp_;  // fft_size per-layer synthesis buffer
    std::vector<float> smeared_;    // fft_size blurred-magnitude buffer
    std::uint32_t rng_ = 0x9e3779b9u;
};

} // namespace pulp::gpu_audio
