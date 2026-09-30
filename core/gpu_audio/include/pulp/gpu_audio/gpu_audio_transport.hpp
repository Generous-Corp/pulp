#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <semaphore>
#include <thread>
#include <vector>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/planar_audio_ring_buffer.hpp>
#include <pulp/audio/workgroup.hpp>
#include <pulp/gpu_audio/gpu_audio_capability.hpp>
#include <pulp/gpu_audio/gpu_audio_node.hpp>

namespace pulp::gpu_audio {

/// Fixed-latency real-time transport between the audio thread and a non-RT
/// worker that runs a GpuAudioNode.
///
/// The audio thread calls process(): it writes the input block into a lock-free
/// ring and reads a block that was produced `latency_blocks` ago from a second
/// ring — it never waits on, allocates for, or synchronizes with the GPU. A
/// separate non-RT context calls pump() to drain the input ring, run the node,
/// and fill the output ring. Config::run_worker_thread can drive pump() from an
/// internal non-RT worker; exposing pump() directly keeps the scheduling logic
/// deterministically testable and supports an externally owned service worker.
///
/// Latency is established by priming the output ring with `latency_blocks` of
/// silence at prepare(); the host is told `latency_samples()` for PDC.
class GpuAudioTransport {
  public:
    // The node's descriptor is the single source of truth for channels, block
    // size, and latency. Config only carries transport knobs.
    struct Config {
        uint32_t ring_blocks = 4; // ring capacity, in blocks (>= latency+2)
        // Spawn an internal non-RT worker thread that drives pump(). By default
        // the worker polls the input ring, so the RT path stays fully decoupled
        // and lock-free. When false, the caller drives pump() (deterministic
        // tests / custom worker integration).
        bool run_worker_thread = false;
        // Opt-in: the RT process() posts a semaphore after each input write and
        // the worker waits on it (with the poll interval as a fallback timeout)
        // instead of always sleeping the full quantum. Cuts the worker's
        // reaction latency — and thus the miss probability at low latency_blocks
        // — without a busy loop. Signaling a semaphore is RT-safe (no alloc, no
        // block); default OFF keeps the pure-polling RT path byte-identical.
        // Only meaningful when run_worker_thread is true.
        bool wake_on_write = false;
        // Optional macOS Audio Workgroup for the owned submission worker. The
        // handle is borrowed by the transport and must remain valid until
        // release(). This is an experiment switch: Dawn encode/submit still
        // runs on the worker and is not claimed to be realtime-safe.
        void* audio_workgroup = nullptr;
        bool join_audio_workgroup = false;
    };

    struct Stats {
        std::uint64_t produced_blocks = 0;      // blocks the worker completed
        std::uint64_t miss_blocks = 0;          // RT reads with no ready output
        std::uint64_t input_dropped_frames = 0; // RT writes lost to a full input ring
        // Wet blocks the worker dropped to realign the stream after a miss. Each
        // miss already emitted a substitute (dry/fallback) block for that timeline
        // slot, so its late-arriving wet counterpart is redundant; dropping it
        // keeps effective latency pinned at latency_blocks instead of creeping one
        // block per miss (which would comb-filter dry against wet).
        std::uint64_t resynced_blocks = 0;
        // Worker-observed wall time. The staged path measures one process_block
        // call, including any blocking readback. The shared path measures one
        // service call that reports progress; it may cover several completions.
        // Neither interval is GPU execution time, CPU consumption, or an audio
        // deadline measurement. last is the latest recorded interval; avg is an
        // EWMA. Both remain zero until the worker first reports progress.
        double last_block_us = 0.0;
        double avg_block_us = 0.0;
        bool worker_workgroup_joined = false;
        std::uint64_t worker_workgroup_join_failures = 0;
    };

    /// Selected output for prepared process() calls, not worker completions or
    /// GPU admissions. Each call contributes to exactly one counter, including
    /// a rejected view's single invalid position. Offline and unprepared calls
    /// are excluded. These counters do not identify individual stream blocks.
    struct DeliverySnapshot {
        std::uint64_t gpu_blocks = 0; // callback-side GPU path selected ready output
        // A generic node may implement process_gpu() using CPU work or internal
        // fallback. A ring delivery alone cannot establish GPU execution.
        std::uint64_t worker_output_blocks = 0;
        std::uint64_t cpu_fallback_blocks = 0;
        std::uint64_t silence_blocks = 0;
        std::uint64_t passthrough_blocks = 0;
        std::uint64_t priming_blocks = 0;
        std::uint64_t invalid_blocks = 0;
    };

    GpuAudioTransport() = default;
    ~GpuAudioTransport() {
        release();
    }

    GpuAudioTransport(const GpuAudioTransport&) = delete;
    GpuAudioTransport& operator=(const GpuAudioTransport&) = delete;

