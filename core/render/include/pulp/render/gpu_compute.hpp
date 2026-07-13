#pragma once

#include <pulp/render/gpu_surface.hpp>

#include <chrono>
#include <cstddef>
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
    ///
    /// Native only. It requests the adapter and device and then pumps the event
    /// queue once; in a browser those futures resolve only after control returns
    /// to the JS event loop, so there is no correct in-line spelling of it. The
    /// browser build fails this call explicitly rather than returning a
    /// mysterious false — use initialize_from_device() there.
    virtual bool initialize_standalone() = 0;

    /// Initialize by adopting an already-created WebGPU device. `wgpu_device` is
    /// an opaque `WGPUDevice`, the same handle style as
    /// GpuSurface::dawn_device_handle(). Acquires the device's queue, reads its
    /// limits/features, installs the uncaptured-error and device-lost callbacks,
    /// and builds the compute pipelines.
    ///
    /// This is the browser entry point. WebGPU is exposed on Window and
    /// DedicatedWorker only, so the worker's JS awaits
    /// navigator.gpu.requestAdapter()/requestDevice(), publishes the result as
    /// `Module.preinitializedWebGPUDevice`, and the C++ side takes it via
    /// `emscripten_webgpu_get_device()`. Every asynchronous step of device
    /// acquisition stays in JS: no Asyncify is used or needed.
    ///
    /// Returns false on a null handle or pipeline-creation failure.
    virtual bool initialize_from_device(void* wgpu_device) = 0;

    bool is_initialized() const { return initialized_; }

    /// True once the WebGPU device has been lost. Device loss is normal — a
    /// browser tab can lose its device at any time — so it is reported as state,
    /// not as an initialization failure. On loss every in-flight readback
    /// completes as `Failed` and `is_initialized()` goes false; the owner is
    /// expected to re-acquire a device and re-`prepare_*`.
    virtual bool device_lost() const = 0;

    /// Report that an ADOPTED device (initialize_from_device /
    /// initialize_from_surface) has been lost. A device-lost callback can only be
    /// installed on the device *descriptor*, i.e. at creation, so a device this
    /// object did not create cannot observe its own loss — its owner must say so.
    /// In the browser that owner is the worker's JS, which awaits `device.lost`.
    /// Idempotent. A device created by initialize_standalone() observes loss on
    /// its own and needs no call.
    virtual void notify_device_lost() = 0;

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

    /// Same as multi_convolve, but also reports the true GPU-busy time of the
    /// fused pass sequence (forward FFT → broadcast multiply → batched inverse
    /// FFT → pan-combine) in microseconds via `gpu_compute_us`, measured with a
    /// compute-pass timestamp query — excluding the input upload and the
    /// blocking readback. Sets `*gpu_compute_us = -1` when timing is
    /// unavailable (capabilities().timestamp_query == false). Diagnostic /
    /// roofline path: comparing this against the call's wall time separates a
    /// GPU-busy (bandwidth/compute-bound) kernel from an upload/readback-bound
    /// one. Mirrors fft_forward_timed.
    virtual bool multi_convolve_timed(const float* in_complex, const float* pan_l,
                                      const float* pan_r, float* out_lr, uint32_t n,
                                      uint32_t num_ir, double* gpu_compute_us) = 0;
    // ── Partitioned-FDL multi-convolution (roofline #1) ──────────────────────
    //
    // The same one-input → many-IRs → stereo result, but with the IR split into
    // `num_part` block-size partitions so the FFT stays at the small fixed block
    // size (n = 2*block) instead of growing to next_pow2(block + IR_len). The
    // convolution becomes a sum of spectral products over a delay line of recent
    // input spectra — the algorithm signal::PartitionedConvolver uses on the CPU.
    // This trades one giant FFT for P small-FFT-equivalent MAC rows, which is the
    // removable waste multi_convolve_timed measured. Not real-time-safe (blocks
    // on the readback); the amortizable building block for a real-time GPU
    // multi-room convolver.

    /// Build the partitioned-FDL plan for `n` = 2*block. `ir_part_specs` holds
    /// `num_ir * num_part` back-to-back interleaved-complex partition spectra
    /// (each length 2*n): room r partition p at offset (r*num_part + p)*2n. Each
    /// partition spectrum is the forward FFT of that IR block zero-padded to n.
    /// Call once before multi_fdl_convolve(n, num_ir). Returns false on invalid
    /// args or if the resident IR spectra exceed the storage-buffer limit.
    virtual bool prepare_multi_fdl(uint32_t n, const float* ir_part_specs,
                                   uint32_t num_ir, uint32_t num_part) = 0;

    /// Run one block of partitioned-FDL convolution for a prepared (n, num_ir).
    /// `in_block` is `block` (= n/2) real input samples; `pan_l`/`pan_r` are
    /// per-room linear gains (length num_ir). `out_block` receives the stereo
    /// output block: out_block[0..block) = L, out_block[block..2*block) = R
    /// (length 2*block = n). The plan carries the delay line and the previous
    /// block across calls. Returns false if prepare_multi_fdl was not called for
    /// (n, num_ir), args are invalid, or dispatch fails.
    virtual bool multi_fdl_convolve(const float* in_block, const float* pan_l,
                                    const float* pan_r, float* out_block,
                                    uint32_t n, uint32_t num_ir) = 0;

    // ── Multi-layer spectral stack (N resident layer-spectra → one frame) ───
    //
    // The spectral analogue of the multi-IR regime: keep `num_layers` frozen
    // spectral frames (each a magnitude array + a persistent phase array)
    // RESIDENT on the GPU, and per hop process EVERY layer's bins in ONE batched
    // submit — advance each layer's phase by its nominal per-hop frequency, blur
    // (smear) each layer's magnitude across frequency, then weighted-sum all
    // layers into one combined spectrum and inverse-FFT it to a single real
    // frame. Only that one frame (2n floats) is read back per hop, regardless of
    // how many layers there are. On the CPU this is num_layers × (per-bin smear
    // + transcendentals), serial; on the GPU it is one batch parallel across all
    // n bins — the structural win that scales past real time on the CPU at high
    // num_layers while the GPU holds. Not real-time-safe (blocks on the single
    // readback); driven from the GPU audio worker / offline.

    /// Build the spectral-stack plan for FFT size `n` (power of two), STFT hop
    /// `hop`, and `num_layers` resident layer-spectra. `hop` fixes the per-bin
    /// phase advance (2*pi*k*hop/n) applied each render. Allocates the resident
    /// magnitude / phase buffers (zeroed: every layer starts inactive/silent)
    /// and the combine + inverse-FFT pipeline. Call once before
    /// spectral_stack_render. Returns false on invalid args, if the resident
    /// buffers would exceed the device storage-buffer binding limit, or on GPU
    /// failure.
    virtual bool prepare_spectral_stack(uint32_t n, uint32_t hop,
                                        uint32_t num_layers) = 0;

    /// Upload one captured layer's magnitude (`mag`, n reals) and initial phase
    /// (`phase`, n reals) into the resident plan for (`n`, `num_layers`),
    /// activating it. The phase then persists and advances on the GPU each
    /// render. Returns false if prepare_spectral_stack(n, num_layers) was not
    /// called, args are invalid, or dispatch fails.
    virtual bool spectral_stack_set_layer(uint32_t layer, const float* mag,
                                          const float* phase, uint32_t n,
                                          uint32_t num_layers) = 0;

    /// Advance + smear + weighted-sum all resident layers and inverse-FFT to one
    /// real frame. `layer_weights` (length num_layers) scales each layer (drive
    /// it from a Morph control). `smear` (0..1) circular box-blurs each layer's
    /// magnitude across frequency; `jitter` (0..1) adds conjugate-symmetric phase
    /// wander seeded by `rng_seed`. The phase advance is conjugate-symmetric so
    /// `frame_out` (n reals) is real. Returns false if the plan was not prepared
    /// for (n, num_layers), args are invalid, or dispatch fails.
    virtual bool spectral_stack_render(const float* layer_weights,
                                       uint32_t num_layers, float smear,
                                       float jitter, uint32_t rng_seed,
                                       float* frame_out, uint32_t n) = 0;

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

    // ── Fused conv-stack inference (WaveNet-style neural amp) ───────────────
    //
    // A stack of gated, dilated, causal 1-D conv layers — the architecture
    // behind neural-amp captures. The whole block's samples are computed in
    // PARALLEL (the network is feedforward: output[t] depends on past inputs,
    // not past outputs) and the entire forward pass runs as successive compute
    // passes in ONE submit with device-resident activations — so the CPU<->GPU
    // round-trip is paid ONCE per block, not per layer or per sample. That is
    // the regime where GPU neural inference beats the CPU, and the win grows
    // with model size (big captures the CPU can't run in real time stay smooth
    // on the GPU). Per-sample/per-layer dispatch loses badly and is never used.
    //
    // Per-layer weight layout (row-major f32, concatenated for all layers, then
    // the two global blocks): conv W [2C*C*K] + bias [2C], residual W [C*C] +
    // bias [C], skip W [C*C] + bias [C]; then once: input W [C] + bias [C], head
    // W [C] + bias [1]. `channels` = C, `kernel` = K, `dilations` has one entry
    // per layer. Not real-time-safe (blocks on readback); run on the worker.

    /// Build the conv-stack plan: upload weights, allocate the per-layer
    /// device-resident activation buffers for `block_size` samples, and compile
    /// the fused pass pipeline. Returns false on invalid args, if the buffers
    /// exceed a device limit, or on GPU failure. Call once before
    /// conv_stack_forward(block_size).
    virtual bool prepare_conv_stack(uint32_t channels, uint32_t kernel,
                                    const uint32_t* dilations, uint32_t num_layers,
                                    const float* weights, uint32_t weights_len,
                                    uint32_t block_size, float head_scale) = 0;

    /// Run the fused forward for one mono block (`block_size` samples in/out):
    /// input projection, the dilated gated conv layers, and the linear head, all
    /// in one submit. Carries each layer's dilation history across calls (the
    /// activation window slides every block), so streaming blocks are continuous
    /// and bit-for-bit match the CPU reference run sample-continuously. Returns
    /// false if the plan was not prepared for `block_size`.
    virtual bool conv_stack_forward(const float* in_block, float* out_block,
                                    uint32_t block_size) = 0;

    // ── Fused conditioned WaveNet inference (gated dilated conv sequence) ────
    //
    // A conditioned WaveNet forward — a sequence of layer-arrays of gated,
    // dilated, causal 1-D conv layers (van den Oord et al.) — reproduced
    // GPU-resident and block-parallel. Unlike conv_stack (a single gated layer
    // stack), this models the full shape: a sequence of layer-arrays, each with
    // an input rechannel (1x1), dilated causal conv layers that mix in a
    // separate condition signal (here the raw mono input), Tanh (or gated
    // tanh·sigmoid) activation, a 1x1 residual, a per-array head accumulator,
    // and a head rechannel — with the layer output and the head accumulator
    // chained array→array. The whole block's samples compute in PARALLEL (the
    // net is feedforward), every pass encodes into ONE submit over
    // device-resident activations, and only the final mono block is read back —
    // the round-trip is paid once per block. Dilation history slides across
    // calls so streaming blocks are continuous. Not real-time-safe (blocks on
    // the readback); run on the GPU worker / offline. It is the GPU counterpart
    // to a streaming CPU WaveNet and drives neural waveshaping / amp inference.
    //
    // Weight layout (flat f32 blob). Per layer-array, walking the blob:
    // rechannel W [channels*input_size] (no bias); then per layer: conv W
    // [Z*channels*kernel] + bias [Z], input_mixin W [Z*condition_size] (no bias),
    // layer1x1 W [channels*channels] + bias [channels]; then head_rechannel W
    // [head_size*channels] (+ bias [head_size] if head_bias). After all arrays:
    // one trailing head_scale scalar. Z = gated ? 2*channels : channels.
    // Activation is Tanh; the gate is sigmoid.

    /// One WaveNet layer-array's static shape. `dilations` points to `num_layers`
    /// dilation factors. `condition_size` must be 1 (a mono condition signal).
    /// `gated` / `head_bias` are 0/1 flags.
    struct WavenetLayerArraySpec {
        uint32_t input_size = 1;
        uint32_t condition_size = 1;
        uint32_t channels = 1;
        uint32_t kernel = 1;
        uint32_t head_size = 1;
        uint32_t gated = 0;
        uint32_t head_bias = 0;
        const uint32_t* dilations = nullptr;
        uint32_t num_layers = 0;
    };

    /// Build the WaveNet plan: upload `weights` (flat, length `weights_len`,
    /// `head_scale` is the trailing scalar passed separately here), allocate the
    /// per-array/per-layer device-resident activation + head buffers for
    /// `block_size` samples, and compile the fused passes. Returns false on
    /// invalid args (channels > 64, condition_size != 1, an array-chaining shape
    /// mismatch, a weight-count mismatch) or GPU failure. Call once before
    /// wavenet_forward(block_size).
    ///
    /// `instance` selects an independent plan slot on THIS device: two instances
    /// (e.g. stereo channels) prepared with the same `block_size` get separate
    /// device-resident buffers + dilation history but share the device, queue, and
    /// compiled pipelines — so a stereo plugin needs one device, not one per
    /// channel. Instances are identified by `(block_size, instance)`; the default 0
    /// preserves the single-stream behaviour.
    virtual bool prepare_wavenet(const WavenetLayerArraySpec* arrays, uint32_t num_arrays,
                                 const float* weights, uint32_t weights_len,
                                 uint32_t block_size, float head_scale,
                                 uint32_t instance = 0) = 0;

    /// Run the fused WaveNet forward for one mono block (`block_size` in/out): the
    /// per-array rechannel, dilated conv layers (+ condition mixin, activation,
    /// residual, head accumulate), head rechannels, and the final head_scale, all
    /// in one submit. Carries each layer's dilation history across calls so
    /// streaming blocks match the CPU reference run sample-continuously. Returns
    /// false if prepare_wavenet was not called for `(block_size, instance)`.
    /// `instance` selects the plan slot prepared above (default 0).
    virtual bool wavenet_forward(const float* in_block, float* out_block,
                                 uint32_t block_size, uint32_t instance = 0) = 0;

    // ── Asynchronous readback (compute_magnitude + convolve_batch) ──────────
    //
    // SCOPE FIRST, so nobody reads the contract below as a general promise:
    // exactly TWO operations have an async variant — compute_magnitude_async()
    // and convolve_batch_async(). fft_forward, convolve, multi_convolve,
    // multi_fdl_convolve, spectral_stack_render, matmul, conv_stack_forward and
    // wavenet_forward ALL still block. This is not a non-blocking mode for
    // GpuCompute; it is two non-blocking operations plus the completion
    // machinery the rest of the ops reuse when they are converted.
    //
    // Every blocking operation pumps the device event queue until the GPU map
    // resolves. That is only viable where the caller owns a thread it may stall
    // (the GPU worker, an offline render). On a single-threaded event loop it is
    // fatal — spinning starves the very loop that would resolve the map. The
    // async path submits the dispatch, returns immediately, and hands results
    // back from poll_readbacks(), so the caller decides when completions run.
    //
    // In the browser (Emscripten/emdawnwebgpu) the blocking ops are compiled out
    // or hard-fail: the async path is the ONLY way to get bytes back from the
    // GPU there.
    //
    // Contract (of the async path):
    // - An accepted request (non-zero id) completes EXACTLY ONCE, from
    //   poll_readbacks() or from ~GpuCompute(). It never hangs: a request whose
    //   map has not resolved by its deadline completes as `Expired`, which the
    //   caller routes to its miss policy (gpu_audio::MissPolicy) exactly as it
    //   would route a late block.
    // - Completions fire in submission order.
    // - `dest` must stay valid until the completion fires. It is written only
    //   on `Success`; a map that lands after an `Expired` completion is
    //   discarded without touching `dest`.
    // - Not real-time-safe: submit and poll from the GPU worker, never from the
    //   audio callback. The audio thread's contract (never wait on, allocate
    //   for, or synchronize with the GPU) is upheld by the transport, not here.

    enum class ReadbackStatus {
        Success,   ///< `dest` holds the full payload.
        Expired,   ///< Deadline passed before the map resolved; `dest` untouched.
        Failed,    ///< Map error, or the device went away before the map landed.
    };

    struct ReadbackResult {
        uint64_t id = 0;                                  ///< request id
        ReadbackStatus status = ReadbackStatus::Failed;
        uint32_t bytes = 0;                               ///< written to dest (0 unless Success)
    };

    using ReadbackCallback = std::function<void(const ReadbackResult&)>;

    /// Non-blocking counterpart of compute_magnitude(). Submits the dispatch and
    /// returns a request id; `magnitudes` receives `num_bins` floats when the
    /// completion reports Success. `deadline` is measured from this call.
    /// Returns 0 if the request was rejected (not initialized, invalid args, no
    /// callback) — the callback does NOT fire in that case.
    virtual uint64_t compute_magnitude_async(const float* complex_pairs,
                                             float* magnitudes, uint32_t num_bins,
                                             std::chrono::microseconds deadline,
                                             ReadbackCallback on_complete) = 0;

    /// Non-blocking counterpart of convolve_batch(). Same plan requirement
    /// (prepare_convolution_batch(n, ir_spec, batch) first) and same result:
    /// `out_complex` receives `batch * n * 2` floats, already scaled by the 1/n
    /// inverse-FFT factor, when the completion reports Success. `deadline` is
    /// measured from this call. Returns 0 if the request was rejected (not
    /// initialized, no plan for `n`, batch mismatch, null pointers, no callback,
    /// or the in-flight cap is reached) — the callback does NOT fire in that
    /// case, and the caller counts a miss.
    ///
    /// Multiple blocks may be in flight at once. The plan's compute buffers are
    /// SHARED across them, which is safe because the WebGPU queue executes
    /// submissions in order: submit N's copy into its readback buffer runs before
    /// submit N+1 overwrites the plan's input buffer. Only the mapped readback
    /// buffer is per-submit (one per request, from the staging pool) — a shared
    /// one is precisely what forces a blocking design.
    virtual uint64_t convolve_batch_async(const float* in_complex, float* out_complex,
                                          uint32_t n, uint32_t batch,
                                          std::chrono::microseconds deadline,
                                          ReadbackCallback on_complete) = 0;

    /// Pump the device event queue once and fire the completions that are ready
    /// or past their deadline. Never blocks. Returns the number of requests
    /// still in flight. Re-entrant calls (from inside a completion) are a no-op.
    virtual std::size_t poll_readbacks() = 0;

    /// Requests submitted whose completion has not fired yet.
    virtual std::size_t readbacks_in_flight() const = 0;

    /// Cap on concurrently in-flight async readbacks (default 4). Admission
    /// control, not a hint: at the cap, an async submit is REJECTED (returns 0)
    /// instead of queueing. Without it a stalled or throttled GPU — a
    /// backgrounded browser tab is the ordinary case — would grow staging memory
    /// by one buffer per audio block forever. A rejected submit is a miss, which
    /// the caller already knows how to route (MissPolicy).
    virtual void set_max_readbacks_in_flight(std::size_t max_in_flight) = 0;

    /// Counters for the async path, incremented where the work actually happens:
    /// `submits` at the queue submit, `map_resolves` when the GPU map callback
    /// fires, `expired`/`failed` when a request completes with that status.
    /// `submits - (map_resolves + expired + failed)` is not a meaningful
    /// identity — a resolved map can still complete Expired if its deadline had
    /// already passed.
    struct AsyncStats {
        uint64_t submits = 0;
        uint64_t map_resolves = 0;
        uint64_t expired = 0;
        uint64_t failed = 0;
    };

    virtual AsyncStats async_stats() const = 0;

    // ── Kernel-source seam (diagnostic) ──────────────────────────────────
    //
    // A named compute pipeline's WGSL is readable, and replaceable before the
    // pipeline is built. This exists for ONE purpose: to demonstrate that the
    // WGSL text is causally upstream of the audio samples — mutate the kernel,
    // hear/measure the mutation in the output. It is what makes the browser
    // GPU-audio claim checkable rather than asserted. It is a diagnostic seam,
    // not a plugin/effect API: there is no validation of the replacement beyond
    // WGSL compilation, and a bad override simply fails pipeline creation.

    /// Built-in WGSL for a named pipeline (e.g. "conv_bmul", "fft_stockham",
    /// "magnitude"). Returns nullptr for an unknown label. Pipeline labels are
    /// the ones passed to the internal create_pipeline().
    virtual const char* kernel_source(const char* label) const = 0;

    /// Replace the WGSL of a named pipeline. MUST be called before the pipelines
    /// are built (i.e. before initialize_*), because pipelines are compiled once
    /// at initialization. Returns false for an unknown label, null WGSL, or a
    /// call made after initialization — a late override is refused rather than
    /// silently ignored.
    virtual bool override_kernel_source(const char* label, const char* wgsl) = 0;

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
