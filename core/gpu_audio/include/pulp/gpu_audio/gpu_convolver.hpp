#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <pulp/gpu_audio/gpu_audio_node.hpp>
#include <pulp/gpu_audio/gpu_audio_status.hpp>
#include <pulp/signal/convolver.hpp>
#include <pulp/signal/fixed_ir_convolver.hpp>

namespace pulp::render {
class GpuCompute;
}

namespace pulp::gpu_audio {

/// Opt-in diagnostics for the shared-I/O path.  The producer side records
/// fixed-size identities and terminal dispositions into bounded SPSC queues;
/// it never calls Perfetto, allocates, locks, or waits.  Configure this before
/// prepare(). The transport worker owns the queue drain and the final close
/// drains any records that remain.
struct GpuConvolverTraceConfig {
    bool enabled = false;
    bool capture_admissions = false;
    bool capture_callback_timing = false;
    std::uint32_t success_stride = 1;
};

/// Circuit breaker for the staged GPU readback. After `trip_after_misses`
/// consecutive blocks whose GPU result is missing, the worker stops submitting
/// and runs the CPU convolution for every block, probing the GPU once every
/// `probe_interval_blocks`. A probe that lands on time puts the GPU on
/// probation; `recover_after_hits` consecutive on-time results return the node
/// to GPU output, and any miss on probation trips it again. All three must be
/// non-zero. Configure before prepare().
struct GpuConvolverStagedBreaker {
    std::uint32_t trip_after_misses = 8;
    std::uint32_t probe_interval_blocks = 32;
    std::uint32_t recover_after_hits = 4;
};

class GpuConvolver;

namespace detail {
struct RealtimeGpuHealth;
struct SharedIoCompletionNotifier;
struct GpuConvolverTestAccess;
struct SharedIoProviderIdentity;
struct GpuConvolverTrialConfig;
struct SharedIoTraceRecord;
struct SharedIoTraceAdmission;
class StagedAsyncTrialState;
bool configure_gpu_convolver_trial(GpuConvolver&, const GpuConvolverTrialConfig&) noexcept;
bool drain_gpu_convolver_trial_records(GpuConvolver&, std::vector<SharedIoTraceRecord>&) noexcept;
bool drain_gpu_convolver_trial_admissions(GpuConvolver&,
                                          std::vector<SharedIoTraceAdmission>&) noexcept;
std::uint64_t gpu_convolver_trial_engine_id(const GpuConvolver&) noexcept;
} // namespace detail

/// First real GPU audio node: FFT-based (overlap-add) convolution of the input
/// with a fixed impulse response, computed on the GPU via render::GpuCompute on
/// the transport's non-RT worker.
///
/// Each block issues ONE batched GPU convolution — forward FFT, complex
/// multiply by the resident IR spectrum, and inverse FFT for every channel,
/// fused into a single submit with a single readback. The per-call GPU
/// round-trip (~0.5 ms of map latency) is therefore paid once per block rather
/// than once per channel.
///
/// Staged readback (Auto / StagedOnly). The worker never waits on the GPU: it
/// submits block N with `GpuCompute::convolve_batch_async` and returns, and the
/// result counts only if it has landed by the time block N is due,
/// latency_blocks - 1 calls later (descriptor().worker_pipeline_blocks; 1 at
/// the default lead of 2). That pipeline sits inside the reported latency, so
/// total latency and PDC are unchanged. A late result is discarded.
/// Every block's input lands in exactly one place: the GPU overlap-add carry
/// when its result is on time, otherwise the worker's zero-latency CPU
/// convolution. The output is the carry plus whatever the CPU convolution still
/// owes, so a late, failed, non-finite or device-lost block is the exact
/// continuation, never silence. The CPU convolution runs only for a missed
/// block and the IR-length tail after it; a GPU hit costs none. A run of
/// misses trips the breaker (GpuConvolverStagedBreaker): the worker stops
/// submitting, runs on the CPU, and probes the GPU periodically until it is on
/// time again. A node prepared with a one-block lead cannot pipeline and waits,
/// bounded, inside the block instead.
///
/// Lifetime: while a GpuAudioTransport hosts a pipelining GpuConvolver
/// (descriptor().worker_pipeline_blocks > 0), the transport's release() opts
/// the node back out of pipelining. A node destroyed first detaches itself
/// from the transport, so release() then touches nothing; a non-pipelining
/// node is never touched by release(). Destroying it first is safe only once
/// neither pump() nor process() runs on its transport: the audio callback's
/// process() calls the node's CPU fallback, and a transport running its own
/// worker (Config::run_worker_thread) pumps the node until release() joins it.
/// Otherwise release that transport before destroying the node.
///
/// CPU fallback (MissPolicy::CpuFallback). The fallback is a zero-latency
/// `signal::FixedIrConvolver` per channel that the transport keeps
/// CONTINUOUSLY fed via prime_fallback() — one direct-FIR or partitioned block
/// each audio block, not only on misses. A fallback fed only on misses would
/// have gaps in its history, so a substituted block would not continue the
/// convolution (its IR tail energy from the preceding audio would be missing):
/// an audible discontinuity, not merely lower precision. Because the priming
/// path is zero-latency but the transport reports `latency_blocks` of PDC for
/// the GPU pipeline, the fallback's output is pushed
/// through a matching `latency_blocks`-deep delay so a miss substitute equals the
/// wet block the worker ring would have returned (input(t-L) convolved) — the
/// seam is sample-aligned, never a dry glitch and never a block early/late.
///
/// `process_block` still waits (bounded) on the GPU readback, which is why it
/// runs on the transport's non-RT worker and never on the audio thread. Live use
/// at small block sizes should still prefer the CPU fallback: a prepared CPU
/// convolution costs microseconds, and no amount of batching beats a map
/// round-trip for a single stereo pair.
class GpuConvolver : public GpuAudioNode {
  public:
    /// Host-thread selection for the provider used during the next prepare().
    /// Auto preserves staged/CPU execution even when shared support is compiled,
    /// using the deadline-bounded staged readback described above. StagedOnly
    /// is the same staged path, named explicitly. SharedRequired explicitly opts
    /// into the authenticated shared Dawn path. StagedBlocking keeps the earlier
    /// unbounded `GpuCompute::convolve_batch` readback for callers that depend
    /// on its timing; it still fills failed or device-lost blocks from the CPU
    /// continuation. StagedBlocking is a transition escape hatch and is planned
    /// for removal in the next minor release. Selection is immutable while
    /// prepared and never changes from the audio callback.
    enum class ProviderPolicy : std::uint8_t {
        Auto,
        StagedOnly,
        SharedRequired,
        StagedBlocking,
    };

