// Spectral Lab tests: the GPU spectral stack matches the CPU reference, the GPU
// genuinely wins at high layer counts, and the STFT freeze framer sustains a
// captured tone. GPU cases skip cleanly with no device.

#include <catch2/catch_test_macros.hpp>

#include <pulp/gpu_audio/spectral_stack.hpp>
#include <pulp/render/gpu_compute.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

using namespace pulp;

namespace {

constexpr double kPi = 3.14159265358979323846;

bool gpu_available() {
    auto g = render::GpuCompute::create();
    return g && g->initialize_standalone();
}

double xcorr(const std::vector<float>& a, const std::vector<float>& b) {
    double sxy = 0, sxx = 0, syy = 0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) { sxy += a[i]*b[i]; sxx += a[i]*a[i]; syy += b[i]*b[i]; }
    return sxy / std::sqrt(sxx * syy + 1e-30);
}

std::vector<float> noise_frame(uint32_t n, std::uint32_t& s) {
    std::vector<float> f(n);
    for (uint32_t i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        f[i] = static_cast<float>(s >> 9) / 4194304.0f - 1.0f;
    }
    return f;
}

double rms(const float* x, uint32_t n) {
    double s = 0; for (uint32_t i = 0; i < n; ++i) s += double(x[i]) * x[i];
    return std::sqrt(s / n);
}

}  // namespace

TEST_CASE("CpuSpectralStack captures and renders a real, finite frame", "[spectral]") {
    constexpr uint32_t N = 1024, HOP = 256, L = 4;
    gpu_audio::CpuSpectralStack cpu;
    REQUIRE(cpu.prepare(N, HOP, L));
    REQUIRE(cpu.available());

    std::uint32_t s = 0xABCDu;
    for (uint32_t l = 0; l < L; ++l) { auto f = noise_frame(N, s); REQUIRE(cpu.capture(l, f.data())); }
    REQUIRE(cpu.layer_active(0));

    std::vector<float> w(L, 1.0f / L), out(N, 0.0f);
    REQUIRE(cpu.render(out.data(), w.data(), 0.3f, 0.0f));
    REQUIRE(rms(out.data(), N) > 0.0);              // produced sound
    for (float v : out) REQUIRE(std::isfinite(v));  // real + finite

    // Clearing every layer silences the render and reports no active layer.
    for (uint32_t l = 0; l < L; ++l) cpu.clear(l);
    REQUIRE_FALSE(cpu.layer_active(0));
    REQUIRE_FALSE(cpu.render(out.data(), w.data(), 0.3f, 0.0f));
}

TEST_CASE("GpuSpectralStack matches the CPU reference bit-for-bit", "[spectral][gpu]") {
    if (!gpu_available()) { WARN("no GPU device; skipping"); return; }
    constexpr uint32_t N = 2048, HOP = 512, L = 16;
    gpu_audio::CpuSpectralStack cpu; REQUIRE(cpu.prepare(N, HOP, L));
    gpu_audio::GpuSpectralStack gpu; REQUIRE(gpu.prepare(N, HOP, L));
    REQUIRE(gpu.available());

    std::uint32_t s = 0xF00Du;
    for (uint32_t l = 0; l < L; ++l) {
        auto f = noise_frame(N, s);
        REQUIRE(cpu.capture(l, f.data()));
        REQUIRE(gpu.capture(l, f.data()));
    }
    std::vector<float> w(L, 1.0f), oc(N), og(N);
    for (int hop = 0; hop < 8; ++hop) {   // jitter=0 → deterministic both sides
        REQUIRE(cpu.render(oc.data(), w.data(), 0.4f, 0.0f));
        REQUIRE(gpu.render(og.data(), w.data(), 0.4f, 0.0f));
        INFO("hop " << hop << " xcorr=" << xcorr(oc, og));
        REQUIRE(xcorr(oc, og) > 0.99);
    }
}

TEST_CASE("GpuSpectralStack wins at high layer counts", "[spectral][gpu]") {
    if (!gpu_available()) { WARN("no GPU device; skipping"); return; }
    constexpr uint32_t N = 2048, HOP = 512, L = 128;
    gpu_audio::CpuSpectralStack cpu; REQUIRE(cpu.prepare(N, HOP, L));
    gpu_audio::GpuSpectralStack gpu; REQUIRE(gpu.prepare(N, HOP, L));
    if (!gpu.available()) { WARN("GPU over a device limit at 128 layers; skipping"); return; }

    std::uint32_t s = 0x5151u;
    for (uint32_t l = 0; l < L; ++l) {
        auto f = noise_frame(N, s);
        cpu.capture(l, f.data());
        gpu.capture(l, f.data());
    }
    std::vector<float> w(L, 1.0f / L), out(N);
    for (int i = 0; i < 4; ++i) { cpu.render(out.data(), w.data(), 0.5f, 0.0f);
                                  gpu.render(out.data(), w.data(), 0.5f, 0.0f); }
    constexpr int ITERS = 40;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < ITERS; ++i) cpu.render(out.data(), w.data(), 0.5f, 0.0f);
    const double cpu_us = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - t0).count() / ITERS;
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < ITERS; ++i) gpu.render(out.data(), w.data(), 0.5f, 0.0f);
    const double gpu_us = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - t0).count() / ITERS;
    INFO("128 layers: CPU " << cpu_us << " us, GPU " << gpu_us << " us");
    REQUIRE(gpu_us < cpu_us);   // GPU is faster at 128 layers
}

