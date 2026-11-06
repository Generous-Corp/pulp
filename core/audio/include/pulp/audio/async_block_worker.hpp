#pragma once

/// @file async_block_worker.hpp
/// Fixed-latency playout of audio (and timed events) rendered on a worker
/// thread, for hosts that send blocks of any size.

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/pull_rate_detector.hpp>
#include <pulp/runtime/wake_signal.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

namespace pulp::audio {

/// How a process() call may treat a worker that has not finished the audio
/// the callback is about to play.
enum class AsyncBlockMode : std::uint8_t {
    /// Live callback: never wait. Unfinished audio plays as silence and is
    /// counted as a miss; the reported latency never moves.
    Realtime,
    /// Declared offline render: wait for the worker (bounded by the
    /// configured deadline), so the bounce holds the worker's real output.
    Offline,
    /// Realtime, except while a PullRateDetector sees the host pulling faster
    /// than real time without declaring it; then wait as for Offline.
    Auto,
};

/// A timed event handed to or produced by the worker. `offset` is the frame
/// within the block (process()) or chunk (worker) it belongs to.
template <typename Event> struct AsyncBlockEvent {
    std::uint32_t offset = 0;
    Event event{};
};

struct AsyncBlockWorkerConfig {
    double sample_rate = 48000.0;
    std::uint32_t input_channels = 0; ///< 0 for an instrument
    std::uint32_t output_channels = 2;
    /// The largest block the host may send; also the unit of latency.
    std::uint32_t max_block = 512;
    /// Latency in blocks of max_block. 0 makes every call wait for the
    /// worker, like rendering inline.
    std::uint32_t latency_blocks = 1;
    /// The largest value request_latency_blocks() may later set.
    std::uint32_t max_latency_blocks = 8;
    /// The longest chunk handed to the job; 0 means max_block.
    std::uint32_t max_chunk = 0;
    /// Capacity of the event rings, in events in flight each way.
    std::uint32_t max_events_in = 256;
    std::uint32_t max_events_out = 256;
    /// Latency the worker's own processing adds (a resampler, a look-ahead),
    /// reported on top of the block latency.
    std::uint32_t worker_latency_samples = 0;
    /// Longest a waiting process() call blocks before it plays what exists.
    std::chrono::microseconds wait_deadline{500000};
    /// Run an internal worker thread. When false the owner calls pump()
    /// (tests, an externally owned service thread); Offline/Auto waits then
    /// pump inline on the calling thread.
    bool run_worker_thread = true;
};

struct AsyncBlockWorkerStats {
    std::uint64_t produced_frames = 0; ///< frames the worker rendered
    std::uint64_t miss_frames = 0;     ///< frames played as silence because unfinished
    std::uint64_t overrun_frames =
        0;                   ///< input frames the worker skipped after falling a ring behind
    std::uint64_t waits = 0; ///< process() calls that waited for the worker
    std::uint64_t wait_timeouts = 0;     ///< waits that hit the deadline
    std::uint64_t events_in_dropped = 0; ///< input events lost to a full ring
    std::uint64_t events_out_dropped = 0;
    std::uint64_t late_events = 0;     ///< output events delivered at offset 0 past their frame
    std::uint64_t latency_changes = 0; ///< reported-latency changes signalled
    bool fast_pull = false;            ///< Auto: the detector's last verdict
};

/// Plays audio that a worker thread renders, at an exact, fixed latency, for
/// a host whose blocks vary in size.
///
/// Every frame has a stream position. process() writes the host block's input
/// (and timed events) at positions [S, S + n), then plays the output of
/// positions [D - L, D - L + n), where D counts the frames delivered so far
/// and L = latency_blocks * max_block. The worker renders the positions in
/// order, in chunks of its own size, into a ring addressed by position.
/// Because latency is defined on positions, not on calls, it is exact for any
/// block-size sequence (1 frame, 4096 frames, alternating) and cannot drift:
/// a frame the worker has not finished in time plays as silence (counted as
/// a miss) and its late output is never played, rather than shifting what
/// follows. Positions before the first frame play as silence (pre-roll).
///
/// Events go to the worker with their absolute position and come back the
/// same way, so an event the job emits at chunk offset k is delivered in the
/// output callback that plays position start + k, at the matching offset.
/// An output event whose frame has already been played is delivered at
/// offset 0 of the next call (counted late) rather than dropped: a note-off
/// must not be lost.
///
/// Threads:
/// - prepare() / release(): control thread, no process() or pump() running.
/// - process(): the audio thread. No allocation and no lock; it posts the
///   worker through runtime::WakeSignal (a system call only while the worker
///   sleeps). It blocks only in Offline mode, in Auto while the host pulls
///   faster than real time, or at zero latency, and then never longer than
///   wait_deadline.
/// - The job: the worker thread (or the pump() caller), one chunk at a time.
/// - request_latency_blocks() / set_worker_latency_samples(): any thread.
///   A change takes effect at the start of the next process() call, which
///   then calls the latency listener on the audio thread (a processor's
///   flag_latency_changed() is the intended listener; it is realtime safe).
///   A larger latency never replays audio: the frames the window moves back
///   over were already played and play as silence once; a smaller one skips
///   frames. Hosts expect a glitch when a plug-in's latency changes.
///
/// GpuAudioTransport keeps the same timeline for the special case of one
/// fixed block size: its primed output ring is pre-roll, its latency_blocks
/// is L / max_block, and its resync after a miss is the position rule above.
template <typename Event = std::uint8_t> class AsyncBlockWorker {
    static_assert(std::is_default_constructible_v<Event> &&
                      std::is_nothrow_copy_assignable_v<Event>,
                  "AsyncBlockWorker events are copied into fixed rings");

  public:
    using TimedEvent = AsyncBlockEvent<Event>;

    /// One stretch of positions the job renders. Input and output views
    /// point into the rings; events carry chunk offsets.
    class Chunk {
      public:
        std::uint64_t start = 0; ///< stream position of frame 0
        std::uint32_t frames = 0;
        BufferView<const float> input;
        BufferView<float> output; ///< zeroed before the job runs
        std::span<const TimedEvent> events;

        /// Emits an output event at `offset` within this chunk. False (and
        /// counted) when the output ring is full.
        bool emit(std::uint32_t offset, const Event& event) noexcept {
            return owner_->push_out_event(start + std::min(offset, frames ? frames - 1 : 0u),
                                          event);
        }

      private:
        friend class AsyncBlockWorker;
        AsyncBlockWorker* owner_ = nullptr;
    };

    using Job = std::function<void(Chunk&)>;
    using LatencyListener = void (*)(void* context) noexcept;

    AsyncBlockWorker() = default;
    ~AsyncBlockWorker() {
        release();
    }
    AsyncBlockWorker(const AsyncBlockWorker&) = delete;
    AsyncBlockWorker& operator=(const AsyncBlockWorker&) = delete;

    /// Control thread. Allocates the rings and starts the worker thread when
    /// configured. False for an invalid configuration.
    bool prepare(const AsyncBlockWorkerConfig& config, Job job) {
        release();
        if (!job || config.max_block == 0 || config.output_channels == 0 ||
            config.latency_blocks > config.max_latency_blocks || !(config.sample_rate > 0.0))
            return false;
        config_ = config;
        if (config_.max_chunk == 0 || config_.max_chunk > config_.max_block)
            config_.max_chunk = config_.max_block;
        job_ = std::move(job);
        // The worker writes no further ahead than the frames already
        // submitted, and the callback reads no further back than one block
        // plus the largest latency, so this span is never overwritten while
        // it can still be played. The extra blocks give a lagging worker room
        // before it overruns.
        const std::uint64_t needed =
            (static_cast<std::uint64_t>(config_.max_latency_blocks) + 4) * config_.max_block;
        capacity_ = 1;
        while (capacity_ < needed)
            capacity_ <<= 1;
        input_.assign(static_cast<std::size_t>(config_.input_channels) * capacity_, 0.0f);
        output_.assign(static_cast<std::size_t>(config_.output_channels) * capacity_, 0.0f);
        input_ptrs_.assign(config_.input_channels, nullptr);
        output_ptrs_.assign(config_.output_channels, nullptr);
        events_in_.assign(round_up(config_.max_events_in), Stamped{});
        events_out_.assign(round_up(config_.max_events_out), Stamped{});
        chunk_events_.assign(events_in_.size(), TimedEvent{});
        reset_counters();
        latency_blocks_ = config_.latency_blocks;
        requested_latency_blocks_.store(config_.latency_blocks, std::memory_order_relaxed);
        worker_latency_.store(config_.worker_latency_samples, std::memory_order_relaxed);
        applied_worker_latency_ = config_.worker_latency_samples;
        detector_.reset();
        prepared_ = true;
        if (config_.run_worker_thread) {
            stop_.store(false, std::memory_order_relaxed);
            thread_ = std::thread([this] { run(); });
        }
        return true;
    }

    /// Control thread. Stops and joins the worker thread.
    void release() noexcept {
        if (thread_.joinable()) {
            stop_.store(true, std::memory_order_release);
            work_.notify();
            thread_.join();
        }
        prepared_ = false;
    }

    bool is_prepared() const noexcept {
        return prepared_;
    }
    const AsyncBlockWorkerConfig& config() const noexcept {
        return config_;
    }

    /// The latency to report to the host: the block latency plus the
    /// worker's own. Any thread.
    std::uint32_t latency_samples() const noexcept {
        return requested_latency_blocks_.load(std::memory_order_acquire) * config_.max_block +
               worker_latency_.load(std::memory_order_acquire);
    }

    /// Any thread. Takes effect at the next process() call. False when out
    /// of the prepared range.
    bool request_latency_blocks(std::uint32_t blocks) noexcept {
        if (blocks > config_.max_latency_blocks)
            return false;
        requested_latency_blocks_.store(blocks, std::memory_order_release);
        return true;
    }

    /// Any thread (typically the job, when its own latency moves).
    void set_worker_latency_samples(std::uint32_t samples) noexcept {
        worker_latency_.store(samples, std::memory_order_release);
    }

    /// Called on the audio thread, inside process(), when the reported
    /// latency changed. Set before the first process() call.
    void set_latency_listener(LatencyListener listener, void* context) noexcept {
        listener_ = listener;
        listener_context_ = context;
    }

    /// The audio callback. `input` may be empty for an instrument; `frames`
    /// at most max_block. Events carry offsets within this block, in offset
    /// order; `emit` is called as emit(offset, event) for each output event
    /// due in it. A job emits its chunk's events in offset order too.
    template <typename Emit>
    void process(const BufferView<const float>& input, BufferView<float>& output,
                 std::uint32_t frames, std::span<const TimedEvent> events, Emit&& emit,
                 AsyncBlockMode mode = AsyncBlockMode::Realtime) noexcept {
        for (std::size_t c = 0; c < output.num_channels(); ++c)
            std::fill_n(output.channel_ptr(c), std::min<std::size_t>(frames, output.num_samples()),
                        0.0f);
        if (!prepared_ || frames == 0 || frames > config_.max_block)
            return;
        apply_latency_changes();

        // Input: positions [submitted, submitted + frames).
        const std::uint64_t submitted = submitted_.load(std::memory_order_relaxed);
        const std::uint64_t produced_now = produced_.load(std::memory_order_acquire);
        if (submitted + frames - produced_now > capacity_ - config_.max_block) {
            // The worker fell a ring behind: let it skip to here so the
            // timeline holds; what it skips plays as silence.
            skip_to_.store(submitted, std::memory_order_release);
        }
        for (std::uint32_t c = 0; c < config_.input_channels; ++c) {
            float* ring = input_.data() + static_cast<std::size_t>(c) * capacity_;
            const float* source = c < input.num_channels() && input.num_samples() >= frames
                                      ? input.channel_ptr(c)
                                      : nullptr;
            for (std::uint32_t i = 0; i < frames; ++i)
                ring[(submitted + i) & (capacity_ - 1)] = source ? source[i] : 0.0f;
        }
        for (const auto& timed : events)
            push_in_event(submitted + std::min(timed.offset, frames - 1), timed.event);
        submitted_.store(submitted + frames, std::memory_order_release);
        work_.notify();

        // Output: positions [window_start, window_end).
        const std::int64_t window_start =
            static_cast<std::int64_t>(delivered_) -
            static_cast<std::int64_t>(latency_blocks_) * config_.max_block;
        const std::int64_t window_end = window_start + frames;
        delivered_ += frames;

        bool wait = latency_blocks_ == 0 || mode == AsyncBlockMode::Offline;
        if (mode == AsyncBlockMode::Auto) {
            const bool fast =
                detector_.observe(frames, config_.sample_rate, PullRateDetector::Clock::now());
            fast_pull_.store(fast, std::memory_order_relaxed);
            wait = wait || fast;
        }
        if (wait && window_end > 0)
            wait_until_produced(static_cast<std::uint64_t>(window_end));

        const std::uint64_t produced = produced_.load(std::memory_order_acquire);
        const std::uint64_t valid_from = valid_from_.load(std::memory_order_acquire);
        std::uint64_t missed = 0;
        for (std::int64_t p = std::max<std::int64_t>(window_start, 0); p < window_end; ++p) {
            const auto position = static_cast<std::uint64_t>(p);
            const auto target = static_cast<std::size_t>(p - window_start);
            if (position < played_until_)
                continue; // already played before a latency increase
            if (position >= produced || position < valid_from) {
                ++missed;
                continue;
            }
            const std::size_t index = position & (capacity_ - 1);
            for (std::size_t c = 0; c < output.num_channels() && c < config_.output_channels; ++c)
                output.channel_ptr(c)[target] = output_[c * capacity_ + index];
        }
        if (missed)
            miss_frames_.fetch_add(missed, std::memory_order_relaxed);

        drain_out_events(window_start, window_end, emit);
    }

    /// Worker step: renders every submitted position not yet produced, at
    /// most `max_chunks` chunks (0: all). Returns the chunks rendered. Call
    /// from one thread at a time: the internal worker, or the owner when
    /// run_worker_thread is false.
    std::uint32_t pump(std::uint32_t max_chunks = 0) noexcept {
        if (!prepared_)
            return 0;
        std::uint32_t chunks = 0;
        for (;;) {
            if (max_chunks && chunks >= max_chunks)
                break;
            std::uint64_t produced = produced_.load(std::memory_order_relaxed);
            const std::uint64_t skip = skip_to_.load(std::memory_order_acquire);
            if (skip > produced) {
                // Overrun: drop the input events of the skipped span and mark
                // its output invalid instead of rendering stale ring data.
                overrun_frames_.fetch_add(skip - produced, std::memory_order_relaxed);
                discard_in_events_before(skip);
                valid_from_.store(skip, std::memory_order_release);
                produced_.store(skip, std::memory_order_release);
                produced = skip;
            }
            const std::uint64_t submitted = submitted_.load(std::memory_order_acquire);
            if (submitted <= produced)
                break;
            std::uint64_t frames = std::min<std::uint64_t>(submitted - produced, config_.max_chunk);
            const std::uint64_t index = produced & (capacity_ - 1);
            frames = std::min<std::uint64_t>(frames, capacity_ - index); // no wrap inside a chunk
            render_chunk(produced, static_cast<std::uint32_t>(frames),
                         static_cast<std::size_t>(index));
            produced_.store(produced + frames, std::memory_order_release);
            produced_frames_.fetch_add(frames, std::memory_order_relaxed);
            done_.notify();
            ++chunks;
        }
        return chunks;
    }

    AsyncBlockWorkerStats stats() const noexcept {
        AsyncBlockWorkerStats s;
        s.produced_frames = produced_frames_.load(std::memory_order_relaxed);
        s.miss_frames = miss_frames_.load(std::memory_order_relaxed);
        s.overrun_frames = overrun_frames_.load(std::memory_order_relaxed);
        s.waits = waits_.load(std::memory_order_relaxed);
        s.wait_timeouts = wait_timeouts_.load(std::memory_order_relaxed);
        s.events_in_dropped = events_in_dropped_.load(std::memory_order_relaxed);
        s.events_out_dropped = events_out_dropped_.load(std::memory_order_relaxed);
        s.late_events = late_events_.load(std::memory_order_relaxed);
        s.latency_changes = latency_changes_.load(std::memory_order_relaxed);
        s.fast_pull = fast_pull_.load(std::memory_order_relaxed);
        return s;
    }

  private:
    struct Stamped {
        std::uint64_t position = 0;
        Event event{};
    };

    static std::size_t round_up(std::uint32_t n) {
        std::size_t size = 1;
        while (size < std::max<std::uint32_t>(n, 1))
            size <<= 1;
        return size;
    }

    void reset_counters() noexcept {
        submitted_.store(0, std::memory_order_relaxed);
        produced_.store(0, std::memory_order_relaxed);
        skip_to_.store(0, std::memory_order_relaxed);
        valid_from_.store(0, std::memory_order_relaxed);
        delivered_ = 0;
        played_until_ = 0;
        in_write_.store(0, std::memory_order_relaxed);
        in_read_.store(0, std::memory_order_relaxed);
        out_write_.store(0, std::memory_order_relaxed);
        out_read_.store(0, std::memory_order_relaxed);
        for (auto* counter :
             {&produced_frames_, &miss_frames_, &overrun_frames_, &waits_, &wait_timeouts_,
              &events_in_dropped_, &events_out_dropped_, &late_events_, &latency_changes_})
            counter->store(0, std::memory_order_relaxed);
        fast_pull_.store(false, std::memory_order_relaxed);
    }

    void apply_latency_changes() noexcept {
        const auto blocks = requested_latency_blocks_.load(std::memory_order_acquire);
        const auto worker = worker_latency_.load(std::memory_order_acquire);
        if (blocks == latency_blocks_ && worker == applied_worker_latency_)
            return;
        if (blocks > latency_blocks_) {
            // The window moves back over frames already played: play those
            // as silence instead of a second time.
            const auto played = static_cast<std::int64_t>(delivered_) -
                                static_cast<std::int64_t>(latency_blocks_) * config_.max_block;
            if (played > 0)
                played_until_ = std::max(played_until_, static_cast<std::uint64_t>(played));
        }
        latency_blocks_ = blocks;
        applied_worker_latency_ = worker;
        latency_changes_.fetch_add(1, std::memory_order_relaxed);
        if (listener_)
            listener_(listener_context_);
    }

    void wait_until_produced(std::uint64_t position) noexcept {
        if (produced_.load(std::memory_order_acquire) >= position)
            return;
        waits_.fetch_add(1, std::memory_order_relaxed);
        if (!config_.run_worker_thread) {
            pump();
        } else {
            const bool done = done_.wait_for(
                [&] { return produced_.load(std::memory_order_acquire) >= position; },
                std::chrono::duration_cast<runtime::WakeSignal::Clock::duration>(
                    config_.wait_deadline));
            if (!done)
                wait_timeouts_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void push_in_event(std::uint64_t position, const Event& event) noexcept {
        const auto write = in_write_.load(std::memory_order_relaxed);
        if (write - in_read_.load(std::memory_order_acquire) >= events_in_.size()) {
            events_in_dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        events_in_[write & (events_in_.size() - 1)] = Stamped{position, event};
        in_write_.store(write + 1, std::memory_order_release);
    }

    void discard_in_events_before(std::uint64_t position) noexcept {
        auto read = in_read_.load(std::memory_order_relaxed);
        const auto write = in_write_.load(std::memory_order_acquire);
        while (read != write && events_in_[read & (events_in_.size() - 1)].position < position)
            ++read;
        in_read_.store(read, std::memory_order_release);
    }

    bool push_out_event(std::uint64_t position, const Event& event) noexcept {
        const auto write = out_write_.load(std::memory_order_relaxed);
        if (write - out_read_.load(std::memory_order_acquire) >= events_out_.size()) {
            events_out_dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        events_out_[write & (events_out_.size() - 1)] = Stamped{position, event};
        out_write_.store(write + 1, std::memory_order_release);
        return true;
    }

    template <typename Emit>
    void drain_out_events(std::int64_t window_start, std::int64_t window_end, Emit& emit) noexcept {
        auto read = out_read_.load(std::memory_order_relaxed);
        const auto write = out_write_.load(std::memory_order_acquire);
        while (read != write) {
            const auto& stamped = events_out_[read & (events_out_.size() - 1)];
            const auto position = static_cast<std::int64_t>(stamped.position);
            if (position >= window_end)
                break;
            std::uint32_t offset = 0;
            if (position >= window_start)
                offset = static_cast<std::uint32_t>(position - window_start);
            else
                late_events_.fetch_add(1, std::memory_order_relaxed);
            emit(offset, stamped.event);
            ++read;
        }
        out_read_.store(read, std::memory_order_release);
    }

    void render_chunk(std::uint64_t start, std::uint32_t frames, std::size_t index) noexcept {
        for (std::uint32_t c = 0; c < config_.input_channels; ++c)
            input_ptrs_[c] = input_.data() + static_cast<std::size_t>(c) * capacity_ + index;
        for (std::uint32_t c = 0; c < config_.output_channels; ++c) {
            output_ptrs_[c] = output_.data() + static_cast<std::size_t>(c) * capacity_ + index;
            std::fill_n(output_ptrs_[c], frames, 0.0f);
        }
        // Events in [start, start + frames), in position order.
        std::size_t count = 0;
        auto read = in_read_.load(std::memory_order_relaxed);
        const auto write = in_write_.load(std::memory_order_acquire);
        while (read != write && count < chunk_events_.size()) {
            const auto& stamped = events_in_[read & (events_in_.size() - 1)];
            if (stamped.position >= start + frames)
                break;
            chunk_events_[count++] = TimedEvent{
                static_cast<std::uint32_t>(stamped.position > start ? stamped.position - start : 0),
                stamped.event};
            ++read;
        }
        in_read_.store(read, std::memory_order_release);

        Chunk chunk;
        chunk.owner_ = this;
        chunk.start = start;
        chunk.frames = frames;
        chunk.input = BufferView<const float>(input_ptrs_.data(), config_.input_channels, frames);
        chunk.output = BufferView<float>(output_ptrs_.data(), config_.output_channels, frames);
        chunk.events = std::span<const TimedEvent>(chunk_events_.data(), count);
        job_(chunk);
    }

    void run() noexcept {
        while (!stop_.load(std::memory_order_acquire)) {
            work_.wait_for(
                [&] {
                    return stop_.load(std::memory_order_acquire) ||
                           submitted_.load(std::memory_order_acquire) >
                               produced_.load(std::memory_order_relaxed);
                },
                std::chrono::seconds(1));
            if (stop_.load(std::memory_order_acquire))
                break;
            pump();
        }
    }

    AsyncBlockWorkerConfig config_{};
    Job job_;
    bool prepared_ = false;
    std::uint64_t capacity_ = 0;
    std::vector<float> input_;
    std::vector<float> output_;
    std::vector<const float*> input_ptrs_;
    std::vector<float*> output_ptrs_;
    std::vector<Stamped> events_in_;
    std::vector<Stamped> events_out_;
    std::vector<TimedEvent> chunk_events_;

    // Timeline. submitted_: audio thread writes; produced_ / valid_from_:
    // worker writes; skip_to_: audio thread writes; delivered_: audio only.
    std::atomic<std::uint64_t> submitted_{0};
    std::atomic<std::uint64_t> produced_{0};
    std::atomic<std::uint64_t> skip_to_{0};
    std::atomic<std::uint64_t> valid_from_{0};
    std::uint64_t delivered_ = 0;
    std::uint64_t played_until_ = 0; // audio thread
    std::atomic<std::uint64_t> in_write_{0}, in_read_{0}, out_write_{0}, out_read_{0};

    std::uint32_t latency_blocks_ = 0;         // audio thread
    std::uint32_t applied_worker_latency_ = 0; // audio thread
    std::atomic<std::uint32_t> requested_latency_blocks_{0};
    std::atomic<std::uint32_t> worker_latency_{0};
    LatencyListener listener_ = nullptr;
    void* listener_context_ = nullptr;
    PullRateDetector detector_;

    std::atomic<std::uint64_t> produced_frames_{0}, miss_frames_{0}, overrun_frames_{0}, waits_{0},
        wait_timeouts_{0}, events_in_dropped_{0}, events_out_dropped_{0}, late_events_{0},
        latency_changes_{0};
    std::atomic<bool> fast_pull_{false};

    runtime::WakeSignal work_; // audio thread -> worker
    runtime::WakeSignal done_; // worker -> a waiting audio thread
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace pulp::audio