    /// Default worker/round-trip latency, in host blocks. A constructed node may
    /// select another positive lead for a controlled campaign; the selected value
    /// is reported as PDC and shared by the bridge and CPU fallback.
    static constexpr uint32_t kLatencyBlocks = 2;
    static constexpr uint32_t kMaxLatencyBlocks = 64;
    // Physical shared-I/O provider capacity, independent of logical lead.
    // Diagnostic receipts use this value instead of a stale literal.
    static constexpr uint32_t kSharedIoSlots = 16;

    GpuConvolver(uint32_t channels, uint32_t block_size, uint32_t sample_rate,
                 std::vector<float> impulse_response);
    GpuConvolver(uint32_t channels, uint32_t block_size, uint32_t sample_rate,
                 std::vector<float> impulse_response, uint32_t latency_blocks);
    ~GpuConvolver() override;

    /// Select the provider policy for the next prepare(). Returns false when
    /// called after preparation; callers must quiesce and release the node
    /// before selecting a new policy. SharedRequired fails preparation unless
    /// the exact authenticated shared provider is available.
    bool set_provider_policy(ProviderPolicy policy) noexcept;
    ProviderPolicy provider_policy() const noexcept {
        return provider_policy_;
    }

    /// Enable bounded shared-I/O tracing for the next prepare().  This is a
    /// host-thread operation and fails after preparation. The transport worker
    /// remains the sole live queue consumer; final close drains any remainder.
    bool configure_trace(const GpuConvolverTraceConfig& config) noexcept;

    /// Set the staged-path circuit breaker for the next prepare(). Returns false
    /// while prepared or for a zero field.
    bool configure_staged_breaker(const GpuConvolverStagedBreaker& config) noexcept;
    GpuConvolverStagedBreaker staged_breaker() const noexcept;

    GpuAudioNodeDescriptor descriptor() const override;
    bool prepare() override;

    bool cpu_fallback_prepared() const noexcept override {
        return prepared_;
    }
    void process_block(const audio::BufferView<const float>& input,
                       audio::BufferView<float>& output, uint32_t n) override;