    /// Non-RT. `node` must outlive the transport and already be prepared. The
    /// node's descriptor supplies channels/block-size/latency; `config` only
    /// carries transport knobs. Returns false if the descriptor is invalid
    /// (zero channels/block, input!=output channels), ring_blocks is too small
    /// for the latency, or the miss policy is CpuFallback without
    /// supports_cpu_fallback. Callback and external pump callers must be stopped
    /// before preparation; an already-prepared shared node retains its sequence
    /// and fallback history across transport preparation.
    bool prepare(GpuAudioNode* node, const Config& config);
    void release() noexcept;

    bool is_prepared() const noexcept {
        return prepared_;
    }
    uint32_t latency_samples() const noexcept {
        return latency_blocks_ * block_size_;
    }

    /// Real-time-safe. Writes `n` input frames to the worker and reads the
    /// `latency_blocks`-delayed output. `n` must equal block_size. On a miss
    /// (worker not ready) the node's MissPolicy fills `output`. No allocation,
    /// locking, or blocking. Calls to process() and process_offline() share one
    /// callback timeline and must never overlap. A rejected view emits silence
    /// and advances one zero-input position through the prepared fallback and
    /// delay state, preserving the due position of subsequent valid audio.
    void process(const audio::BufferView<const float>& input, audio::BufferView<float>& output,
                 uint32_t n) noexcept;

    /// Offline / faster-than-real-time render path. NOT real-time-safe — it
    /// drives the node SYNCHRONOUSLY on the calling thread (blocking GPU readback
    /// is fine here: an offline bounce has no real-time deadline) so the actual
    /// node output is captured instead of the async misses that an offline render
    /// would otherwise hit (the host calls process faster than the wall-clock-paced
    /// worker can keep up). Preserves the SAME fixed latency as process() (the
    /// primed output ring), so a host that compensates for `latency_samples()`
    /// stays sample-aligned between realtime playback and an offline bounce. While
    /// this is in use the background worker yields (a shared mutex serializes node
    /// access); switch back to process() to resume async realtime operation. `n`
    /// must equal block_size. The experimental shared provider is fenced into
    /// continuously primed CPU fallback here; a new host preparation is required
    /// before it may admit GPU work again. The callback must already be stopped
    /// before crossing this non-RT boundary.
    void process_offline(const audio::BufferView<const float>& input,
                         audio::BufferView<float>& output, uint32_t n) noexcept;

    /// Non-RT worker step: drain ready input blocks, run the node, fill output.
    /// Processes at most `max_blocks` blocks (0 == as many as are ready).
    void pump(uint32_t max_blocks = 0) noexcept;

    Stats stats() const noexcept;

    /// Allocation-free independent atomic loads: approximate while process()
    /// runs, exact once its caller has stopped. No coherent multi-field instant
    /// is promised. Successful prepare() resets counters; release() and a false
    /// prepare() result preserve them, although failed preparation leaves the
    /// transport unprepared. Preparation/destruction must not race readers.
    DeliverySnapshot delivery_snapshot() const noexcept;

    /// Host/UI-only snapshot of the prepared integration path. This is
    /// allocation-free and does not touch the callback timeline. Provider
    /// identity is Unknown when a generic staged node cannot establish it.
    GpuAudioCapabilityReport capability_report() const noexcept;

  private:
    using TrialDeliveryFn = void (*)(void*, std::uint64_t, std::uint8_t, std::uint64_t,
                                     std::uint64_t, std::uint64_t) noexcept;
    friend bool configure_gpu_audio_transport_trial_observer(GpuAudioTransport&, void*,
                                                             TrialDeliveryFn) noexcept;

    GpuAudioNode* node_ = nullptr;
    // Derived from the node descriptor + config at prepare(); authoritative for
    // the RT path so it never calls the (allocating) descriptor().
    uint32_t channels_ = 0;
    uint32_t block_size_ = 0;
    uint32_t latency_blocks_ = 0;
    uint32_t ring_blocks_ = 0;
    // Fails closed until prepare() copies the node's declared policy in.
    MissPolicy miss_policy_ = MissPolicy::Silence;
    bool prepared_ = false;

    void reset_staged_transport_state() noexcept;
    void process_shared(const audio::BufferView<const float>&, audio::BufferView<float>&,
                        std::uint32_t, std::uint64_t, bool input_valid,
                        std::uint64_t callback_start_ns, bool count_delivery) noexcept;
    void process_realtime_position(const audio::BufferView<const float>&, audio::BufferView<float>&,
                                   std::uint32_t, std::uint64_t, bool input_valid,
                                   std::uint64_t callback_start_ns) noexcept;
    void process_offline_position(const audio::BufferView<const float>&, audio::BufferView<float>&,
                                  std::uint32_t, std::uint64_t, bool input_valid) noexcept;
    void process_invalid_position(audio::BufferView<float>&, std::uint64_t, bool offline) noexcept;
    void publish_trial_delivery(std::uint64_t sequence, std::uint8_t disposition,
                                std::uint64_t callback_start_ns, std::uint64_t callback_end_ns,
                                std::uint64_t result_visible_ns) noexcept;
    void record_delivery(std::uint8_t disposition, bool worker_output) noexcept;

