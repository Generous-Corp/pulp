#include <catch2/catch_test_macros.hpp>
#include <pulp/render/gpu_compute.hpp>
#include <pulp/render/gpu_surface.hpp>
#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace pulp::render;
using namespace pulp::signal;

// ── Correctness Tests ───────────────────────────────────────────────────────

TEST_CASE("GpuCompute factory returns non-null", "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
#if defined(PULP_HAS_SKIA)
    REQUIRE(compute != nullptr);
#else
    REQUIRE(compute == nullptr);
#endif
}

TEST_CASE("GpuCompute standalone initialization", "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute) return;

    bool ok = compute->initialize_standalone();
    if (!ok) {
        // No GPU adapter (headless CI) — graceful failure
        REQUIRE_FALSE(compute->is_initialized());
        return;
    }

    REQUIRE(compute->is_initialized());
}

TEST_CASE("GpuCompute magnitude correctness", "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr uint32_t N = 1024;
    std::vector<float> complex_pairs(N * 2);
    std::vector<float> gpu_mag(N);
    std::vector<float> cpu_mag(N);

    // Fill with known values: complex(3, 4) → magnitude 5
    for (uint32_t i = 0; i < N; ++i) {
        complex_pairs[i * 2]     = 3.0f;
        complex_pairs[i * 2 + 1] = 4.0f;
    }

    REQUIRE(compute->compute_magnitude(complex_pairs.data(), gpu_mag.data(), N));

    // CPU reference
    for (uint32_t i = 0; i < N; ++i) {
        float re = complex_pairs[i * 2];
        float im = complex_pairs[i * 2 + 1];
        cpu_mag[i] = std::sqrt(re * re + im * im);
    }

    for (uint32_t i = 0; i < N; ++i) {
        REQUIRE(std::abs(gpu_mag[i] - cpu_mag[i]) < 1e-4f);
    }
}

TEST_CASE("GpuCompute complex multiply correctness", "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr uint32_t N = 512;
    std::vector<float> a(N * 2), b(N * 2), gpu_result(N * 2);

    // (2+3i) * (4+5i) = (2*4 - 3*5) + (2*5 + 3*4)i = -7 + 22i
    for (uint32_t i = 0; i < N; ++i) {
        a[i * 2] = 2.0f; a[i * 2 + 1] = 3.0f;
        b[i * 2] = 4.0f; b[i * 2 + 1] = 5.0f;
    }

    REQUIRE(compute->complex_multiply(a.data(), b.data(), gpu_result.data(), N));

    for (uint32_t i = 0; i < N; ++i) {
        REQUIRE(std::abs(gpu_result[i * 2]     - (-7.0f)) < 1e-4f);
        REQUIRE(std::abs(gpu_result[i * 2 + 1] - 22.0f)  < 1e-4f);
    }
}

TEST_CASE("GpuCompute batch magnitude", "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr uint32_t bins = 256;
    constexpr uint32_t frames = 4;
    std::vector<float> complex_data(bins * frames * 2);
    std::vector<float> magnitudes(bins * frames);

    for (uint32_t f = 0; f < frames; ++f) {
        for (uint32_t i = 0; i < bins; ++i) {
            float val = static_cast<float>(f + 1);
            complex_data[(f * bins + i) * 2]     = val;
            complex_data[(f * bins + i) * 2 + 1] = 0.0f;
        }
    }

    REQUIRE(compute->batch_magnitude(complex_data.data(), magnitudes.data(), bins, frames));

    // Frame 0: magnitude should be 1.0, frame 1: 2.0, etc.
    for (uint32_t f = 0; f < frames; ++f) {
        float expected = static_cast<float>(f + 1);
        for (uint32_t i = 0; i < bins; ++i) {
            REQUIRE(std::abs(magnitudes[f * bins + i] - expected) < 1e-4f);
        }
    }
}

TEST_CASE("GpuCompute matches CPU FFT magnitude", "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr int fft_size = 1024;
    Fft fft(fft_size);

    // Generate a 440 Hz sine wave at 44100 Hz sample rate
    std::vector<float> audio(fft_size);
    for (int i = 0; i < fft_size; ++i) {
        audio[i] = std::sin(2.0f * 3.14159265f * 440.0f * i / 44100.0f);
    }

    // CPU FFT → complex output
    std::vector<std::complex<float>> freq(fft_size);
    fft.forward_real(audio.data(), freq.data());

    // CPU magnitude
    std::vector<float> cpu_mag(fft_size);
    fft.magnitude(freq.data(), cpu_mag.data(), fft_size);

    // GPU magnitude — convert complex to interleaved pairs
    std::vector<float> complex_pairs(fft_size * 2);
    for (int i = 0; i < fft_size; ++i) {
        complex_pairs[i * 2]     = freq[i].real();
        complex_pairs[i * 2 + 1] = freq[i].imag();
    }
    std::vector<float> gpu_mag(fft_size);
    REQUIRE(compute->compute_magnitude(complex_pairs.data(), gpu_mag.data(), fft_size));

    // Compare
    for (int i = 0; i < fft_size; ++i) {
        REQUIRE(std::abs(gpu_mag[i] - cpu_mag[i]) < 1e-3f);
    }
}

// ── FFT Tests ─────────────────────────────────────────────────────────────

TEST_CASE("GpuCompute FFT forward magnitude matches CPU", "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr uint32_t N = 1024;
    Fft fft(static_cast<int>(N));

    std::vector<std::complex<float>> sig(N);
    for (uint32_t i = 0; i < N; ++i) {
        const float t = static_cast<float>(i);
        const float v = std::sin(2.0f * 3.14159265f * 5.0f * t / N)
                      + 0.5f * std::cos(2.0f * 3.14159265f * 17.0f * t / N);
        sig[i] = std::complex<float>(v, 0.0f);
    }

    std::vector<std::complex<float>> cpu = sig;  // in-place forward
    fft.forward(cpu.data());

    std::vector<float> in(N * 2), out(N * 2);
    for (uint32_t i = 0; i < N; ++i) {
        in[i * 2]     = sig[i].real();
        in[i * 2 + 1] = sig[i].imag();
    }
    REQUIRE(compute->fft_forward(in.data(), out.data(), N));

    // Compare magnitudes (independent of the library's sign convention).
    for (uint32_t i = 0; i < N; ++i) {
        const float gpu_mag = std::sqrt(out[i * 2] * out[i * 2]
                                      + out[i * 2 + 1] * out[i * 2 + 1]);
        const float cpu_mag = std::abs(cpu[i]);
        REQUIRE(std::abs(gpu_mag - cpu_mag) < 1e-2f * (1.0f + cpu_mag));
    }
}

TEST_CASE("GpuCompute FFT impulse and DC", "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr uint32_t N = 256;
    std::vector<float> in(N * 2, 0.0f), out(N * 2, 0.0f);

    // Unit impulse at n=0 → flat spectrum: every bin = (1, 0).
    in[0] = 1.0f;
    REQUIRE(compute->fft_forward(in.data(), out.data(), N));
    for (uint32_t i = 0; i < N; ++i) {
        REQUIRE(std::abs(out[i * 2]     - 1.0f) < 1e-3f);
        REQUIRE(std::abs(out[i * 2 + 1] - 0.0f) < 1e-3f);
    }

    // DC (all ones) → X[0] = (N, 0), all other bins ≈ 0.
    std::fill(in.begin(), in.end(), 0.0f);
    for (uint32_t i = 0; i < N; ++i) in[i * 2] = 1.0f;
    REQUIRE(compute->fft_forward(in.data(), out.data(), N));
    REQUIRE(std::abs(out[0] - static_cast<float>(N)) < 1e-2f);
    REQUIRE(std::abs(out[1]) < 1e-2f);
    for (uint32_t i = 1; i < N; ++i) {
        REQUIRE(std::abs(out[i * 2])     < 1e-2f);
        REQUIRE(std::abs(out[i * 2 + 1]) < 1e-2f);
    }
}

