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

    /// Forward FFT that also reports the TRUE GPU compute time (microseconds)
    /// of the transform passes, measured with compute-pass timestamp queries
    /// — i.e. excluding upload and the blocking readback that dominates
    /// wall-clock. Sets *gpu_compute_us to -1 when timestamp queries are
    /// unavailable (capabilities().timestamp_query == false) or N == 1.
    /// Diagnostic/benchmark path. Returns false on the same conditions as
    /// fft_forward.
    virtual bool fft_forward_timed(const float* complex_in, float* complex_out,
                                   uint32_t n, double* gpu_compute_us) = 0;

    // ── GPU-resident convolution ───────────────────────────────────────────
    //
    // Fused FFT convolution that keeps intermediates GPU-resident: forward FFT
    // → complex-multiply by a resident IR spectrum → inverse FFT (1/N
    // normalized), all encoded into ONE command buffer with ONE readback
    // (vs three readbacks for fft_forward + complex_multiply + fft_inverse).
    // Not real-time-safe (still blocks on the single readback) but the
    // amortizable building block toward a real-time GPU convolver.

    /// Build the resident convolution plan for size `n` (power of two) and
    /// upload `ir_spec` — the interleaved-complex spectrum of the zero-padded
    /// IR (length 2*n, e.g. produced by fft_forward of the padded IR). Call
    /// once before convolve(n). Returns false on invalid args / GPU failure.
    virtual bool prepare_convolution(uint32_t n, const float* ir_spec) = 0;

    /// Run the fused convolution for a prepared `n`. in/out are interleaved
    /// complex, length 2*n. Returns false if prepare_convolution(n) was not
    /// called, args are invalid, or dispatch fails.
    virtual bool convolve(const float* in_complex, float* out_complex, uint32_t n) = 0;

    /// Batched fused convolution: `batch` independent length-`n` blocks
    /// convolved with the same resident IR, in ONE submit with ONE readback —
    /// amortizing the dominant round-trip across all blocks. in/out hold
    /// `batch` back-to-back interleaved-complex blocks (length 2*n*batch).
    /// Returns false if prepare_convolution_batch(n, _, batch) was not called.
    virtual bool prepare_convolution_batch(uint32_t n, const float* ir_spec,
                                           uint32_t batch) = 0;
    virtual bool convolve_batch(const float* in_complex, float* out_complex,
                                uint32_t n, uint32_t batch) = 0;

    // ── Multi-IR convolution (one input → many distinct IRs → stereo) ───────
    //
    // The mass-parallel regime: convolve ONE input block against `num_ir`
    // DISTINCT IR spectra ("rooms"/"taps") in ONE submit, then reduce the
    // num_ir results to a stereo pair on the GPU with per-room pan weights —
    // so only one stereo block (2*n floats) is read back regardless of num_ir.
    // The forward FFT runs once and is shared across all rooms; each room gets
    // its own complex-multiply and inverse FFT, all batched. On the CPU this is
    // num_ir independent convolutions; on the GPU it is one batched job — the
    // structural win that scales past real time on the CPU while the GPU holds.
    // Not real-time-safe (blocks on the single readback); the amortizable
    // building block for a GPU multi-room convolution reverb.

    /// Build the multi-IR plan for size `n` (power of two) and upload
    /// `ir_specs` — `num_ir` back-to-back interleaved-complex IR spectra
    /// (length 2*n*num_ir). Call once before multi_convolve(n, num_ir).
    /// Returns false on invalid args / GPU failure.
    virtual bool prepare_multi_convolution(uint32_t n, const float* ir_specs,
                                           uint32_t num_ir) = 0;

    /// Run the multi-IR convolution for a prepared (n, num_ir). `in_complex` is
    /// one zero-padded interleaved-complex input block (length 2*n). `pan_l` /
    /// `pan_r` are per-room linear gains (length num_ir) applied in the GPU
    /// reduce. `out_lr` receives the stereo result: out_lr[0..n) = L,
    /// out_lr[n..2n) = R (length 2*n). Returns false if prepare_multi_convolution
    /// was not called for (n, num_ir), args are invalid, or dispatch fails.
    virtual bool multi_convolve(const float* in_complex, const float* pan_l,
                                const float* pan_r, float* out_lr, uint32_t n,
                                uint32_t num_ir) = 0;

    // ── Linear algebra ─────────────────────────────────────────────────────

    /// Dense matrix multiply C[M×N] = A[M×K] · B[K×N], all row-major f32.
    /// The foundational GPU primitive for neural inference (dense layers, LSTM
    /// gate matmuls) and matrix-heavy DSP. Not real-time-safe (uploads + blocks
    /// on readback). Returns false on invalid args / dispatch failure.
    virtual bool matmul(const float* a, const float* b, float* c,
                        uint32_t m, uint32_t k, uint32_t n) = 0;

    // ── Synthesis ──────────────────────────────────────────────────────────

    /// GPU additive synthesis: out[s] = Σ_p amp_p · sin(2π·freq_p·(t0+s)/sr +
    /// phase_p), one thread per output sample summing all partials. `partials`
    /// is num_partials × [freq(Hz), amp, phase(rad)] (3·num_partials floats).
    /// Thousands of partials parallelize cleanly — the basis for additive /
    /// spectral resynthesis. `t0_samples` is the absolute start sample (block
    /// continuity). Not real-time-safe. Returns false on invalid args.
    virtual bool additive_synth(const float* partials, float* out,
                                uint32_t num_partials, uint32_t num_samples,
                                float sample_rate, float t0_samples) = 0;

    /// GPU struck modal synthesis: out[s] = Σ_m amp_m·exp(-decay_m·t)·
    /// sin(2π·freq_m·t + phase_m), t=(t0+s)/sr. `modes` is num_modes ×
    /// [freq(Hz), amp, decay(1/s), phase(rad)] (4·num_modes floats) — a
    /// struck/plucked modal object (bell / string / body) as a bank of decaying
    /// resonant modes. Thousands of modes parallelize. Not real-time-safe.
    virtual bool modal_strike(const float* modes, float* out, uint32_t num_modes,
                              uint32_t num_samples, float sample_rate, float t0_samples) = 0;

    /// GPU granular synthesis ("grain cloud"): sum many Hann-windowed,
    /// pitch-shifted snippets of `source` into `out`, one GPU thread per output
    /// sample. `grains` is num_grains × [onset(sample), duration(samples),
    /// source_pos(sample), pitch(ratio), amp]. Tens of thousands of grains
    /// parallelize across samples. Not real-time-safe. Returns false on bad args.
    virtual bool granular_cloud(const float* grains, const float* source, float* out,
                                uint32_t num_grains, uint32_t source_len,
                                uint32_t num_samples) = 0;

    // ── Neural inference ────────────────────────────────────────────────────

    /// GPU dense (fully-connected) layer with tanh activation:
    /// out[j] = tanh(Σ_i W[j][i]·x[i] + b[j]), one thread per output neuron.
    /// W is row-major [out_dim × in_dim], b is [out_dim]. The core building
    /// block for GPU neural inference — NAM dense / output layers and LSTM gate
    /// evaluations are dense layers (the WaveNet path reuses the conv
    /// primitives). Not real-time-safe. Returns false on invalid args.
    virtual bool dense_tanh(const float* input, const float* weights, const float* bias,
                            float* output, uint32_t in_dim, uint32_t out_dim) = 0;

    // ── Capabilities ─────────────────────────────────────────────────────

    /// Runtime GPU capability/limit report for the compute device. Queryable
    /// after a successful initialize_*; `available` is false otherwise. Treat
    /// optional features (timestamp_query, shader_f16) and limits as runtime
    /// capabilities — select shader variants / sizes from these, never assume.
    struct CapabilityReport {
        bool available = false;
        std::string backend;        // "Metal" / "D3D12" / "Vulkan" / "shared" / ...
        std::string vendor;
        bool timestamp_query = false;   // compute-pass GPU timing available
        bool shader_f16 = false;        // half-precision in WGSL
        uint64_t max_storage_buffer_binding_size = 0;
        uint64_t max_buffer_size = 0;
        uint32_t max_compute_invocations_per_workgroup = 0;
        uint32_t max_compute_workgroup_size_x = 0;
        uint32_t max_compute_workgroup_storage_size = 0;
        // Largest power-of-two complex FFT this device's storage-buffer limit
        // admits, capped at the implementation maximum (0 if unavailable).
        uint32_t max_fft_size = 0;
    };

    /// Query the compute device's backend, optional features, and limits.
    virtual CapabilityReport capabilities() const = 0;

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