    /// RT-safe. Feed the continuously-fed CPU fallback one block so its history
    /// stays current, and stage the latency-aligned substitute for a possible
    /// miss this block. Called by the transport on every block (hit or miss).
    void prime_fallback(const audio::BufferView<const float>& input, uint32_t n) noexcept override {
        if (!prepared_ || n != block_ || fallback_delay_blocks_ == 0)
            return;
        float* slot_base =
            fb_delay_.data() + static_cast<std::size_t>(fb_delay_idx_) * block_ * channels_;
        for (uint32_t ch = 0; ch < channels_ && ch < fallback_.size(); ++ch) {
            float* slot = slot_base + static_cast<std::size_t>(ch) * block_;
            float* due = fb_out_.data() + static_cast<std::size_t>(ch) * block_;
            // The slot currently holds conv(input(t-L)) — this block's due
            // substitute. Stash it, then overwrite with conv(input(t)).
            std::copy_n(slot, block_, due);
            if (ch >= input.num_channels()) {
                std::fill_n(slot, block_, 0.0f);
                continue;
            }
            const float* x = input.channel_ptr(ch);
            // Guard the fallback against a non-finite input block too: without it
            // a single NaN would poison the convolver's own history (and the
            // delay ring) for the whole IR tail, not just one block.
            bool finite = true;
            for (uint32_t i = 0; i < n; ++i) {
                if (!std::isfinite(x[i])) {
                    finite = false;
                    break;
                }
            }
            if (!finite) {
                fallback_[ch].reset();
                std::fill_n(slot, block_, 0.0f);
            } else {
                fallback_[ch].process(x, slot, static_cast<std::size_t>(n));
            }
        }
        fb_delay_idx_ = (fb_delay_idx_ + 1) % fallback_delay_blocks_;
    }

    /// RT-safe miss substitute: emit the latency-aligned block that
    /// prime_fallback() already computed for this timeline slot.
    void process_cpu_fallback(const audio::BufferView<const float>& /*input*/,
                              audio::BufferView<float>& output, uint32_t n) noexcept override {
        const uint32_t out_ch = static_cast<uint32_t>(output.num_channels());
        for (uint32_t ch = 0; ch < out_ch; ++ch)
            std::fill_n(output.channel_ptr(ch), n, 0.0f);
        if (!prepared_ || n != block_)
            return;
        const uint32_t ch_count = std::min<uint32_t>(channels_, out_ch);
        for (uint32_t ch = 0; ch < ch_count; ++ch) {
            const float* due = fb_out_.data() + static_cast<std::size_t>(ch) * block_;
            std::copy_n(due, n, output.channel_ptr(ch));
        }
    }

    /// True if the GPU compute device initialized (process_block uses the GPU).
    bool gpu_available() const;
    /// The live native compute backend ("Metal"/"D3D12"/"Vulkan"), or "" if CPU-only.
    /// On the shared path this names Dawn's underlying Metal backend; use the
    /// transport capability report when the provider identity itself matters.
    std::string backend() const;
    uint32_t fft_size() const {
        return fft_size_;
    }

    /// Allocation-free provider-neutral lifecycle projection, safe to call
    /// from a host or UI thread while a transport runs this node (not
    /// concurrently with prepare()). The authenticated shared path reports
    /// the health its transport last sampled after a service step: a fenced
    /// session is `Degraded`, a lost provider is `Unavailable`, each with the
    /// first latched recovery reason. The staged path reports what it can
    /// prove: provider and engine are `Unknown` while the staged GpuCompute
    /// device is live, the engine is `Cpu` when no device initialized, and a
    /// lost device is `Unavailable` with `ProviderLost`.
    GpuAudioStatus status_snapshot() const noexcept override;
    bool bind_realtime_path(detail::RealtimeGpuNodeBinding& binding) noexcept override;
    /// First latched recovery cause, or None. Same snapshot as status_snapshot().
    GpuAudioRecoveryReason recovery_reason() const noexcept;
    /// True before preparation, or once GPU delivery was fenced or its
    /// provider lost. A diagnostic snapshot, not a promise about the next block.
    bool fenced() const noexcept;

  protected:
    /// Allocate + load the CPU fallback machinery (both the RT miss fallback and
    /// the no-GPU worker fallback) and the latency-alignment delay ring. Shared
    /// by the render and no-render prepare() paths so they can never drift.
    void init_fallback() {
        fallback_.clear();
        worker_fallback_.clear();
        fallback_.resize(channels_);
        worker_fallback_.resize(channels_);
        for (uint32_t ch = 0; ch < channels_; ++ch) {
            fallback_[ch].load_ir(ir_.data(), ir_.size(), block_);
            worker_fallback_[ch].load_ir(ir_.data(), ir_.size(), block_);
        }
        fallback_delay_blocks_ = latency_blocks_;
        fb_delay_idx_ = 0;
        fb_delay_.assign(static_cast<std::size_t>(fallback_delay_blocks_) * block_ * channels_,
                         0.0f);
        fb_out_.assign(static_cast<std::size_t>(block_) * channels_, 0.0f);
    }