TEST_CASE("GpuCompute FFT round-trip identity", "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr uint32_t N = 512;
    std::vector<float> in(N * 2), fwd(N * 2), back(N * 2);
    for (uint32_t i = 0; i < N; ++i) {
        in[i * 2]     = std::sin(2.0f * 3.14159265f * 7.0f * i / N);
        in[i * 2 + 1] = 0.3f * std::cos(2.0f * 3.14159265f * 3.0f * i / N);
    }
    REQUIRE(compute->fft_forward(in.data(), fwd.data(), N));
    REQUIRE(compute->fft_inverse(fwd.data(), back.data(), N));
    for (uint32_t i = 0; i < N * 2; ++i) {
        REQUIRE(std::abs(back[i] - in[i]) < 2e-3f);
    }
}

TEST_CASE("GpuCompute FFT matches direct DFT (complex, phase-exact)",
          "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr double kPi = 3.14159265358979323846;
    constexpr uint32_t N = 8;

    // Complex input with nonzero real AND imaginary parts so the test would
    // catch a conjugation, a wrong bin permutation, or a sign flip — things
    // the magnitude/impulse/round-trip tests cannot see.
    std::vector<std::complex<float>> x(N);
    for (uint32_t n = 0; n < N; ++n) {
        x[n] = std::complex<float>(0.5f + 0.1f * n, -0.2f + 0.3f * n);
    }

    // Direct DFT with the documented forward convention:
    //   X[k] = sum_n x[n] * exp(-2*pi*i*k*n/N)
    std::vector<std::complex<float>> ref(N);
    for (uint32_t k = 0; k < N; ++k) {
        std::complex<double> acc(0.0, 0.0);
        for (uint32_t n = 0; n < N; ++n) {
            const double ang = -2.0 * kPi * static_cast<double>(k * n) / N;
            acc += std::complex<double>(x[n].real(), x[n].imag())
                 * std::complex<double>(std::cos(ang), std::sin(ang));
        }
        ref[k] = std::complex<float>(static_cast<float>(acc.real()),
                                     static_cast<float>(acc.imag()));
    }

    std::vector<float> in(N * 2), out(N * 2);
    for (uint32_t n = 0; n < N; ++n) {
        in[n * 2]     = x[n].real();
        in[n * 2 + 1] = x[n].imag();
    }
    REQUIRE(compute->fft_forward(in.data(), out.data(), N));

    for (uint32_t k = 0; k < N; ++k) {
        REQUIRE(std::abs(out[k * 2]     - ref[k].real()) < 1e-3f);
        REQUIRE(std::abs(out[k * 2 + 1] - ref[k].imag()) < 1e-3f);
    }
}

TEST_CASE("GpuCompute FFT timed reports true GPU compute time", "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr uint32_t N = 1024;
    std::vector<float> in(N * 2), out(N * 2), ref(N * 2);
    for (uint32_t i = 0; i < N; ++i) { in[i * 2] = std::sin(0.02f * i); in[i * 2 + 1] = 0.0f; }

    // Timed forward must produce the same result as the plain forward.
    REQUIRE(compute->fft_forward(in.data(), ref.data(), N));
    double gpu_us = -123.0;
    REQUIRE(compute->fft_forward_timed(in.data(), out.data(), N, &gpu_us));
    for (uint32_t i = 0; i < N * 2; ++i) REQUIRE(std::abs(out[i] - ref[i]) < 1e-4f);

    const auto caps = compute->capabilities();
    if (caps.timestamp_query) {
        // A resolved timestamp delta is far below the ~ms-scale wall-clock
        // round trip (the readback-dominates finding). Under heavy GPU
        // contention on shared/virtualized runners the two timestamps can
        // coalesce to an unresolved zero delta, so accept 0.0 rather than
        // flaking; still reject a negative non-sentinel value or an absurd
        // magnitude. The FFT-correctness check above is the deterministic
        // signal — this bound only guards the reported timing's sanity.
        REQUIRE(gpu_us >= 0.0);
        REQUIRE(gpu_us < 5000.0);
    } else {
        REQUIRE(gpu_us == -1.0);  // timing unavailable -> sentinel
    }
}

TEST_CASE("GpuCompute FFT rejects non-power-of-two", "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;
    std::vector<float> in(200, 0.0f), out(200, 0.0f);
    REQUIRE_FALSE(compute->fft_forward(in.data(), out.data(), 100));
    REQUIRE_FALSE(compute->fft_inverse(in.data(), out.data(), 100));
}

// ── FFT Benchmark (GPU vs CPU crossover) ─────────────────────────────────────
//
// Measures the current GPU FFT path (upload + log2(N) dispatches + readback)
// against pulp::signal::Fft across sizes, to locate the crossover and quantify
// per-call overhead. Prints a table; asserts only that GPU output is produced
// (no perf assertion — GPU timing is host-load sensitive on shared runners).
// The per-pass-submit overhead this exposes motivates the single-submit FFT
// fusion tracked for the RT transport work.

TEST_CASE("GpuCompute FFT benchmark vs CPU", "[render][gpu][compute][.benchmark]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    const std::vector<uint32_t> sizes = {256, 1024, 4096, 16384, 65536};
    constexpr int iters = 20;

    std::printf("\n");
    std::printf("  FFT: CPU (pulp::signal::Fft) vs GPU (Stockham, single submit + sync readback)\n");
    std::printf("  %8s | %12s | %12s | %14s | %s\n",
                "N", "CPU us", "GPU wall us", "GPU compute us", "winner (wall)");
    std::printf("  ---------+--------------+--------------+----------------+--------------\n");

    for (uint32_t n : sizes) {
        // CPU baseline: complex forward FFT.
        Fft fft(static_cast<int>(n));
        std::vector<std::complex<float>> cbuf(n);
        for (uint32_t i = 0; i < n; ++i) {
            cbuf[i] = std::complex<float>(std::sin(0.01f * i), 0.0f);
        }
        auto cpu_t0 = std::chrono::high_resolution_clock::now();
        for (int it = 0; it < iters; ++it) {
            auto tmp = cbuf;
            fft.forward(tmp.data());
        }
        auto cpu_t1 = std::chrono::high_resolution_clock::now();
        const double cpu_us =
            std::chrono::duration_cast<std::chrono::nanoseconds>(cpu_t1 - cpu_t0).count()
            / 1000.0 / iters;

        // GPU: interleaved input, full forward (upload + dispatches + readback).
        std::vector<float> in(n * 2), out(n * 2);
        for (uint32_t i = 0; i < n; ++i) { in[i * 2] = std::sin(0.01f * i); in[i * 2 + 1] = 0.0f; }
        double gpu_compute_us = -1.0;
        REQUIRE(compute->fft_forward_timed(in.data(), out.data(), n,
                                           &gpu_compute_us));  // warm-up + true GPU time
        auto gpu_t0 = std::chrono::high_resolution_clock::now();
        for (int it = 0; it < iters; ++it) {
            REQUIRE(compute->fft_forward(in.data(), out.data(), n));
        }
        auto gpu_t1 = std::chrono::high_resolution_clock::now();
        const double gpu_us =
            std::chrono::duration_cast<std::chrono::nanoseconds>(gpu_t1 - gpu_t0).count()
            / 1000.0 / iters;

        const double speedup = gpu_us > 0.0 ? cpu_us / gpu_us : 0.0;
        std::printf("  %8u | %12.1f | %12.1f | %14.2f | %s\n",
                    n, cpu_us, gpu_us, gpu_compute_us, speedup > 1.0 ? "GPU" : "CPU");
    }
    std::printf("\n");
}

