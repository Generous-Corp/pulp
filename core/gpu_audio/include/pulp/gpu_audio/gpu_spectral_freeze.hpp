#pragma once

#include <cstdint>
#include <vector>

#include <pulp/gpu_audio/gpu_stft.hpp>
#include <pulp/signal/windowing.hpp>

namespace pulp::gpu_audio {

/// Phase-vocoder spectral freeze: capture one analysis frame and
/// sustain it indefinitely with a SEAMLESS, evolving loop — no loop boundary,
/// no click. capture() stores per-bin magnitude + phase; each render() advances
/// every bin's phase by its nominal frequency for one synthesis hop (with
/// optional jitter to avoid a static/metallic sound), rebuilds the complex
/// spectrum, and runs a GPU STFT synthesis. The caller overlap-adds successive
/// render() frames at the hop. Built on GpuStft; not real-time-safe (blocks on
/// readback) — for the worker / offline use. The GPU-resident foundation for
/// the hardcore multi-layer "hyper-freeze" modes.
class GpuSpectralFreeze {
public:
    /// `shared_device` (optional) reuses an existing GpuCompute so this freeze
    /// and a sibling spectral primitive don't each spin up a device; must be
    /// initialized and outlive the freeze. null = create + own a device.
    bool prepare(uint32_t fft_size, uint32_t hop,
                 signal::WindowFunction::Type window = signal::WindowFunction::Type::hann,
                 render::GpuCompute* shared_device = nullptr);

    bool gpu_available() const { return stft_.gpu_available(); }
    uint32_t fft_size() const { return stft_.fft_size(); }
    uint32_t hop() const { return hop_; }
    bool is_captured() const { return captured_; }

    /// Analyze `frame_in` (fft_size real samples) and capture its magnitude +
    /// phase as the frozen content. Returns false if not GPU-ready.
    bool capture(const float* frame_in);

    /// Render the next synthesis frame (fft_size real samples), advancing each
    /// bin's phase by its nominal per-hop increment. `phase_jitter` (0..1) adds
    /// per-hop phase wander using the shared spectral-jitter contract (see
    /// spectral_jitter.hpp): a full-turn-at-1 stateless-hash wander identical to
    /// the SpectralStack, so the knob means the same thing (and sounds the same)
    /// here as in the stack. Returns false if nothing captured or not GPU-ready.
    bool render(float* frame_out, float phase_jitter = 0.0f);

private:
    GpuStft stft_;
    uint32_t hop_ = 0;
    std::vector<float> mag_;     // per-bin magnitude (fft_size)
    std::vector<float> phase_;   // per-bin running phase (fft_size)
    std::vector<float> scratch_; // 2*fft_size interleaved-complex rebuild buffer
    std::uint32_t seed_ = 0x9E3779B9u;  // jitter seed, advanced each render
    bool captured_ = false;
};

} // namespace pulp::gpu_audio