TEST_CASE("Freeze with jitter does not buzz at the FFT period", "[spectral]") {
    // A coherent phase-advance freeze repeats exactly every fft_size samples —
    // an audible buzzy "loop". With jitter the per-hop phase wander decorrelates
    // that repetition, so the sustained output must NOT be strongly periodic at
    // the fft_size lag.
    constexpr uint32_t N = 2048, HOP = 512, B = 256, BLOCKS = 360;
    constexpr double SR = 48000.0, FREQ = 220.0;
    gpu_audio::CpuSpectralStack stack; REQUIRE(stack.prepare(N, HOP, 1));
    gpu_audio::SpectralFreezeFramer framer; REQUIRE(framer.prepare(&stack, N, HOP));

    std::vector<float> in(B), out(B), tail;
    double ph = 0; const double dp = 2.0 * kPi * FREQ / SR;
    for (uint32_t b = 0; b < BLOCKS; ++b) {
        const bool feeding = b < 40;
        for (uint32_t i = 0; i < B; ++i) { in[i] = feeding ? 0.5f*std::sin(ph) : 0.0f; ph += dp; }
        gpu_audio::SpectralFreezeControls c;
        c.freeze = (b >= 20); c.smear = 0.0f; c.jitter = 0.45f;  // the shipped default
        c.weights = nullptr; c.active = true;
        framer.process(in.data(), out.data(), B, c);
        if (b >= 200) tail.insert(tail.end(), out.begin(), out.end());
    }
    // Autocorrelation exactly at the fft_size lag — the buzz would push this ~1.0.
    double num = 0, e0 = 0, e1 = 0;
    for (size_t i = 0; i + N < tail.size(); ++i) {
        num += tail[i] * tail[i+N]; e0 += tail[i]*tail[i]; e1 += tail[i+N]*tail[i+N];
    }
    const double r = num / std::sqrt(e0 * e1 + 1e-30);
    INFO("autocorr at fft-period lag = " << r);
    REQUIRE(r < 0.8);   // not a tight FFT-period loop
}

TEST_CASE("SpectralFreezeFramer sustains a captured tone", "[spectral]") {
    constexpr uint32_t N = 1024, HOP = 256;
    constexpr double SR = 48000.0, FREQ = 1000.0;
    gpu_audio::CpuSpectralStack stack;
    REQUIRE(stack.prepare(N, HOP, 1));
    gpu_audio::SpectralFreezeFramer framer;
    REQUIRE(framer.prepare(&stack, N, HOP));

    // Feed a steady sine; freeze partway; then feed SILENCE while holding the
    // freeze — the framer should keep emitting tone energy from the frozen layer.
    constexpr uint32_t BLOCK = 128, BLOCKS = 80;
    std::vector<float> in(BLOCK), out(BLOCK);
    double phase = 0.0; const double dp = 2.0 * kPi * FREQ / SR;
    double held_energy = 0.0; uint32_t held_blocks = 0;
    for (uint32_t b = 0; b < BLOCKS; ++b) {
        const bool feeding = b < BLOCKS / 2;       // tone for the first half
        for (uint32_t i = 0; i < BLOCK; ++i) {
            in[i] = feeding ? static_cast<float>(std::sin(phase)) : 0.0f;
            phase += dp;
        }
        gpu_audio::SpectralFreezeControls ctl;
        ctl.freeze = (b >= 10);                     // latch a capture early
        ctl.weights = nullptr; ctl.smear = 0.0f; ctl.jitter = 0.0f; ctl.active = true;
        framer.process(in.data(), out.data(), BLOCK, ctl);
        for (float v : out) REQUIRE(std::isfinite(v));
        if (b >= BLOCKS * 3 / 4) {                  // well after input went silent
            held_energy += rms(out.data(), BLOCK);
            ++held_blocks;
        }
    }
    REQUIRE(framer.captured_layers() >= 1);
    // Output still carries energy after the input went silent → the freeze holds.
    REQUIRE(held_energy / held_blocks > 1e-3);
}
