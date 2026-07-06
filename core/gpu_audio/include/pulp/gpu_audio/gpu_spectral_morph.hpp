#pragma once

#include <cstdint>
#include <vector>

#include <pulp/gpu_audio/gpu_stft.hpp>
#include <pulp/signal/windowing.hpp>

namespace pulp::gpu_audio {

/// Spectral morph (Phase 5): capture two spectra (A and B) and render a blend
/// at position t ∈ [0,1] — the signature "morph between two sounds" effect.
/// Composed from GpuStft: capture_a/capture_b run GPU STFT analysis; render(t)
/// linearly interpolates the stored complex spectra and runs a GPU STFT
/// synthesis. t=0 → A, t=1 → B. Built on the GPU spectral toolkit; not
/// real-time-safe (blocks on readback) — for the worker / offline use.
class GpuSpectralMorph {
public:
    bool prepare(uint32_t fft_size,
                 signal::WindowFunction::Type window = signal::WindowFunction::Type::hann);

    bool gpu_available() const { return stft_.gpu_available(); }
    uint32_t fft_size() const { return stft_.fft_size(); }
    bool ready() const { return has_a_ && has_b_; }

    bool capture_a(const float* frame_in);
    bool capture_b(const float* frame_in);

    /// Render the morph at position `t` (clamped to [0,1]) into `frame_out`
    /// (fft_size real samples). Returns false if both endpoints aren't captured
    /// or not GPU-ready.
    bool render(float t, float* frame_out);

private:
    GpuStft stft_;
    std::vector<float> a_, b_, mix_;  // 2*fft_size interleaved-complex spectra
    bool has_a_ = false;
    bool has_b_ = false;
};

} // namespace pulp::gpu_audio