TEST_CASE("GpuCompute fused convolve vs 3-call readback cost",
          "[render][gpu][compute][.benchmark]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr uint32_t N = 4096;
    std::vector<float> ir_spec(N * 2, 0.0f);
    for (uint32_t i = 0; i < N; ++i) { ir_spec[i * 2] = 0.5f; ir_spec[i * 2 + 1] = 0.1f; }
    REQUIRE(compute->prepare_convolution(N, ir_spec.data()));

    std::vector<float> in(N * 2), out(N * 2), spec(N * 2), prod(N * 2);
    for (uint32_t i = 0; i < N; ++i) { in[i * 2] = std::sin(0.01f * i); in[i * 2 + 1] = 0.0f; }

    constexpr int iters = 30;
    compute->convolve(in.data(), out.data(), N);  // warm

    auto f0 = std::chrono::high_resolution_clock::now();
    for (int it = 0; it < iters; ++it) compute->convolve(in.data(), out.data(), N);
    auto f1 = std::chrono::high_resolution_clock::now();
    const double fused_us =
        std::chrono::duration_cast<std::chrono::nanoseconds>(f1 - f0).count() / 1000.0 / iters;

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int it = 0; it < iters; ++it) {
        compute->fft_forward(in.data(), spec.data(), N);
        compute->complex_multiply(spec.data(), ir_spec.data(), prod.data(), N);
        compute->fft_inverse(prod.data(), out.data(), N);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    const double three_us =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / 1000.0 / iters;

    std::printf("\n  Convolution (N=%u): fused 1-readback = %.1f us/block | "
                "3-call (3 readbacks) = %.1f us/block | speedup %.2fx\n",
                N, fused_us, three_us, three_us / fused_us);
}

TEST_CASE("GpuCompute batched convolve matches single", "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr uint32_t N = 256, B = 5;
    std::vector<float> irspec(N * 2);
    for (uint32_t i = 0; i < N; ++i) {
        irspec[i * 2]     = 0.5f * std::cos(0.10f * i);
        irspec[i * 2 + 1] = 0.3f * std::sin(0.07f * i);
    }
    REQUIRE(compute->prepare_convolution(N, irspec.data()));
    REQUIRE(compute->prepare_convolution_batch(N, irspec.data(), B));

    std::vector<float> in(N * 2 * B, 0.0f), out_batch(N * 2 * B, 0.0f);
    for (uint32_t b = 0; b < B; ++b)
        for (uint32_t i = 0; i < N; ++i)
            in[(b * N + i) * 2] = std::sin(0.05f * (i + b * 7));

    REQUIRE(compute->convolve_batch(in.data(), out_batch.data(), N, B));

    // Each batched block must equal the same block run through single convolve().
    for (uint32_t b = 0; b < B; ++b) {
        std::vector<float> single(N * 2, 0.0f);
        REQUIRE(compute->convolve(in.data() + b * N * 2, single.data(), N));
        for (uint32_t i = 0; i < N * 2; ++i) {
            REQUIRE(std::abs(out_batch[b * N * 2 + i] - single[i])
                    < 1e-3f * (1.0f + std::abs(single[i])));
        }
    }

    // Wrong batch count is rejected.
    REQUIRE_FALSE(compute->convolve_batch(in.data(), out_batch.data(), N, B + 1));
}

TEST_CASE("GpuCompute batched convolve isolates batches", "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr uint32_t N = 128, B = 4;
    std::vector<float> irspec(N * 2);
    for (uint32_t i = 0; i < N; ++i) { irspec[i * 2] = 0.7f; irspec[i * 2 + 1] = -0.2f; }
    REQUIRE(compute->prepare_convolution_batch(N, irspec.data(), B));

    // Only batch index 2 has a nonzero input (an impulse). With zero inputs in
    // the other batches, any cross-transform bleed would show as nonzero output
    // there.
    std::vector<float> in(N * 2 * B, 0.0f), out(N * 2 * B, 0.0f);
    in[(2u * N + 0u) * 2] = 1.0f;  // batch 2, sample 0, real
    REQUIRE(compute->convolve_batch(in.data(), out.data(), N, B));

    for (uint32_t b : {0u, 1u, 3u}) {
        for (uint32_t i = 0; i < N * 2; ++i) {
            REQUIRE(std::abs(out[b * N * 2 + i]) < 1e-4f);
        }
    }
    double energy = 0.0;
    for (uint32_t i = 0; i < N * 2; ++i) energy += std::abs(out[2u * N * 2 + i]);
    REQUIRE(energy > 0.1);  // batch 2 actually produced output
}

TEST_CASE("GpuCompute convolve_batch B=1 equals single convolve", "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr uint32_t N = 512;
    std::vector<float> irspec(N * 2);
    for (uint32_t i = 0; i < N; ++i) {
        irspec[i * 2]     = 0.3f * std::cos(0.02f * i);
        irspec[i * 2 + 1] = 0.4f * std::sin(0.03f * i);
    }
    REQUIRE(compute->prepare_convolution(N, irspec.data()));
    REQUIRE(compute->prepare_convolution_batch(N, irspec.data(), 1));

    std::vector<float> in(N * 2), single(N * 2), batched(N * 2);
    for (uint32_t i = 0; i < N; ++i) in[i * 2] = std::sin(0.05f * i);
    REQUIRE(compute->convolve(in.data(), single.data(), N));
    REQUIRE(compute->convolve_batch(in.data(), batched.data(), N, 1));
    for (uint32_t i = 0; i < N * 2; ++i) {
        REQUIRE(std::abs(batched[i] - single[i]) < 1e-4f * (1.0f + std::abs(single[i])));
    }
}

TEST_CASE("GpuCompute batched convolve amortizes readback",
          "[render][gpu][compute][.benchmark]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr uint32_t N = 2048, B = 16;
    std::vector<float> irspec(N * 2, 0.0f);
    for (uint32_t i = 0; i < N; ++i) { irspec[i * 2] = 0.4f; irspec[i * 2 + 1] = 0.1f; }
    REQUIRE(compute->prepare_convolution(N, irspec.data()));
    REQUIRE(compute->prepare_convolution_batch(N, irspec.data(), B));

    std::vector<float> in(N * 2 * B), out(N * 2 * B), one(N * 2);
    for (uint32_t i = 0; i < N * B; ++i) in[i * 2] = std::sin(0.01f * i);

    constexpr int iters = 10;
    compute->convolve_batch(in.data(), out.data(), N, B);  // warm

    auto s0 = std::chrono::high_resolution_clock::now();
    for (int it = 0; it < iters; ++it)
        for (uint32_t b = 0; b < B; ++b) compute->convolve(in.data() + b * N * 2, one.data(), N);
    auto s1 = std::chrono::high_resolution_clock::now();
    const double single_us =
        std::chrono::duration_cast<std::chrono::nanoseconds>(s1 - s0).count() / 1000.0 / (iters * B);

    auto b0 = std::chrono::high_resolution_clock::now();
    for (int it = 0; it < iters; ++it) compute->convolve_batch(in.data(), out.data(), N, B);
    auto b1 = std::chrono::high_resolution_clock::now();
    const double batch_us =
        std::chrono::duration_cast<std::chrono::nanoseconds>(b1 - b0).count() / 1000.0 / (iters * B);

    std::printf("\n  Convolution (N=%u, batch=%u): single = %.1f us/block | "
                "batched = %.1f us/block | speedup %.2fx\n",
                N, B, single_us, batch_us, single_us / batch_us);
}

