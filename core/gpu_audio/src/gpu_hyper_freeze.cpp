#include <pulp/gpu_audio/gpu_hyper_freeze.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::gpu_audio {

namespace {
constexpr float kTwoPi = 6.2831853071795864f;
inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
}  // namespace

bool GpuHyperFreeze::prepare(uint32_t fft_size, uint32_t hop, uint32_t num_layers,
                             signal::WindowFunction::Type window) {
    if (hop == 0u || num_layers == 0u) return false;
    if (!stft_.prepare(fft_size, window)) return false;
    hop_ = hop;
    layers_.assign(num_layers, Layer{});
    for (auto& L : layers_) {
        L.mag.assign(fft_size, 0.0f);
        L.phase.assign(fft_size, 0.0f);
        L.active = false;
    }
    spectrum_.assign(static_cast<std::size_t>(fft_size) * 2u, 0.0f);
    frame_tmp_.assign(fft_size, 0.0f);
    smeared_.assign(fft_size, 0.0f);
    return true;
}

bool GpuHyperFreeze::capture(uint32_t layer, const float* frame_in) {
    if (!stft_.gpu_available() || frame_in == nullptr || layer >= layers_.size()) return false;
    if (!stft_.analyze(frame_in, spectrum_.data())) return false;
    Layer& L = layers_[layer];
    const uint32_t n = fft_size();
    for (uint32_t k = 0; k < n; ++k) {
        const float re = spectrum_[2u * k];
        const float im = spectrum_[2u * k + 1u];
        L.mag[k] = std::sqrt(re * re + im * im);
        L.phase[k] = std::atan2(im, re);
    }
    L.active = true;
    return true;
}

bool GpuHyperFreeze::render(float* frame_out, const float* layer_weights,
                            float smear, float jitter) {
    if (!stft_.gpu_available() || frame_out == nullptr) return false;
    bool any = false;
    for (const auto& L : layers_) if (L.active) { any = true; break; }
    if (!any) return false;

    const uint32_t n = fft_size();
    for (uint32_t i = 0; i < n; ++i) frame_out[i] = 0.0f;

    const float jit = clamp01(jitter);
    const float sm = clamp01(smear);
    // Blur half-width (kernel = 2*radius+1, always odd). Any positive smear
    // gives at least radius 1, scaling up to ~N/32 — no odd-only discontinuity.
    const uint32_t radius =
        sm > 0.0f ? std::max<uint32_t>(1u, static_cast<uint32_t>(sm * (n / 32u))) : 0u;

    for (std::size_t li = 0; li < layers_.size(); ++li) {
        Layer& L = layers_[li];
        if (!L.active) continue;

        // Advance phase every render (even when muted) so a muted layer fading
        // back in stays phase-coherent. Nominal per-hop advance is conjugate-
        // symmetric → real output; wrap to keep cos/sin precise.
        const float hop_ratio = static_cast<float>(hop_) / static_cast<float>(n);
        for (uint32_t k = 0; k < n; ++k)
            L.phase[k] = std::remainder(L.phase[k] + kTwoPi * static_cast<float>(k) * hop_ratio,
                                        kTwoPi);
        if (jit > 0.0f) {
            for (uint32_t k = 1; k < n / 2u; ++k) {
                rng_ = rng_ * 1664525u + 1013904223u;
                const float u = static_cast<float>(rng_ >> 8) / 16777216.0f - 0.5f;
                const float j = jit * u;
                L.phase[k] += j;
                L.phase[n - k] -= j;  // conjugate pair
            }
        }

        const float w = layer_weights ? layer_weights[li] : 1.0f;
        if (std::abs(w) < 1e-6f) continue;  // muted: phase advanced, skip synth

        // Spectral smear: circular box-blur of magnitude. Magnitude is already
        // symmetric and circular blur preserves that exactly → output stays real.
        const float* mag = L.mag.data();
        if (radius > 0u) {
            const float inv = 1.0f / static_cast<float>(2u * radius + 1u);
            for (uint32_t k = 0; k < n; ++k) {
                float s = 0.0f;
                for (int d = -static_cast<int>(radius); d <= static_cast<int>(radius); ++d)
                    s += L.mag[(static_cast<int>(k) + d + static_cast<int>(n)) % static_cast<int>(n)];
                smeared_[k] = s * inv;
            }
            mag = smeared_.data();
        }

        for (uint32_t k = 0; k < n; ++k) {
            spectrum_[2u * k] = mag[k] * std::cos(L.phase[k]);
            spectrum_[2u * k + 1u] = mag[k] * std::sin(L.phase[k]);
        }
        if (!stft_.synthesize(spectrum_.data(), frame_tmp_.data())) return false;
        for (uint32_t i = 0; i < n; ++i) frame_out[i] += w * frame_tmp_[i];
    }
    return true;
}

} // namespace pulp::gpu_audio
