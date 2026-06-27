#pragma once

#include <pulp/render/gpu_surface.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef PULP_BENCHMARK
#include <pulp/render/bench/perf_counters.hpp>
#endif

namespace pulp::render {

/// GPU compute pipeline for non-rendering workloads (e.g. spectral processing).
///
/// Experimental API for background GPU compute work. It operates on a Dawn
/// wgpu::Device shared with GpuSurface/SkiaSurface.
///
/// Design constraints:
/// - Never runs in the audio callback (upload/readback latency is too high)
/// - Operates on pre-allocated GPU buffers for batch processing
/// - Separate from SkSL rendering shaders — this is WebGPU compute, not fragment
///
/// Intended use: offline or background batch processing of audio data
/// (spectral analysis, batch convolution, spectrogram generation).
class GpuCompute {
public:
    virtual ~GpuCompute() = default;

    /// Initialize from an existing GpuSurface's device (device sharing).
    /// Returns false if compute pipelines cannot be created.
    virtual bool initialize_from_surface(GpuSurface& surface) = 0;

    /// Initialize standalone (creates its own device, no rendering surface).
    /// Useful for benchmarking and headless compute.
    virtual bool initialize_standalone() = 0;

    bool is_initialized() const { return initialized_; }

    // ── Compute operations ──────────────────────────────────────────────

    /// Compute magnitude spectrum from interleaved complex float pairs.
    /// Input:  complex_pairs — [re0, im0, re1, im1, ...] (2 * num_bins floats)
    /// Output: magnitudes    — [mag0, mag1, ...] (num_bins floats, linear scale)
    /// Returns false if GPU dispatch fails.
    virtual bool compute_magnitude(const float* complex_pairs, float* magnitudes,
                                   uint32_t num_bins) = 0;

    /// Element-wise complex multiply (frequency-domain convolution step).
    /// Inputs:  a, b — interleaved complex [re, im, re, im, ...] (2 * count floats each)
    /// Output:  result — interleaved complex product (2 * count floats)
    virtual bool complex_multiply(const float* a, const float* b, float* result,
                                  uint32_t count) = 0;

    /// Batch spectral magnitude: processes multiple FFT frames in one dispatch.
    /// Input:  frames of interleaved complex data, packed contiguously
    /// Output: frames of magnitude data, packed contiguously
    virtual bool batch_magnitude(const float* complex_frames, float* magnitude_frames,
                                 uint32_t bins_per_frame, uint32_t num_frames) = 0;

    // ── FFT ──────────────────────────────────────────────────────────────
    //
    // GPU radix-2 FFT (Stockham auto-sort, no bit-reversal pass). Operates on
    // interleaved complex data [re0, im0, re1, im1, ...] of length `n` complex
    // values (2*n floats in and out). `n` must be a power of two and small
    // enough that 2*n*sizeof(float) fits the device storage-buffer limit.
    // Not for the audio callback — these build/cache a per-`n` plan and block
    // on readback. Validated against a direct DFT and pulp::signal::Fft
    // (magnitude parity + round-trip identity); see test/test_gpu_compute.cpp.
    //
    // Thread-safety: like the rest of GpuCompute, a single instance is NOT
    // safe for concurrent use — call all methods from one thread (e.g. the
    // GPU worker). The plan/pipeline caches are unsynchronized by design.

    /// Forward DFT: X[k] = sum_n x[n] exp(-2*pi*i*k*n/N). Unnormalized.
    /// Returns false if `n` is not a power of two, exceeds the buffer limit,
    /// either pointer is null, or dispatch fails.
    virtual bool fft_forward(const float* complex_in, float* complex_out, uint32_t n) = 0;

    /// Inverse DFT, normalized by 1/N (so ifft(fft(x)) == x within tolerance).
    /// Returns false if `n` is not a power of two, exceeds the buffer limit,
    /// either pointer is null, or dispatch fails.
    virtual bool fft_inverse(const float* complex_in, float* complex_out, uint32_t n) = 0;

    // ── Device sharing verification ──────────────────────────────────────

    struct DeviceSharingReport {
        bool device_obtained = false;
        bool second_consumer_works = false;
        bool concurrent_submission_ok = false;
        bool memory_pressure_ok = false;
        std::string backend_name;     // "Metal", "D3D12", "Vulkan"
        std::string notes;
    };

    /// Test device sharing between Skia Graphite and compute pipelines.
    /// Creates render targets and compute buffers on the same device,
    /// submits from both, checks for corruption.
    virtual DeviceSharingReport verify_device_sharing(GpuSurface& surface) = 0;

    // ── Benchmarking ────────────────────────────────────────────────────

    struct BenchmarkResult {
        uint32_t num_elements = 0;
        double upload_us = 0;      // microseconds: CPU → GPU buffer
        double dispatch_us = 0;    // microseconds: compute shader execution
        double readback_us = 0;    // microseconds: GPU buffer → CPU
        double total_us = 0;       // upload + dispatch + readback
        double cpu_baseline_us = 0; // equivalent CPU operation for comparison
        bool gpu_faster = false;
    };

    /// Benchmark magnitude computation at various sizes.
    /// Returns results for each size in the vector.
    virtual std::vector<BenchmarkResult> benchmark_magnitude(
        const std::vector<uint32_t>& sizes, int iterations = 10) = 0;

    /// Benchmark complex multiply at various sizes.
    virtual std::vector<BenchmarkResult> benchmark_complex_multiply(
        const std::vector<uint32_t>& sizes, int iterations = 10) = 0;

    static std::unique_ptr<GpuCompute> create();

#ifdef PULP_BENCHMARK
    /// Install (or clear) the zero-copy benchmark perf-counter sink.
    /// The implementation accumulates upload/readback/dispatch times
    /// around Dawn `WriteBuffer`, `Submit`, and `MapAsync`+memcpy calls.
    /// Tooling-only; compiled out unless PULP_BENCHMARK is defined.
    virtual void set_bench_counters(bench::PerfCounters* counters) = 0;
#endif

protected:
    bool initialized_ = false;
};

} // namespace pulp::render