TEST_CASE("GpuCompute matmul matches CPU", "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr uint32_t M = 6, K = 4, N = 5;
    std::vector<float> a(M * K), b(K * N), c(M * N, 0.0f), ref(M * N, 0.0f);
    for (uint32_t i = 0; i < M * K; ++i) a[i] = std::sin(0.3f * i) + 0.5f;
    for (uint32_t i = 0; i < K * N; ++i) b[i] = std::cos(0.2f * i) - 0.25f;

    REQUIRE(compute->matmul(a.data(), b.data(), c.data(), M, K, N));

    for (uint32_t r = 0; r < M; ++r)
        for (uint32_t col = 0; col < N; ++col) {
            double acc = 0.0;
            for (uint32_t kk = 0; kk < K; ++kk)
                acc += static_cast<double>(a[r * K + kk]) * b[kk * N + col];
            ref[r * N + col] = static_cast<float>(acc);
        }
    for (uint32_t i = 0; i < M * N; ++i)
        REQUIRE(std::abs(c[i] - ref[i]) < 1e-4f * (1.0f + std::abs(ref[i])));

    REQUIRE_FALSE(compute->matmul(a.data(), b.data(), c.data(), 0, K, N));  // invalid
}

TEST_CASE("GpuCompute additive synth produces the requested partials",
          "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr uint32_t N = 2048;
    constexpr float SR = 48000.0f;
    constexpr uint32_t K1 = 10, K2 = 20, K3 = 40;  // exact FFT bins (integer cycles)
    std::vector<float> partials = {
        K1 * SR / N, 1.00f, 0.0f,
        K2 * SR / N, 0.50f, 0.0f,
        K3 * SR / N, 0.25f, 0.0f,
    };
    std::vector<float> out(N, 0.0f);
    REQUIRE(compute->additive_synth(partials.data(), out.data(), 3, N, SR, 0.0f));

    std::vector<std::complex<float>> s(N);
    for (uint32_t i = 0; i < N; ++i) s[i] = std::complex<float>(out[i], 0.0f);
    pulp::signal::Fft fft(static_cast<int>(N));
    fft.forward(s.data());
    auto mag = [&](uint32_t k) { return std::abs(s[k]); };

    // Sharp peaks at exactly K1/K2/K3, magnitudes in the amplitude ratio.
    REQUIRE(mag(K1) > 50.0f * mag(K1 + 5));
    REQUIRE(mag(K1) > mag(K2));
    REQUIRE(mag(K2) > mag(K3));
    REQUIRE(std::abs(mag(K2) / mag(K1) - 0.5f) < 0.05f);
    REQUIRE(std::abs(mag(K3) / mag(K1) - 0.25f) < 0.05f);

    REQUIRE_FALSE(compute->additive_synth(partials.data(), out.data(), 0, N, SR, 0.0f));
}

// Guards the block-parallel additive_synth reshape (one workgroup per sample, WG
// lanes tree-reducing the partials sum): numeric parity with a scalar CPU
// reference across partial counts spanning below / at / above the 256-lane
// workgroup, a sample count that is NOT a multiple of the workgroup, both routing
// paths (cooperative vs serial), the >65535-sample grid-stride path, and
// run-to-run determinism (the tree reduction is fixed-order and atomics-free).
TEST_CASE("GpuCompute additive synth matches a CPU reference across workgroup sizes",
          "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr float SR = 48000.0f;
    constexpr double TWO_PI = 6.28318530717958647692;
    auto cpu_add = [&](const std::vector<float>& parts, uint32_t P, uint32_t s,
                       float t0) {
        const double t = (static_cast<double>(t0) + s) / SR;
        double acc = 0.0;  // scalar reference, serial order
        for (uint32_t i = 0; i < P; ++i) {
            const double f = parts[i * 3], a = parts[i * 3 + 1], ph = parts[i * 3 + 2];
            acc += a * std::sin(TWO_PI * f * t + ph);
        }
        return static_cast<float>(acc);
    };
    auto make_partials = [](uint32_t P) {
        std::vector<float> m(static_cast<std::size_t>(P) * 3);
        for (uint32_t i = 0; i < P; ++i) {
            m[i * 3 + 0] = 55.0f + 11.0f * static_cast<float>(i);   // freq
            m[i * 3 + 1] = 1.0f / (1.0f + static_cast<float>(i));   // amp
            m[i * 3 + 2] = 0.1f * static_cast<float>(i);            // phase
        }
        return m;
    };

    // Cooperative-routed: non-multiple-of-256 block; partial counts spanning WG.
    const uint32_t S = 333;
    for (uint32_t P : {1u, 2u, 200u, 256u, 300u, 1000u}) {
        const auto parts = make_partials(P);
        std::vector<float> out(S, 0.0f), out2(S, 0.0f);
        REQUIRE(compute->additive_synth(parts.data(), out.data(), P, S, SR, 0.0f));
        REQUIRE(compute->additive_synth(parts.data(), out2.data(), P, S, SR, 0.0f));
        for (uint32_t s = 0; s < S; ++s) {
            const float ref = cpu_add(parts, P, s, 0.0f);
            REQUIRE(std::abs(out[s] - ref) < 1e-3f * (1.0f + std::abs(ref)));
            REQUIRE(out[s] == out2[s]);  // deterministic run-to-run
        }
    }

    // Serial-routed: a block large enough to fill the device on the serial kernel
    // (ceil(S/256) >= 32) routes to the unchanged one-thread-per-sample path.
    {
        const uint32_t S2 = 16384, P = 300;  // ceil(16384/256)=64 >= 32 -> serial
        const auto parts = make_partials(P);
        std::vector<float> out(S2, 0.0f), out2(S2, 0.0f);
        REQUIRE(compute->additive_synth(parts.data(), out.data(), P, S2, SR, 0.0f));
        REQUIRE(compute->additive_synth(parts.data(), out2.data(), P, S2, SR, 0.0f));
        for (uint32_t s = 0; s < S2; ++s) {
            const float ref = cpu_add(parts, P, s, 0.0f);
            REQUIRE(std::abs(out[s] - ref) < 1e-3f * (1.0f + std::abs(ref)));
            REQUIRE(out[s] == out2[s]);
        }
    }

    // Grid-stride path (cooperative): a block whose serial dispatch would exceed
    // the 65535 workgroup-per-dimension cap routes to the cooperative kernel,
    // which caps its dispatch and grid-strides — so each workgroup wraps to
    // several samples. Only reachable at the maximum supported block, so keep the
    // per-sample work tiny (P=1) and spot-check indices across the wrap boundary.
    {
        const uint32_t Sbig = 1u << 24;  // 16,777,216 -> ceil/256 = 65536 > 65535
        const uint32_t P = 1;
        const auto parts = make_partials(P);
        std::vector<float> out(Sbig, 0.0f);
        REQUIRE(compute->additive_synth(parts.data(), out.data(), P, Sbig, SR, 0.0f));
        // Check indices at LOW t only: at very large sample indices t is huge and
        // f32 sin's argument-reduction error swamps the double-precision reference
        // (an f32-large-angle artifact shared with the serial kernel, not a
        // grid-stride bug). Workgroup 0 owns samples {0, 65535, 131070} (dispatch
        // caps at 65535, so stride == 65535), so these still exercise the wrap.
        for (uint32_t s : {0u, 1u, 65535u, 65536u, 131070u, 131071u}) {
            const float ref = cpu_add(parts, P, s, 0.0f);
            REQUIRE(std::abs(out[s] - ref) < 1e-3f * (1.0f + std::abs(ref)));
        }
    }
}

