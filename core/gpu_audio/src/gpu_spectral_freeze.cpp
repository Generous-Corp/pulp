#include <pulp/gpu_audio/gpu_spectral_freeze.hpp>

namespace pulp::gpu_audio {

bool GpuSpectralFreeze::prepare(uint32_t fft_size, signal::WindowFunction::Type window) {
    captured_ = false;
    if (!stft_.prepare(fft_size, window)) return false;
    spectrum_.assign(static_cast<std::size_t>(fft_size) * 2u, 0.0f);
    return true;
}

bool GpuSpectralFreeze::capture(const float* frame_in) {
    if (!stft_.gpu_available() || frame_in == nullptr) return false;
    if (!stft_.analyze(frame_in, spectrum_.data())) return false;
    captured_ = true;
    return true;
}

bool GpuSpectralFreeze::render(float* frame_out) {
    if (!captured_ || frame_out == nullptr) return false;
    return stft_.synthesize(spectrum_.data(), frame_out);
}

} // namespace pulp::gpu_audio
