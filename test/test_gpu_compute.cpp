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
        // True GPU compute time is positive and far below the ~ms-scale
        // wall-clock round trip (this is the readback-dominates finding).
        REQUIRE(gpu_us > 0.0);
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
