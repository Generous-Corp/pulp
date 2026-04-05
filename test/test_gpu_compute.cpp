#include <catch2/catch_test_macros.hpp>
#include <pulp/render/gpu_compute.hpp>
#include <pulp/render/gpu_surface.hpp>
#include <pulp/signal/fft.hpp>

#include <cmath>
#include <cstdio>
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