    /// No-GPU worker path: run the (continuously-fed) worker fallback for every
    /// prepared channel into `output`, zeroing anything else. Both selected
    /// engines are zero-latency, so the fallback matches the GPU path's
    /// algorithmic timing.
    ///
    /// This is the SOLE audio path when the device fails to init (the `!gpu_`
    /// branch of process_block) and in the no-render build, so it carries the same
    /// non-finite-input guard as prime_fallback(): a single NaN/Inf would otherwise
    /// poison worker_fallback_'s history for the whole IR tail (the IR length,
    /// plus one block on the FFT engine). On a poisoned block we reset the
    /// convolver and emit silence for that channel; it self-heals on the next
    /// finite block instead of dying for the tail.
    void render_worker_fallback(const audio::BufferView<const float>& input,
                                audio::BufferView<float>& output, uint32_t n) noexcept {
        const uint32_t out_ch = static_cast<uint32_t>(output.num_channels());
        for (uint32_t ch = 0; ch < out_ch; ++ch)
            std::fill_n(output.channel_ptr(ch), n, 0.0f);
        if (!prepared_)
            return;
        const uint32_t ch_count = std::min<uint32_t>(channels_, out_ch);
        for (uint32_t ch = 0; ch < ch_count && ch < worker_fallback_.size(); ++ch) {
            if (ch >= input.num_channels())
                continue;
            const float* x = input.channel_ptr(ch);
            bool finite = true;
            for (uint32_t i = 0; i < n; ++i) {
                if (!std::isfinite(x[i])) {
                    finite = false;
                    break;
                }
            }
            if (!finite) {
                worker_fallback_[ch].reset(); // output stays the pre-cleared silence
                continue;
            }
            worker_fallback_[ch].process(x, output.channel_ptr(ch), static_cast<std::size_t>(n));
        }
    }

    static std::uint8_t process_realtime_shared_io(void* self,
                                                   const audio::BufferView<const float>& input,
                                                   audio::BufferView<float>& output,
                                                   std::uint32_t n, std::uint64_t sequence,
                                                   bool input_valid,
                                                   std::uint64_t callback_start_ns) noexcept;
    static std::uint32_t service_realtime_shared_io(void* self, std::uint64_t now_ns) noexcept;
    static bool fence_realtime_shared_io(void* self) noexcept;
    static void complete_realtime_shared_io(void*, std::uint64_t, std::uint8_t,
                                            std::uint64_t callback_end_ns,
                                            std::uint64_t result_visible_ns) noexcept;
    static std::uint64_t next_realtime_shared_io_sequence(void*) noexcept;
    // Serialized non-RT (transport worker or quiescent host). Also republishes
    // the sample for status_snapshot() readers on other threads.
    static bool realtime_shared_io_health(const void* self, detail::RealtimeGpuHealth&) noexcept;
    // Host only, while no service step runs: route the shared session's
    // provider completion events to the transport worker.
    static bool set_realtime_shared_io_notifier(void* self,
                                                detail::SharedIoCompletionNotifier) noexcept;
    // Host-thread safe: the staged GpuCompute loss query is atomic.
    GpuAudioRecoveryReason staged_recovery_reason() const noexcept;
    bool has_realtime_shared_io() const noexcept;
    detail::SharedIoProviderIdentity provider_identity_for_diagnostics() const noexcept;

