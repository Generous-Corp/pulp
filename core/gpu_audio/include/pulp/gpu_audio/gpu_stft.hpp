#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <pulp/render/gpu_compute.hpp>
#include <pulp/signal/windowing.hpp>

namespace pulp::gpu_audio {

/// GPU short-time Fourier transform primitive (Phase 4 spectral toolkit).
///
/// analyze(): apply the analysis window to one frame, then a GPU forward FFT →
/// an interleaved-complex spectral frame. synthesize(): GPU inverse FFT → the
/// real time-domain frame (no synthesis window; the caller overlap-adds). With
/// a COLA window/hop (e.g. Hann at 50% overlap), analyze→synthesize→overlap-add
/// reconstructs the input. `fft_size` must be a power of two. GPU work runs via
/// render::GpuCompute; not real-time-safe (blocks on readback) — for the worker
/// / offline analysis, the foundation for spectral freeze / morph / paint.
class GpuStft {
public:
    bool prepare(uint32_t fft_size,
                 signal::WindowFunction::Type window = signal::WindowFunction::Type::hann,
                 float window_param = 0.0f);

    bool gpu_available() const { return gpu_ != nullptr; }
    uint32_t fft_size() const { return fft_size_; }
    const std::vector<float>& window() const { return window_; }

    /// The underlying GPU compute device (or null if none initialized). Exposed
    /// so a sibling primitive (e.g. the multi-layer spectral stack) can run on
    /// the SAME device and keep spectral frames GPU-resident without a second
    /// device. Worker / offline use only — not real-time-safe.
    render::GpuCompute* compute() { return gpu_.get(); }

    /// frame_in: fft_size real samples. spectrum_out: 2*fft_size interleaved
    /// complex (windowed forward FFT). Returns false if not GPU-ready.
    bool analyze(const float* frame_in, float* spectrum_out);

    /// spectrum_in: 2*fft_size interleaved complex. frame_out: fft_size real
    /// samples (inverse FFT, no synthesis window). Returns false if not ready.
    bool synthesize(const float* spectrum_in, float* frame_out);

private:
    uint32_t fft_size_ = 0;
    std::vector<float> window_;
    std::unique_ptr<render::GpuCompute> gpu_;
    std::vector<float> scratch_;  // 2*fft_size interleaved-complex scratch
};

} // namespace pulp::gpu_audio