TEST_CASE("GpuCompute modal strike decays and carries mode frequencies",
          "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr uint32_t N = 8192;
    constexpr float SR = 48000.0f;
    const float fA = 300.0f, fB = 1200.0f;
    // [freq, amp, decay, phase]; fB decays much faster than fA. Decay rates
    // chosen so the strike clearly decays >2x across the ~0.17 s buffer.
    std::vector<float> modes = {fA, 1.0f, 12.0f, 0.0f, fB, 1.0f, 40.0f, 0.0f};
    std::vector<float> out(N, 0.0f);
    REQUIRE(compute->modal_strike(modes.data(), out.data(), 2, N, SR, 0.0f));

    auto rms = [&](uint32_t a, uint32_t b) {
        double e = 0.0; for (uint32_t i = a; i < b; ++i) e += out[i] * out[i];
        return std::sqrt(e / (b - a));
    };
    const float early = rms(0, 1024), late = rms(N - 1024, N);
    REQUIRE(early > 0.01f);
    REQUIRE(early > late * 2.0f);  // the strike decays

    std::vector<std::complex<float>> s(N);
    for (uint32_t i = 0; i < N; ++i) s[i] = std::complex<float>(out[i], 0.0f);
    pulp::signal::Fft fft(static_cast<int>(N));
    fft.forward(s.data());
    auto band = [&](uint32_t c) {
        float m = 0.0f;
        for (int k = static_cast<int>(c) - 3; k <= static_cast<int>(c) + 3; ++k)
            m = std::max(m, std::abs(s[static_cast<uint32_t>(k)]));
        return m;
    };
    const uint32_t binA = static_cast<uint32_t>(fA * N / SR + 0.5f);
    const uint32_t binB = static_cast<uint32_t>(fB * N / SR + 0.5f);
    REQUIRE(band(binA) > 5.0f * band(binA + 60));   // clear resonance at fA
    REQUIRE(band(binB) > band(binB + 60));          // energy at fB too

    REQUIRE_FALSE(compute->modal_strike(modes.data(), out.data(), 0, N, SR, 0.0f));
}

TEST_CASE("GpuCompute granular cloud places windowed grains", "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr uint32_t SRCLEN = 4096, N = 2048;
    std::vector<float> source(SRCLEN);
    for (uint32_t i = 0; i < SRCLEN; ++i) source[i] = std::sin(0.1f * i);

    // Two grains: [onset, duration, src_pos, pitch, amp].
    std::vector<float> grains = {
        100.0f, 256.0f, 0.0f,   1.0f, 1.0f,
        1000.0f, 256.0f, 500.0f, 1.0f, 1.0f,
    };
    std::vector<float> out(N, 0.0f);
    REQUIRE(compute->granular_cloud(grains.data(), source.data(), out.data(), 2, SRCLEN, N));

    auto rms = [&](uint32_t a, uint32_t b) {
        double e = 0.0; for (uint32_t i = a; i < b; ++i) e += out[i] * out[i];
        return std::sqrt(e / (b - a));
    };
    REQUIRE(rms(0, 90) < 1e-4f);          // silence before grain 1
    REQUIRE(rms(150, 350) > 0.05f);        // energy inside grain 1
    REQUIRE(rms(400, 950) < 1e-4f);        // silent gap between grains
    REQUIRE(rms(1050, 1250) > 0.05f);      // energy inside grain 2
    REQUIRE(rms(1300, N) < 1e-4f);         // silence after grain 2

    // Hann window: grain center louder than its edge.
    REQUIRE(rms(220, 240) > rms(102, 112));

    REQUIRE_FALSE(compute->granular_cloud(grains.data(), source.data(), out.data(), 0, SRCLEN, N));
}

TEST_CASE("GpuCompute dense_tanh matches CPU", "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr uint32_t IN = 5, OUT = 4;
    std::vector<float> x(IN), w(OUT * IN), b(OUT), out(OUT, 0.0f), ref(OUT, 0.0f);
    for (uint32_t i = 0; i < IN; ++i) x[i] = std::sin(0.4f * i) - 0.2f;
    for (uint32_t i = 0; i < OUT * IN; ++i) w[i] = std::cos(0.15f * i) * 0.5f;
    for (uint32_t j = 0; j < OUT; ++j) b[j] = 0.1f * j - 0.15f;

    REQUIRE(compute->dense_tanh(x.data(), w.data(), b.data(), out.data(), IN, OUT));

    for (uint32_t j = 0; j < OUT; ++j) {
        double acc = b[j];
        for (uint32_t i = 0; i < IN; ++i) acc += static_cast<double>(w[j * IN + i]) * x[i];
        ref[j] = std::tanh(static_cast<float>(acc));
    }
    for (uint32_t j = 0; j < OUT; ++j)
        REQUIRE(std::abs(out[j] - ref[j]) < 1e-4f);

    REQUIRE_FALSE(compute->dense_tanh(x.data(), w.data(), b.data(), out.data(), 0, OUT));
}

// ── Fused WaveNet inference ─────────────────────────────────────────────────

namespace {
// Scalar CPU reference for the simplest WaveNet: 1 array, 1 layer, channels=1,
// kernel=1, ungated, no head bias. With kernel=1 there is no dilation history so
// each output sample depends only on the current input:
//   layer_in = Wre * x
//   z        = Wconv * layer_in + bconv + Wmix * x   (condition = the raw input)
//   a        = tanh(z)                               (the head accumulates a)
//   out      = Whr * a * head_scale
// The layer1x1 (residual) weights exist in the blob but do not affect a
// single-layer array's output (nothing downstream consumes the residual).
float wavenet_ref_1x1(float x, float Wre, float Wconv, float bconv, float Wmix,
                      float Whr, float head_scale) {
    const float layer_in = Wre * x;
    const float z = Wconv * layer_in + bconv + Wmix * x;
    return Whr * std::tanh(z) * head_scale;
}

// Two chained arrays (each channels=1/kernel=1/1 layer/ungated/head_size=1). The
// point of coverage: array 1's head accumulator is SEEDED with array 0's head
// output before it accumulates its own activation, and array 0's accumulator
// starts cleared. If the seed were dropped (or array 0 not cleared) the result
// changes grossly — so this pins the submission-diet's "clear array 0 only, seed
// the rest" contract. rechannel of array 1 reads array 0's residual output; the
// input mixin uses the original mono input for every array.
float wavenet_ref_1x1_2arrays(float x,
                              float Wre0, float Wconv0, float bconv0, float Wmix0,
                              float W1x1_0, float b1x1_0, float Whr0,
                              float Wre1, float Wconv1, float bconv1, float Wmix1,
                              float Whr1, float head_scale) {
    const float lin0 = Wre0 * x;
    const float z0 = Wconv0 * lin0 + bconv0 + Wmix0 * x;
    const float a0 = std::tanh(z0);
    const float head0 = Whr0 * a0;                       // array 0 head output = the seed
    const float res0 = lin0 + b1x1_0 + W1x1_0 * a0;      // residual → array 1 rechannel input
    const float lin1 = Wre1 * res0;
    const float z1 = Wconv1 * lin1 + bconv1 + Wmix1 * x;
    const float headacc1 = head0 + std::tanh(z1);        // SEED + array 1 activation
    return head_scale * (Whr1 * headacc1);
}
}  // namespace

