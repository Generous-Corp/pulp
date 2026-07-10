#include <pulp/gpu_audio/gpu_spectral_morph.hpp>

#include <algorithm>

namespace pulp::gpu_audio {

bool GpuSpectralMorph::prepare(uint32_t fft_size, signal::WindowFunction::Type window,
                               render::GpuCompute* shared_device) {
    has_a_ = false;
    has_b_ = false;
    if (!stft_.prepare(fft_size, window, 0.0f, shared_device)) return false;
    const std::size_t cplx = static_cast<std::size_t>(fft_size) * 2u;
    a_.assign(cplx, 0.0f);
    b_.assign(cplx, 0.0f);
    mix_.assign(cplx, 0.0f);
    return true;
}

bool GpuSpectralMorph::capture_a(const float* frame_in) {
    if (!stft_.gpu_available() || frame_in == nullptr) return false;
    if (!stft_.analyze(frame_in, a_.data())) return false;
    has_a_ = true;
    return true;
}

bool GpuSpectralMorph::capture_b(const float* frame_in) {
    if (!stft_.gpu_available() || frame_in == nullptr) return false;
    if (!stft_.analyze(frame_in, b_.data())) return false;
    has_b_ = true;
    return true;
}

bool GpuSpectralMorph::render(float t, float* frame_out) {
    if (!ready() || frame_out == nullptr) return false;
    t = std::clamp(t, 0.0f, 1.0f);
    const float ta = 1.0f - t;
    for (std::size_t i = 0; i < mix_.size(); ++i) mix_[i] = ta * a_[i] + t * b_[i];
    return stft_.synthesize(mix_.data(), frame_out);
}

} // namespace pulp::gpu_audio
