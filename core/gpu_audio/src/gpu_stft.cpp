#include <pulp/gpu_audio/gpu_stft.hpp>

namespace pulp::gpu_audio {

namespace {
bool is_power_of_two(uint32_t n) { return n != 0u && (n & (n - 1u)) == 0u; }
}  // namespace

bool GpuStft::prepare(uint32_t fft_size, signal::WindowFunction::Type window,
                      float window_param, render::GpuCompute* shared_device) {
    if (!is_power_of_two(fft_size)) return false;
    fft_size_ = fft_size;
    window_ = signal::WindowFunction::generate(static_cast<int>(fft_size), window, window_param);
    scratch_.assign(fft_size * 2u, 0.0f);

    if (shared_device) {
        // Borrow an already-initialized device — no second device spun up.
        owned_.reset();
        gpu_ = shared_device;
    } else {
        owned_ = render::GpuCompute::create();
        if (owned_ && owned_->initialize_standalone()) gpu_ = owned_.get();
        else { owned_.reset(); gpu_ = nullptr; }
    }
    return true;  // prepared even without GPU; analyze/synthesize then no-op false
}

bool GpuStft::analyze(const float* frame_in, float* spectrum_out) {
    if (!gpu_ || frame_in == nullptr || spectrum_out == nullptr) return false;
    for (uint32_t i = 0; i < fft_size_; ++i) {
        scratch_[2u * i] = frame_in[i] * window_[i];
        scratch_[2u * i + 1u] = 0.0f;
    }
    return gpu_->fft_forward(scratch_.data(), spectrum_out, fft_size_);
}

bool GpuStft::synthesize(const float* spectrum_in, float* frame_out) {
    if (!gpu_ || spectrum_in == nullptr || frame_out == nullptr) return false;
    if (!gpu_->fft_inverse(spectrum_in, scratch_.data(), fft_size_)) return false;
    for (uint32_t i = 0; i < fft_size_; ++i) frame_out[i] = scratch_[2u * i];  // real part
    return true;
}

} // namespace pulp::gpu_audio