TEST_CASE("GpuCompute wavenet_forward matches a scalar reference",
          "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    // Weight blob for 1 array / 1 layer / channels=1 / kernel=1 / ungated:
    // [rechannel Wre][conv Wconv][conv bconv][mixin Wmix]
    // [layer1x1 W1x1][layer1x1 b1x1][head_rechannel Whr][trailing head_scale].
    const float Wre = 0.7f, Wconv = 1.3f, bconv = -0.2f, Wmix = 0.5f;
    const float W1x1 = 0.9f, b1x1 = 0.1f, Whr = 1.1f, head_scale = 0.8f;
    std::vector<float> weights = {Wre, Wconv, bconv, Wmix, W1x1, b1x1, Whr, head_scale};

    std::vector<uint32_t> dilations = {1};
    GpuCompute::WavenetLayerArraySpec spec;
    spec.input_size = 1; spec.condition_size = 1; spec.channels = 1;
    spec.kernel = 1; spec.head_size = 1; spec.gated = 0; spec.head_bias = 0;
    spec.dilations = dilations.data(); spec.num_layers = 1;

    const uint32_t B = 16;
    REQUIRE(compute->prepare_wavenet(&spec, 1, weights.data(),
                                     static_cast<uint32_t>(weights.size()), B, head_scale));

    std::vector<float> in(B), out(B, 0.0f);
    for (uint32_t i = 0; i < B; ++i) in[i] = 0.3f * std::sin(0.2f * i) - 0.1f;
    REQUIRE(compute->wavenet_forward(in.data(), out.data(), B));

    for (uint32_t i = 0; i < B; ++i) {
        const float expect =
            wavenet_ref_1x1(in[i], Wre, Wconv, bconv, Wmix, Whr, head_scale);
        REQUIRE(std::abs(out[i] - expect) < 1e-5f);
    }
}

TEST_CASE("GpuCompute wavenet_forward is deterministic across a gated multi-layer net",
          "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    // 1 array, 2 gated dilated layers, channels=4, kernel=3, head_size=4. This
    // exercises the gated (tanh*sigmoid) path, multi-channel conv, dilation
    // history, and the head accumulate/rechannel — not a hand reference, but a
    // determinism + finiteness contract on the full forward.
    const uint32_t C = 4, K = 3, L = 2, H = 4, Z = 2 * C;
    const uint32_t need = C /*rechannel*/
        + L * (Z * C * K + Z /*conv W+b*/ + Z /*mixin*/ + C * C + C /*1x1 W+b*/)
        + H * C /*head rechannel*/ + 1 /*trailing head_scale*/;
    std::vector<float> weights(need);
    for (uint32_t i = 0; i < need; ++i)
        weights[i] = 0.15f * std::sin(0.37f * i) - 0.05f * std::cos(0.11f * i);

    std::vector<uint32_t> dilations = {1, 2};
    GpuCompute::WavenetLayerArraySpec spec;
    spec.input_size = 1; spec.condition_size = 1; spec.channels = C;
    spec.kernel = K; spec.head_size = H; spec.gated = 1; spec.head_bias = 0;
    spec.dilations = dilations.data(); spec.num_layers = L;

    const uint32_t B = 32;
    const float head_scale = 0.6f;
    std::vector<float> in(B);
    for (uint32_t i = 0; i < B; ++i) in[i] = 0.4f * std::sin(0.23f * i);

    std::vector<float> a(B, 0.0f), b(B, 0.0f);
    REQUIRE(compute->prepare_wavenet(&spec, 1, weights.data(), need, B, head_scale));
    REQUIRE(compute->wavenet_forward(in.data(), a.data(), B));
    // A fresh plan on the same input must reproduce it bit-for-bit.
    REQUIRE(compute->prepare_wavenet(&spec, 1, weights.data(), need, B, head_scale));
    REQUIRE(compute->wavenet_forward(in.data(), b.data(), B));
    for (uint32_t i = 0; i < B; ++i) {
        REQUIRE(std::isfinite(a[i]));
        REQUIRE(a[i] == b[i]);
    }
}

TEST_CASE("GpuCompute wavenet_forward chains two arrays through the head seed",
          "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    // Two arrays, each channels=1/kernel=1/1 layer/ungated/head_size=1. This is
    // the multi-array topology real .nam WaveNets use, and the one the submission
    // diet optimizes: only array 0's head accumulator is cleared, and array 1's
    // is seeded from array 0's head output. Weight blob is [array0 7][array1 7]
    // [trailing head_scale] = 15 floats, laid out as prepare_wavenet consumes it.
    const float Wre0 = 0.7f, Wconv0 = 1.3f, bconv0 = -0.2f, Wmix0 = 0.5f;
    const float W1x1_0 = 0.9f, b1x1_0 = 0.1f, Whr0 = 1.1f;
    const float Wre1 = 0.6f, Wconv1 = -0.8f, bconv1 = 0.15f, Wmix1 = 0.4f;
    const float W1x1_1 = 0.5f, b1x1_1 = -0.05f, Whr1 = 0.95f;
    const float head_scale = 0.8f;
    std::vector<float> weights = {
        Wre0, Wconv0, bconv0, Wmix0, W1x1_0, b1x1_0, Whr0,
        Wre1, Wconv1, bconv1, Wmix1, W1x1_1, b1x1_1, Whr1,
        head_scale};

    std::vector<uint32_t> dilations = {1};
    GpuCompute::WavenetLayerArraySpec spec;
    spec.input_size = 1; spec.condition_size = 1; spec.channels = 1;
    spec.kernel = 1; spec.head_size = 1; spec.gated = 0; spec.head_bias = 0;
    spec.dilations = dilations.data(); spec.num_layers = 1;
    GpuCompute::WavenetLayerArraySpec specs[2] = {spec, spec};

    const uint32_t B = 16;
    REQUIRE(compute->prepare_wavenet(specs, 2, weights.data(),
                                     static_cast<uint32_t>(weights.size()), B, head_scale));

    std::vector<float> in(B), out(B, 0.0f);
    for (uint32_t i = 0; i < B; ++i) in[i] = 0.3f * std::sin(0.2f * i) - 0.1f;
    REQUIRE(compute->wavenet_forward(in.data(), out.data(), B));

    for (uint32_t i = 0; i < B; ++i) {
        const float expect = wavenet_ref_1x1_2arrays(
            in[i], Wre0, Wconv0, bconv0, Wmix0, W1x1_0, b1x1_0, Whr0,
            Wre1, Wconv1, bconv1, Wmix1, Whr1, head_scale);
        REQUIRE(std::abs(out[i] - expect) < 5e-5f);
    }
}

