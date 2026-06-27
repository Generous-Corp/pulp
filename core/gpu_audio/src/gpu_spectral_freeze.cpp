#include <pulp/gpu_audio/gpu_spectral_freeze.hpp>

#include <cmath>

namespace pulp::gpu_audio {

namespace {
constexpr float kTwoPi = 6.2831853071795864f;
}  // namespace

bool GpuSpectralFreeze::prepare(uint32_t fft_size, uint32_t hop,
                                signal::WindowFunction::Type window) {
    captured_ = false;
    if (hop == 0u) return false;
    if (!stft_.prepare(fft_size, window)) return false;
    hop_ = hop;
    mag_.assign(fft_size, 0.0f);
    phase_.assign(fft_size, 0.0f);
    scratch_.assign(static_cast<std::size_t>(fft_size) * 2u, 0.0f);
    return true;
}

bool GpuSpectralFreeze::capture(const float* frame_in) {
    if (!stft_.gpu_available() || frame_in == nullptr) return false;
    if (!stft_.analyze(frame_in, scratch_.data())) return false;
    const uint32_t n = fft_size();
    for (uint32_t k = 0; k < n; ++k) {
        const float re = scratch_[2u * k];
        const float im = scratch_[2u * k + 1u];
        mag_[k] = std::sqrt(re * re + im * im);
        phase_[k] = std::atan2(im, re);
    }
    captured_ = true;
    return true;
}

bool GpuSpectralFreeze::render(float* frame_out, float phase_jitter) {
    if (!captured_ || frame_out == nullptr) return false;
    const uint32_t n = fft_size();
    const float hop_ratio = static_cast<float>(hop_) / static_cast<float>(n);

    // Nominal per-hop phase advance keeps a steady tone phase-coherent across
    // hops (seamless, no loop boundary). Bins k and n-k advance oppositely mod
    // 2π, so conjugate symmetry — and hence a real output — is preserved. Wrap
    // to keep cos/sin precise over long freezes.
    for (uint32_t k = 0; k < n; ++k) {
        phase_[k] = std::remainder(phase_[k] + kTwoPi * static_cast<float>(k) * hop_ratio,
                                   kTwoPi);
    }

    // Optional phase jitter — applied CONJUGATE-SYMMETRICALLY (mirror into n-k)
    // and skipping DC (k=0) and Nyquist (k=n/2) so the rebuilt spectrum stays
    // Hermitian and the output stays real. Clamped to [0,1].
    if (phase_jitter > 0.0f) {
        const float amt = phase_jitter > 1.0f ? 1.0f : phase_jitter;
        for (uint32_t k = 1; k < n / 2u; ++k) {
            rng_ = rng_ * 1664525u + 1013904223u;  // LCG
            const float u = static_cast<float>(rng_ >> 8) / 16777216.0f - 0.5f;  // [-0.5,0.5)
            const float j = amt * u;
            phase_[k] += j;
            phase_[n - k] -= j;  // keep the conjugate pair
        }
    }

    for (uint32_t k = 0; k < n; ++k) {
        scratch_[2u * k] = mag_[k] * std::cos(phase_[k]);
        scratch_[2u * k + 1u] = mag_[k] * std::sin(phase_[k]);
    }
    return stft_.synthesize(scratch_.data(), frame_out);
}

} // namespace pulp::gpu_audio
