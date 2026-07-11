#pragma once

#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <semaphore>
#include <thread>
#include <vector>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/planar_audio_ring_buffer.hpp>
#include <pulp/gpu_audio/gpu_audio_node.hpp>

namespace pulp::gpu_audio {

/// Fixed-latency real-time transport between the audio thread and a non-RT
/// worker that runs a GpuAudioNode.
///
/// The audio thread calls process(): it writes the input block into a lock-free
/// ring and reads a block that was produced `latency_blocks` ago from a second
/// ring — it never waits on, allocates for, or synchronizes with the GPU. A
/// separate non-RT context calls pump() to drain the input ring, run the node,
/// and fill the output ring. (pump() is driven by a background worker thread in
/// a later slice; exposing it directly keeps the scheduling logic
/// deterministically testable.)
///
/// Latency is established by priming the output ring with `latency_blocks` of
/// silence at prepare(); the host is told `latency_samples()` for PDC.
class GpuAudioTransport {
public:
    // The node's descriptor is the single source of truth for channels, block
    // size, and latency. Config only carries transport knobs.
    struct Config {
        uint32_t ring_blocks = 4;        // ring capacity, in blocks (>= latency+2)
        // Spawn an internal non-RT worker thread that drives pump(). The worker
        // POLLS the input ring (the audio thread never signals it), so the RT
        // path stays fully decoupled and lock-free. When false, the caller
        // drives pump() (deterministic tests / custom worker integration).
        bool run_worker_thread = false;
        // Opt-in: the RT process() posts a semaphore after each input write and
        // the worker waits on it (with the poll interval as a fallback timeout)
        // instead of always sleeping the full quantum. Cuts the worker's
        // reaction latency — and thus the miss probability at low latency_blocks
        // — without a busy loop. Signaling a semaphore is RT-safe (no alloc, no
        // block); default OFF keeps the pure-polling RT path byte-identical.
        // Only meaningful when run_worker_thread is true.
        bool wake_on_write = false;
    };

    struct Stats {
        std::uint64_t produced_blocks = 0;   // blocks the worker completed
        std::uint64_t miss_blocks = 0;       // RT reads with no ready output
        std::uint64_t input_dropped_frames = 0;   // RT writes lost to a full input ring
        // Wet blocks the worker dropped to realign the stream after a miss. Each
        // miss already emitted a substitute (dry/fallback) block for that timeline
        // slot, so its late-arriving wet counterpart is redundant; dropping it
        // keeps effective latency pinned at latency_blocks instead of creeping one
        // block per miss (which would comb-filter dry against wet).
        std::uint64_t resynced_blocks = 0;
        // Wall-clock cost of the node's work per block, measured on the worker
        // thread (includes the GPU submit + the blocking readback — the honest
        // real cost of the GPU path). last = most recent block; avg = an EWMA.
        // Zero until the first block is produced.
        double last_block_us = 0.0;
        double avg_block_us = 0.0;
    };

    GpuAudioTransport() = default;
    ~GpuAudioTransport() { release(); }

    GpuAudioTransport(const GpuAudioTransport&) = delete;
    GpuAudioTransport& operator=(const GpuAudioTransport&) = delete;

    /// Non-RT. `node` must outlive the transport and already be prepared. The
    /// node's descriptor supplies channels/block-size/latency; `config` only
    /// carries transport knobs. Returns false if the descriptor is invalid
    /// (zero channels/block, input!=output channels), ring_blocks is too small
    /// for the latency, or the miss policy is CpuFallback without
    /// supports_cpu_fallback.
    bool prepare(GpuAudioNode* node, const Config& config);
    void release() noexcept;

    bool is_prepared() const noexcept { return prepared_; }
    uint32_t latency_samples() const noexcept { return latency_blocks_ * block_size_; }

    /// Real-time-safe. Writes `n` input frames to the worker and reads the
    /// `latency_blocks`-delayed output. `n` must equal block_size. On a miss
    /// (worker not ready) the node's MissPolicy fills `output`. No allocation,
    /// locking, or blocking.
    void process(const audio::BufferView<const float>& input,
                 audio::BufferView<float>& output, uint32_t n) noexcept;

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
    /// must equal block_size.
    void process_offline(const audio::BufferView<const float>& input,
                         audio::BufferView<float>& output, uint32_t n) noexcept;

    /// Non-RT worker step: drain ready input blocks, run the node, fill output.
    /// Processes at most `max_blocks` blocks (0 == as many as are ready).
    void pump(uint32_t max_blocks = 0) noexcept;

    Stats stats() const noexcept;

private:
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

    audio::PlanarAudioRingBuffer input_ring_;
    audio::PlanarAudioRingBuffer output_ring_;

    audio::Buffer<float> worker_in_;
    audio::Buffer<float> worker_out_;
    // Stable channel-pointer arrays for the worker views (BufferView holds the
    // array by reference, so it must outlive the views).
    std::vector<float*> in_fptrs_;
    std::vector<const float*> in_cptrs_;
    std::vector<float*> out_fptrs_;

    std::atomic<std::uint64_t> produced_blocks_{0};
    std::atomic<std::uint64_t> miss_blocks_{0};
    std::atomic<std::uint64_t> input_dropped_blocks_{0};  // whole-block input drops
    std::atomic<std::uint64_t> resynced_blocks_{0};       // late wet blocks dropped to realign
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

    // Opt-in wake-on-write (Config::wake_on_write). The RT process() posts
    // `wake_sem_` after each input write; the worker waits on it (bounded by
    // poll_interval_) instead of always sleeping the full quantum, cutting its
    // reaction latency without a busy loop. release() on a counting semaphore is
    // non-blocking and allocation-free, so posting it stays RT-safe. Default
    // OFF: worker_loop() falls back to a plain sleep so the polling path is
    // unchanged.
    bool wake_on_write_ = false;
    std::counting_semaphore<> wake_sem_{0};

    // Synchronous (offline) drive. When `synchronous_` is set, the background
    // worker yields and process_offline() pumps the node inline under
    // `pump_mutex_`; the realtime process() clears the flag so the worker
    // resumes. `pump_mutex_` only ever contends between the offline path and the
    // worker (both off the audio thread) — process() never takes it, so the
    // realtime path stays lock-free.
    std::atomic<bool> synchronous_{false};
    std::mutex pump_mutex_;
};

} // namespace pulp::gpu_audio