TEST_CASE("GpuCompute wavenet_forward keeps two instances independent on one device",
          "[render][gpu][compute]") {
    // Two WaveNet streams (e.g. stereo channels) prepared on ONE device with the
    // SAME block_size but different weights must not collide: each instance keeps
    // its own device buffers and produces its own reference. Before the plans were
    // keyed by (block_size, instance) the second prepare clobbered the shared plan,
    // so instance 0 would have run instance 1's weights — this pins the isolation
    // that lets a stereo plugin share one device instead of one per channel.
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    std::vector<uint32_t> dilations = {1};
    GpuCompute::WavenetLayerArraySpec spec;
    spec.input_size = 1; spec.condition_size = 1; spec.channels = 1;
    spec.kernel = 1; spec.head_size = 1; spec.gated = 0; spec.head_bias = 0;
    spec.dilations = dilations.data(); spec.num_layers = 1;

    // Distinct weights per instance so a collision would be caught by value.
    const float head_scale0 = 0.8f, head_scale1 = 0.6f;
    std::vector<float> w0 = {0.7f, 1.3f, -0.2f, 0.5f, 0.9f, 0.1f, 1.1f, head_scale0};
    std::vector<float> w1 = {0.4f, 0.9f,  0.3f, 1.2f, 0.6f, 0.2f, 0.7f, head_scale1};

    const uint32_t B = 16;
    // Prepare BOTH instances before forwarding EITHER — the ordering under which a
    // shared-plan bug would let instance 1's prepare overwrite instance 0.
    REQUIRE(compute->prepare_wavenet(&spec, 1, w0.data(),
                                     static_cast<uint32_t>(w0.size()), B, head_scale0, 0));
    REQUIRE(compute->prepare_wavenet(&spec, 1, w1.data(),
                                     static_cast<uint32_t>(w1.size()), B, head_scale1, 1));

    std::vector<float> in0(B), in1(B), out0(B, 0.0f), out1(B, 0.0f);
    for (uint32_t i = 0; i < B; ++i) {
        in0[i] = 0.3f * std::sin(0.2f * i) - 0.1f;
        in1[i] = 0.25f * std::cos(0.17f * i) + 0.05f;
    }
    REQUIRE(compute->wavenet_forward(in0.data(), out0.data(), B, 0));
    REQUIRE(compute->wavenet_forward(in1.data(), out1.data(), B, 1));

    for (uint32_t i = 0; i < B; ++i) {
        const float e0 = wavenet_ref_1x1(in0[i], w0[0], w0[1], w0[2], w0[3], w0[6], head_scale0);
        const float e1 = wavenet_ref_1x1(in1[i], w1[0], w1[1], w1[2], w1[3], w1[6], head_scale1);
        REQUIRE(std::abs(out0[i] - e0) < 1e-5f);
        REQUIRE(std::abs(out1[i] - e1) < 1e-5f);
    }
}

TEST_CASE("GpuCompute prepare_wavenet rejects unsupported shapes",
          "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    std::vector<uint32_t> dilations = {1};
    auto base = [&]() {
        GpuCompute::WavenetLayerArraySpec s;
        s.input_size = 1; s.condition_size = 1; s.channels = 1; s.kernel = 1;
        s.head_size = 1; s.gated = 0; s.head_bias = 0;
        s.dilations = dilations.data(); s.num_layers = 1;
        return s;
    };
    // A valid 8-weight blob for the base (channels=1/kernel=1/1 layer) shape.
    std::vector<float> ok = {0.7f, 1.3f, -0.2f, 0.5f, 0.9f, 0.1f, 1.1f, 0.8f};
    const uint32_t B = 8;

    // Sanity: the base shape is accepted.
    { auto s = base(); REQUIRE(compute->prepare_wavenet(&s, 1, ok.data(), 8, B, 0.8f)); }

    // A non-mono condition is not modeled.
    { auto s = base(); s.condition_size = 2;
      REQUIRE_FALSE(compute->prepare_wavenet(&s, 1, ok.data(), 8, B, 0.8f)); }

    // Channel count above the fixed cap.
    { auto s = base(); s.channels = 65;
      REQUIRE_FALSE(compute->prepare_wavenet(&s, 1, ok.data(), 8, B, 0.8f)); }

    // Zero layers.
    { auto s = base(); s.num_layers = 0;
      REQUIRE_FALSE(compute->prepare_wavenet(&s, 1, ok.data(), 8, B, 0.8f)); }

    // A weight blob that does not match the declared shape.
    { auto s = base(); REQUIRE_FALSE(compute->prepare_wavenet(&s, 1, ok.data(), 7, B, 0.8f)); }

    // wavenet_forward before a matching prepare (wrong block size) fails.
    { auto s = base(); REQUIRE(compute->prepare_wavenet(&s, 1, ok.data(), 8, B, 0.8f));
      std::vector<float> in(B, 0.1f), out(B * 2, 0.0f);
      REQUIRE_FALSE(compute->wavenet_forward(in.data(), out.data(), B * 2)); }
}

// ── Capability Report Tests ─────────────────────────────────────────────────

TEST_CASE("GpuCompute capability report", "[render][gpu][compute]") {
    auto compute = GpuCompute::create();
    if (!compute) return;

    // Before initialization the device is unavailable.
    REQUIRE_FALSE(compute->capabilities().available);

    if (!compute->initialize_standalone()) return;

    const auto caps = compute->capabilities();
    REQUIRE(caps.available);
    REQUIRE_FALSE(caps.backend.empty());
    REQUIRE(caps.max_storage_buffer_binding_size > 0);
    REQUIRE(caps.max_buffer_size > 0);
    // Our compute kernels dispatch at workgroup_size(256).
    REQUIRE(caps.max_compute_invocations_per_workgroup >= 256u);
    REQUIRE(caps.max_compute_workgroup_size_x >= 256u);
    // Derived FFT cap is a power of two and covers the sizes the FFT tests use.
    REQUIRE(caps.max_fft_size >= 1024u);
    REQUIRE((caps.max_fft_size & (caps.max_fft_size - 1u)) == 0u);
}

// ── Device Sharing Tests ────────────────────────────────────────────────────

TEST_CASE("GpuCompute device sharing with GpuSurface", "[render][gpu][compute]") {
    auto surface = GpuSurface::create_dawn();
    if (!surface) return;

    GpuSurface::Config config;
    config.width = 64;
    config.height = 64;
    if (!surface->initialize(config)) return;

    auto compute = GpuCompute::create();
    if (!compute) return;
    if (!compute->initialize_from_surface(*surface)) return;
    REQUIRE(compute->is_initialized());

    // Verify compute works on the shared device
    constexpr uint32_t N = 256;
    std::vector<float> input(N * 2, 1.0f);
    std::vector<float> output(N);
    REQUIRE(compute->compute_magnitude(input.data(), output.data(), N));

    // (1 + 1i) → magnitude = sqrt(2) ≈ 1.414
    for (uint32_t i = 0; i < N; ++i) {
        REQUIRE(std::abs(output[i] - std::sqrt(2.0f)) < 1e-4f);
    }
}

TEST_CASE("GpuCompute device sharing report", "[render][gpu][compute]") {
    auto surface = GpuSurface::create_dawn();
    if (!surface) return;

    GpuSurface::Config config;
    config.width = 64;
    config.height = 64;
    if (!surface->initialize(config)) return;

    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    auto report = compute->verify_device_sharing(*surface);

    INFO("Device sharing report: " << report.notes);
    REQUIRE(report.device_obtained);
    REQUIRE(report.second_consumer_works);
    REQUIRE(report.concurrent_submission_ok);
    REQUIRE(report.memory_pressure_ok);
    REQUIRE_FALSE(report.backend_name.empty());
}

// ── Benchmark Tests ─────────────────────────────────────────────────────────

