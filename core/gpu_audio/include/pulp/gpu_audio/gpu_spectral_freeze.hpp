#pragma once

#include <cstdint>
#include <vector>

#include <pulp/gpu_audio/gpu_stft.hpp>
#include <pulp/signal/windowing.hpp>

namespace pulp::gpu_audio {

/// Spectral freeze (Phase 5): capture one analysis frame's spectrum and sustain
/// it indefinitely — the canonical "frozen reverb / infinite pad" effect.
/// Composed from GpuStft: capture() runs a GPU STFT analysis and stores the
/// spectral frame; render() runs a GPU STFT synthesis of the stored frame, so
/// repeated render() calls (with overlap-add at the caller) hold the captured
/// timbre. Built on the GPU spectral toolkit; not real-time-safe (blocks on
/// readback) — for the worker / offline use.
class GpuSpectralFreeze {
public:
    bool prepare(uint32_t fft_size,
                 signal::WindowFunction::Type window = signal::WindowFunction::Type::hann);

    bool gpu_available() const { return stft_.gpu_available(); }
    uint32_t fft_size() const { return stft_.fft_size(); }
    bool is_captured() const { return captured_; }

    /// Analyze `frame_in` (fft_size real samples) and capture its spectrum as
    /// the frozen content. Returns false if not GPU-ready.
    bool capture(const float* frame_in);

    /// Synthesize the captured spectrum into `frame_out` (fft_size real
    /// samples). Deterministic — sustains the captured timbre. Returns false if
    /// nothing has been captured or not GPU-ready.
    bool render(float* frame_out);

private:
    GpuStft stft_;
    std::vector<float> spectrum_;  // 2*fft_size captured interleaved-complex frame
    bool captured_ = false;
};

} // namespace pulp::gpu_audio