    audio::PlanarAudioRingBuffer input_ring_;
    audio::PlanarAudioRingBuffer output_ring_;

    audio::Buffer<float> worker_in_;
    audio::Buffer<float> worker_out_;
    // Callback-owned scratch for rejected views. Zero input advances every
    // stateful delay/history position without reading the malformed input.
    audio::Buffer<float> rejected_input_;
    audio::Buffer<float> rejected_output_;
    std::vector<const float*> rejected_input_ptrs_;
    std::vector<float*> rejected_output_ptrs_;
    // Stable channel-pointer arrays for the worker views (BufferView holds the
    // array by reference, so it must outlive the views).
    std::vector<float*> in_fptrs_;
    std::vector<const float*> in_cptrs_;
    std::vector<float*> out_fptrs_;

    std::atomic<std::uint64_t> produced_blocks_{0};
    std::atomic<std::uint64_t> miss_blocks_{0};
    std::atomic<std::uint64_t> input_dropped_blocks_{0}; // whole-block input drops
    std::atomic<std::uint64_t> resynced_blocks_{0};      // late wet blocks dropped to realign
    std::atomic<std::uint64_t> delivery_gpu_{0}, delivery_worker_{0}, delivery_fallback_{0},
        delivery_silence_{0}, delivery_passthrough_{0}, delivery_priming_{0}, delivery_invalid_{0};
    // Resync debt: output slots a miss already substituted for, whose late wet
    // counterparts must still be dropped to realign the stream. Incremented on a
    // miss, decremented as those blocks are drained. Touched ONLY by process() on
    // the audio thread, so it needs no atomicity — counting misses explicitly
    // (rather than inferring "excess ring depth") keeps the drain immune to a
    // worker that races a fresh block in before the RT read of the same call.
    std::uint64_t blocks_owed_ = 0;
    // Per-block worker timing, published as integer nanoseconds for lock-free
    // reads by the UI thread (double isn't reliably lock-free).
    std::atomic<std::uint64_t> last_block_ns_{0};
    std::atomic<std::uint64_t> avg_block_ns_{0};

    // Optional internal worker. The worker polls the input ring; the RT path
    // never touches these.
    void worker_loop() noexcept;
    std::thread worker_;
    std::atomic<bool> worker_running_{false};
    std::chrono::microseconds poll_interval_{200};
    void* audio_workgroup_ = nullptr;
    bool join_audio_workgroup_ = false;
    std::atomic<bool> worker_workgroup_joined_{false};
    std::atomic<std::uint64_t> worker_workgroup_join_failures_{0};

    // Opt-in wake-on-write (Config::wake_on_write). The RT process() posts
    // `wake_sem_` after each input write; the worker waits on it (bounded by
    // poll_interval_) instead of always sleeping the full quantum, cutting its
    // reaction latency without a busy loop. release() on a counting semaphore is
    // non-blocking and allocation-free, so posting it stays RT-safe. Default
    // OFF: worker_loop() falls back to a plain sleep so the polling path is
    // unchanged.
    bool wake_on_write_ = false;
    std::counting_semaphore<> wake_sem_{0};

    // Private callback-side GPU path captured at prepare() from the concrete
    // node. These are plain function pointers so the RT process path does not
    // allocate, lock, or touch a public base-class extension.
    void* realtime_gpu_context_ = nullptr;
    std::uint8_t (*realtime_gpu_process_)(void*, const audio::BufferView<const float>&,
                                          audio::BufferView<float>&, std::uint32_t, std::uint64_t,
                                          bool, std::uint64_t) noexcept = nullptr;
    std::uint32_t (*realtime_gpu_service_)(void*, std::uint64_t) noexcept = nullptr;
    bool (*realtime_gpu_fence_)(void*) noexcept = nullptr;
    void (*realtime_gpu_delivered_)(void*, std::uint64_t, std::uint8_t, std::uint64_t,
                                    std::uint64_t) noexcept = nullptr;
    std::uint64_t callback_sequence_ = 0; // one monotonic RT/offline callback timeline
    bool realtime_gpu_fenced_for_offline_ = false;

    // Synchronous (offline) drive. When `synchronous_` is set, the background
    // worker yields and process_offline() pumps the node inline under
    // `pump_mutex_`; the realtime process() clears the flag so the worker
    // resumes. `pump_mutex_` only ever contends between the offline path and the
    // worker (both off the audio thread) — process() never takes it, so the
    // realtime path stays lock-free.
    std::atomic<bool> synchronous_{false};
    std::mutex pump_mutex_;

    // Host-only P4 trial observer. The observer is installed before the
    // callback starts and must itself be realtime-safe. The default runtime
    // path leaves it null, so this adds no hot-path work for normal users.
    void* trial_observer_context_ = nullptr;
    TrialDeliveryFn trial_observer_ = nullptr;
};

} // namespace pulp::gpu_audio
