#pragma once

// Shared spectral-assertion helpers for GPU audio tests.
//
// GPU audio tests validate time-domain GPU output by transforming it to the
// frequency domain and checking peaks/bands against a CPU reference. These
// helpers centralize that so each test_gpu_*.cpp doesn't re-implement the FFT +
// argmax math. (GPU tests also follow the convention: construct the device,
// `if (!gpu_available()) return;` to skip cleanly on headless / no-adapter CI.)

#include <pulp/signal/fft.hpp>

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace pulp::gpu_audio_test {

// Forward FFT of a real frame → complex spectrum.
inline std::vector<std::complex<float>> spectrum_of(const std::vector<float>& x) {
    std::vector<std::complex<float>> s(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) s[i] = std::complex<float>(x[i], 0.0f);
    pulp::signal::Fft fft(static_cast<int>(x.size()));
    fft.forward(s.data());
    return s;
}

// Magnitude at a single bin.
inline float mag_at(const std::vector<float>& x, std::uint32_t k) {
    return std::abs(spectrum_of(x)[k]);
}

// Dominant bin in [1, N/2).
inline std::uint32_t peak_bin(const std::vector<float>& x) {
    const auto s = spectrum_of(x);
    std::uint32_t best = 0;
    float bm = 0.0f;
    for (std::uint32_t k = 1; k < x.size() / 2; ++k) {
        const float m = std::abs(s[k]);
        if (m > bm) { bm = m; best = k; }
    }
    return best;
}

// Peak magnitude within ±width bins of `c`.
inline float band_mag(const std::vector<float>& x, std::uint32_t c, std::uint32_t width = 3) {
    const auto s = spectrum_of(x);
    float m = 0.0f;
    const std::uint32_t lo = c > width ? c - width : 0u;
    const std::uint32_t hi = c + width;
    for (std::uint32_t k = lo; k <= hi && k < x.size(); ++k) m = std::max(m, std::abs(s[k]));
    return m;
}

}  // namespace pulp::gpu_audio_test