    struct StagedReadbackState;
    // One-block-lead fallback: submit the packed block and wait, bounded, for
    // its completion into `dest`. True only for a Success readback.
    bool wait_staged_readback(StagedReadbackState& staged, float* dest);
    // Pipelined path: submit the packed block into slot `slot_index` without
    // waiting; it is judged when due, pipeline_blocks calls later.
    void submit_staged_readback(StagedReadbackState& staged, std::size_t slot_index);
    // Choose the staged pipeline depth from the prepared device and policy and
    // size its slots. Called at the end of prepare(); a test that swaps in a
    // scripted device calls it again before the node is attached.
    void configure_staged_pipeline();
    // Pipeline depth the current hosting, device and policy call for.
    std::uint32_t staged_pipeline_depth() const noexcept;
    void latch_staged_recovery(GpuAudioRecoveryReason reason) noexcept;
    void update_staged_breaker(StagedReadbackState& staged, bool on_time) noexcept;
    // Blocks for which the staged worker ran its CPU convolution since
    // prepare() (a missed block or the tail after one). Diagnostic.
    std::uint64_t staged_cpu_blocks() const noexcept;
    // Blocks whose GPU result was used, and breaker trips, since prepare().
    // Diagnostic; worker-thread values, read when the worker is quiescent.
    std::uint64_t staged_gpu_blocks() const noexcept;
    std::uint64_t staged_breaker_trips() const noexcept;
    // First cause latched by the staged worker since prepare(): ProviderLost
    // (device loss; replaces any other cause), SustainedDeadlineMisses (the
    // breaker tripped on a run of late results), ProviderFailure (failed map or
    // non-finite readback), else None. A single late block latches nothing.
    // Any thread; the public status accessor reads it.
    GpuAudioRecoveryReason staged_latched_reason() const noexcept;
    // The transport's worker-pipelining opt-in, bound through
    // bind_realtime_path(); `self` is the GpuConvolver.
    static bool set_staged_pipeline_hosting(void* self, bool hosted) noexcept;
    friend bool
    detail::configure_gpu_convolver_trial(GpuConvolver&,
                                          const detail::GpuConvolverTrialConfig&) noexcept;
    friend bool
    detail::drain_gpu_convolver_trial_records(GpuConvolver&,
                                              std::vector<detail::SharedIoTraceRecord>&) noexcept;
    friend bool detail::drain_gpu_convolver_trial_admissions(
        GpuConvolver&, std::vector<detail::SharedIoTraceAdmission>&) noexcept;
    friend std::uint64_t detail::gpu_convolver_trial_engine_id(const GpuConvolver&) noexcept;
    friend struct detail::GpuConvolverTestAccess;

    struct SharedIoState;

    uint32_t channels_;
    uint32_t block_;
    uint32_t sample_rate_;
    // Fixed before prepare(); this value is shared by descriptor PDC, the
    // shared-I/O bridge lead, and the CPU fallback delay.
    uint32_t latency_blocks_ = kLatencyBlocks;
    ProviderPolicy provider_policy_ = ProviderPolicy::Auto;
    std::vector<float> ir_;
    uint32_t fft_size_ = 0;
    std::uint8_t trial_requested_path_ = 0; // detail::SharedIoRequest::Auto
    std::uint64_t trial_generation_ = 1;
    bool trial_configured_ = false;
    bool trial_enable_trace_ = false;
    bool trial_capture_admissions_ = false;
    bool trial_capture_callback_timing_ = false;
    bool trial_staged_sync_reference_ = false;
    std::uint32_t trial_success_stride_ = 1;
    std::uint8_t trial_completion_policy_ = 0; // Dawn completion policy enum
    // Low 32 bits carry the private completion wait; high 32 bits carry the
    // private trace-retention bound. Keeping both in this existing field
    // preserves the exported class layout for SDK consumers.
    std::uint64_t trial_completion_wait_ns_ = 0;

    std::unique_ptr<render::GpuCompute> gpu_;
    std::unique_ptr<SharedIoState> shared_io_;
    std::unique_ptr<detail::StagedAsyncTrialState> staged_trial_;
    // Quiescent-only records for the explicit P4 staged_sync reference.
    std::vector<detail::SharedIoTraceRecord> staged_sync_records_;
    std::uint64_t staged_sequence_ = 0;
    std::vector<float> ir_spec_;            // 2*fft_size interleaved IR spectrum
    std::vector<std::vector<float>> carry_; // per-channel OLA accumulator (fft_size)

    // Per-block host scratch (allocated in prepare()). The FFT/mul/inverse
    // intermediates stay GPU-resident inside GpuCompute::convolve().
    std::vector<float> in_pad_; // 2*fft_size interleaved complex input
    std::vector<float> time_;   // 2*fft_size inverse result (one readback)

    // Continuously-fed zero-latency CPU fallbacks.
    std::vector<signal::FixedIrConvolver> fallback_;        // RT miss fallback
    std::vector<signal::FixedIrConvolver> worker_fallback_; // no-GPU worker path

    // Latency-alignment delay for the RT miss fallback. `fb_delay_` is a
    // `fallback_delay_blocks_`-deep circular buffer of block-sized, per-channel
    // slots; `fb_out_` holds this block's due (input(t-L)-convolved) substitute.
    uint32_t fallback_delay_blocks_ = 0;
    uint32_t fb_delay_idx_ = 0;
    std::vector<float> fb_delay_; // [delay_blocks][channels][block]
    std::vector<float> fb_out_;   // [channels][block]

    bool prepared_ = false;

    // Staged worker state: breaker settings, the pipelined readback slots, the
    // CPU convolution's owed tail and the latched recovery cause. Allocated by
    // the constructor so settings made before prepare() have a home.
    std::unique_ptr<StagedReadbackState> staged_readback_;
};

} // namespace pulp::gpu_audio