TEST_CASE("GpuCompute benchmark magnitude", "[render][gpu][compute][benchmark]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    std::vector<uint32_t> sizes = {256, 1024, 4096, 16384, 65536, 262144, 1048576};
    auto results = compute->benchmark_magnitude(sizes, 20);

    std::printf("\n╔═══════════════════════════════════════════════════════════════════╗\n");
    std::printf("║  Magnitude Spectrum: CPU vs GPU Benchmark                       ║\n");
    std::printf("╠═══════════╦═══════════╦═══════════╦═══════════╦═════════════════╣\n");
    std::printf("║   Bins    ║  CPU (us) ║  GPU (us) ║  Speedup  ║    Winner       ║\n");
    std::printf("╠═══════════╬═══════════╬═══════════╬═══════════╬═════════════════╣\n");

    for (const auto& r : results) {
        double speedup = r.cpu_baseline_us / r.total_us;
        std::printf("║ %9u ║ %9.1f ║ %9.1f ║ %7.2fx  ║  %-14s ║\n",
            r.num_elements,
            r.cpu_baseline_us,
            r.total_us,
            speedup,
            r.gpu_faster ? "GPU" : "CPU");
    }
    std::printf("╚═══════════╩═══════════╩═══════════╩═══════════╩═════════════════╝\n\n");

    // At large sizes, GPU should eventually win (or at least not be catastrophic)
    REQUIRE(results.size() == sizes.size());
}

// ── Staging buffer pool reuse ───────────────────────────────────────────────
//
// Without the pool, compute_magnitude() created a fresh wgpu::Buffer on every
// call — steady-state GPU memory kept climbing because Dawn can't immediately
// reclaim backing storage on release, and the bench harness reveals this as an
// allocator-churn hotspot. With the StagingBufferPool, buffers of the same
// size/usage are recycled; 1000 iterations should reach steady state after the
// first 2–3 calls and produce a flat allocation profile.
//
// Verifies:
// - No correctness regression under sustained hot-loop usage
// - Call throughput stays bounded (catches OnSubmittedWorkDone leaks)
// - Wall-time does not degrade linearly with iteration count
TEST_CASE("GpuCompute magnitude hot loop reuses pool buffers",
          "[render][gpu][compute][pool]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    constexpr uint32_t N = 4096;
    std::vector<float> complex_pairs(N * 2);
    std::vector<float> magnitudes(N);
    for (uint32_t i = 0; i < N; ++i) {
        complex_pairs[i * 2]     = 3.0f;
        complex_pairs[i * 2 + 1] = 4.0f;
    }

    // Warm-up so pipeline/adapter init doesn't skew the first timed
    // segment.
    for (int i = 0; i < 5; ++i) {
        REQUIRE(compute->compute_magnitude(
            complex_pairs.data(), magnitudes.data(), N));
    }

    using Clock = std::chrono::high_resolution_clock;

    // Measure several batches and take the MEDIAN per-call time. CI runners are
    // shared; a single averaged run can be inflated by a transient host-load
    // spike (a concurrent build, another VM's GPU work), which previously made
    // this assertion flake on busy hosts. The median across batches resists a
    // one-off spike without masking a real, sustained regression.
    // Total synchronous submits = kBatches * kItersPerBatch. Each submit is a
    // blocking GPU round-trip, so this count is bounded by the ctest wall-clock
    // timeout (--timeout 120): the per-call ceiling is 120s / total_submits.
    // The 100 ms/call runaway sentinel below is only *reachable* if that ceiling
    // sits well above 100 ms — otherwise a real leak times the test out at the
    // ceiling instead of tripping the sentinel, and the flake window widens on a
    // slow/virtualized overflow runner. 5 * 100 = 500 submits → ~240 ms/call
    // ceiling, comfortably above the 100 ms sentinel. (Was 5 * 1000 = 5000 →
    // ~24 ms ceiling, below the sentinel — see #3411.)
    constexpr int kBatches = 5;
    constexpr int kItersPerBatch = 100;
    std::vector<double> per_call_us_samples;
    per_call_us_samples.reserve(kBatches);
    for (int b = 0; b < kBatches; ++b) {
        const auto t0 = Clock::now();
        for (int i = 0; i < kItersPerBatch; ++i) {
            REQUIRE(compute->compute_magnitude(
                complex_pairs.data(), magnitudes.data(), N));
        }
        const auto t1 = Clock::now();
        per_call_us_samples.push_back(
            std::chrono::duration<double, std::micro>(t1 - t0).count()
            / kItersPerBatch);
    }

    // Sanity on the final output — catches pool aliasing bugs where a reused
    // buffer retained stale data.
    for (uint32_t i = 0; i < N; ++i) {
        REQUIRE(std::abs(magnitudes[i] - 5.0f) < 1e-4f);
    }

    std::sort(per_call_us_samples.begin(), per_call_us_samples.end());
    const double median_us = per_call_us_samples[kBatches / 2];
    std::printf(
        "compute_magnitude pool hot-loop: median %.2f us/call over %d batches "
        "x %d iters (N=%u)\n",
        median_us, kBatches, kItersPerBatch, N);

    // Always-on runaway sentinel. This test exists to catch allocator churn or
    // a leaked OnSubmittedWorkDone submit, which manifest as orders-of-magnitude
    // slowdown — NOT a tight perf spec. 100 ms/call is unreachable without a
    // real leak yet far above any shared-runner contention, so it never
    // false-positives on a busy host. (Warm M-series steady state: ~100–500 us.)
    REQUIRE(median_us < 100000.0);

    // Tight perf gate — enforced ONLY in the dedicated, serialized perf lane
    // (PULP_PERF_STRICT=1) where the host is guaranteed GPU-quiet and not an
    // overflow/virtualized runner with no real GPU. Elsewhere the median is
    // telemetry only. See the macos-perf CI lane and #3299 capacity serialization.
    if (const char* strict = std::getenv("PULP_PERF_STRICT");
        strict && strict[0] && strict[0] != '0') {
        INFO("PULP_PERF_STRICT: enforcing tight GPU-quiet perf threshold "
             "(median_us=" << median_us << ")");
        REQUIRE(median_us < 10000.0);
    }
}

TEST_CASE("GpuCompute benchmark complex multiply", "[render][gpu][compute][benchmark]") {
    auto compute = GpuCompute::create();
    if (!compute || !compute->initialize_standalone()) return;

    std::vector<uint32_t> sizes = {256, 1024, 4096, 16384, 65536, 262144, 1048576};
    auto results = compute->benchmark_complex_multiply(sizes, 20);

    std::printf("\n╔═══════════════════════════════════════════════════════════════════╗\n");
    std::printf("║  Complex Multiply: CPU vs GPU Benchmark                         ║\n");
    std::printf("╠═══════════╦═══════════╦═══════════╦═══════════╦═════════════════╣\n");
    std::printf("║  Elements ║  CPU (us) ║  GPU (us) ║  Speedup  ║    Winner       ║\n");
    std::printf("╠═══════════╬═══════════╬═══════════╬═══════════╬═════════════════╣\n");

    for (const auto& r : results) {
        double speedup = r.cpu_baseline_us / r.total_us;
        std::printf("║ %9u ║ %9.1f ║ %9.1f ║ %7.2fx  ║  %-14s ║\n",
            r.num_elements,
            r.cpu_baseline_us,
            r.total_us,
            speedup,
            r.gpu_faster ? "GPU" : "CPU");
    }
    std::printf("╚═══════════╩═══════════╩═══════════╩═══════════╩═════════════════╝\n\n");

    REQUIRE(results.size() == sizes.size());
}
