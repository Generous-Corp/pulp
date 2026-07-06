// Spectral Lab benchmark + correctness proof.
//
// The spectral "cloud": capture N frozen spectral layers and, every STFT hop,
// advance each layer's phase, smear its magnitude across frequency, weighted-sum
// all layers, and inverse-FFT to one frame. On the CPU that's N × (per-bin smear
// + transcendentals) done serially; on the GPU it's one batched pass across all
// bins + a single inverse FFT. This bench answers two honest questions:
//
//   1. Does the GPU engine match the CPU reference? (correctness)
//   2. At how many layers does the GPU actually win, and by how much? (value)
//
// It prints both, with the per-hop real-time budget, so the win is never assumed.

#include <pulp/gpu_audio/spectral_stack.hpp>
#include <pulp/render/gpu_compute.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using clock_t_ = std::chrono::steady_clock;

double us_since(clock_t_::time_point t0) {
    return std::chrono::duration<double, std::micro>(clock_t_::now() - t0).count();
}

// Normalized cross-correlation at zero lag.
double xcorr(const std::vector<float>& a, const std::vector<float>& b) {
    double sxy = 0, sxx = 0, syy = 0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) { sxy += a[i]*b[i]; sxx += a[i]*a[i]; syy += b[i]*b[i]; }
    return sxy / std::sqrt(sxx * syy + 1e-30);
}

std::vector<float> random_frame(uint32_t n, std::uint32_t& s) {
    std::vector<float> f(n);
    for (uint32_t i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        f[i] = static_cast<float>(s >> 9) / 4194304.0f - 1.0f;
    }
    return f;
}

}  // namespace

int main() {
    using namespace pulp;
    constexpr uint32_t N = 2048;     // FFT size
    constexpr uint32_t HOP = 512;    // STFT hop
    constexpr double SR = 48000.0;
    const double budget_us = HOP / SR * 1e6;

    std::printf("Spectral Lab — N-layer spectral cloud benchmark\n");
    if (auto probe = render::GpuCompute::create(); probe && probe->initialize_standalone()) {
        const auto c = probe->capabilities();
        std::printf("device: %s %s | fft=%u hop=%u | per-hop real-time budget: %.0f us\n",
                    c.vendor.c_str(), c.backend.c_str(), N, HOP, budget_us);
    } else {
        std::printf("no GPU compute device — cannot run the GPU columns\n");
        return 0;
    }

    // Correctness first: CPU reference vs GPU at a representative layer count.
    {
        const uint32_t L = 16;
        gpu_audio::CpuSpectralStack cpu; cpu.prepare(N, HOP, L);
        gpu_audio::GpuSpectralStack gpu; gpu.prepare(N, HOP, L);
        std::uint32_t s = 0xBEEF01u;
        for (uint32_t l = 0; l < L; ++l) {
            auto f = random_frame(N, s);
            cpu.capture(l, f.data());
            gpu.capture(l, f.data());
        }
        std::vector<float> wc(L, 1.0f), oc(N), og(N);
        double worst = 1.0;
        for (int hop = 0; hop < 8; ++hop) {            // jitter=0 → deterministic
            cpu.render(oc.data(), wc.data(), 0.4f, 0.0f);
            gpu.render(og.data(), wc.data(), 0.4f, 0.0f);
            worst = std::min(worst, xcorr(oc, og));
        }
        std::printf("\ncorrectness (16 layers, smear 0.4, 8 hops): GPU vs CPU xcorr = %.5f  %s\n",
                    worst, worst > 0.99 ? "[match]" : "[MISMATCH]");
    }

    std::printf("\n=== per-hop render cost: CPU reference vs GPU (lower us = better) ===\n");
    std::printf("layers     CPU us/hop     GPU us/hop    speedup   CPU>budget   GPU>budget\n");
    for (uint32_t L : {1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u}) {
        gpu_audio::CpuSpectralStack cpu; cpu.prepare(N, HOP, L);
        gpu_audio::GpuSpectralStack gpu;
        const bool gpu_ok = gpu.prepare(N, HOP, L) && gpu.available();
        std::uint32_t s = 0x1234u + L;
        for (uint32_t l = 0; l < L; ++l) {
            auto f = random_frame(N, s);
            cpu.capture(l, f.data());
            if (gpu_ok) gpu.capture(l, f.data());
        }
        std::vector<float> w(L, 1.0f / static_cast<float>(L)), out(N);
        constexpr int WARM = 4, ITERS = 60;
        for (int i = 0; i < WARM; ++i) {
            cpu.render(out.data(), w.data(), 0.5f, 0.0f);
            if (gpu_ok) gpu.render(out.data(), w.data(), 0.5f, 0.0f);
        }
        auto t0 = clock_t_::now();
        for (int i = 0; i < ITERS; ++i) cpu.render(out.data(), w.data(), 0.5f, 0.0f);
        const double cpu_us = us_since(t0) / ITERS;
        double gpu_us = -1.0;
        if (gpu_ok) {
            t0 = clock_t_::now();
            for (int i = 0; i < ITERS; ++i) gpu.render(out.data(), w.data(), 0.5f, 0.0f);
            gpu_us = us_since(t0) / ITERS;
        }
        if (gpu_ok)
            std::printf("%-6u %14.1f %14.1f %8.2fx %12s %12s\n", L, cpu_us, gpu_us,
                        cpu_us / gpu_us, cpu_us > budget_us ? "YES" : "no",
                        gpu_us > budget_us ? "YES" : "no");
        else
            std::printf("%-6u %14.1f %14s %8s %12s %12s\n", L, cpu_us, "(no gpu)", "-",
                        cpu_us > budget_us ? "YES" : "no", "-");
    }
    return 0;
}
