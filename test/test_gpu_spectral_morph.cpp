#include <catch2/catch_test_macros.hpp>

#include <pulp/gpu_audio/gpu_spectral_morph.hpp>
#include <pulp/signal/fft.hpp>

#include <cmath>
#include <complex>
#include <vector>

using namespace pulp::gpu_audio;

namespace {
// Magnitude of frame `x` at FFT bin `k`.
float mag_at(const std::vector<float>& x, uint32_t k) {
    std::vector<std::complex<float>> s(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) s[i] = std::complex<float>(x[i], 0.0f);
    pulp::signal::Fft fft(static_cast<int>(x.size()));
    fft.forward(s.data());
    return std::abs(s[k]);
}
uint32_t peak_bin(const std::vector<float>& x) {
    std::vector<std::complex<float>> s(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) s[i] = std::complex<float>(x[i], 0.0f);
    pulp::signal::Fft fft(static_cast<int>(x.size()));
    fft.forward(s.data());
    uint32_t best = 0; float bm = 0.0f;
    for (uint32_t k = 1; k < x.size() / 2; ++k) {
        const float m = std::abs(s[k]);
        if (m > bm) { bm = m; best = k; }
    }
    return best;
}
}  // namespace

TEST_CASE("GpuSpectralMorph blends two spectra", "[gpu_audio][spectral][gpu]") {
    constexpr uint32_t FFT = 512, KA = 15, KB = 40;
    GpuSpectralMorph m;
    REQUIRE(m.prepare(FFT));
    if (!m.gpu_available()) return;

    std::vector<float> fa(FFT), fb(FFT);
    for (uint32_t i = 0; i < FFT; ++i) {
        fa[i] = std::sin(2.0f * 3.14159265f * KA * i / FFT);
        fb[i] = std::sin(2.0f * 3.14159265f * KB * i / FFT);
    }
    REQUIRE(m.capture_a(fa.data()));
    REQUIRE(m.capture_b(fb.data()));
    REQUIRE(m.ready());

    std::vector<float> out0(FFT), out1(FFT), outm(FFT);
    REQUIRE(m.render(0.0f, out0.data()));
    REQUIRE(m.render(1.0f, out1.data()));
    REQUIRE(m.render(0.5f, outm.data()));

    // Endpoints recover each captured sound's frequency.
    REQUIRE(peak_bin(out0) == KA);
    REQUIRE(peak_bin(out1) == KB);

    // Midpoint contains BOTH frequencies (each at least a fraction of the
    // endpoint energy — a true blend, not a switch).
    const float ref = mag_at(out0, KA);  // A's energy at KA
    REQUIRE(ref > 0.0f);
    REQUIRE(mag_at(outm, KA) > 0.2f * ref);
    REQUIRE(mag_at(outm, KB) > 0.2f * ref);
}

TEST_CASE("GpuSpectralMorph render before capture fails", "[gpu_audio][spectral][gpu]") {
    GpuSpectralMorph m;
    REQUIRE(m.prepare(256));
    if (!m.gpu_available()) return;
    std::vector<float> out(256, 0.0f);
    REQUIRE_FALSE(m.render(0.5f, out.data()));  // neither endpoint captured
}
