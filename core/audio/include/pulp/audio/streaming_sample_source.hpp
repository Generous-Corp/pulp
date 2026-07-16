#pragma once

/// @file streaming_sample_source.hpp
/// Real-time-safe streaming sample playback primitive.
///
/// Plays a long sample without holding the whole thing resident in RAM, and
/// without ever blocking the audio thread on disk/decoder I/O. The model is a
/// resident *preload window* (the head of the sample, kept in memory for a
/// glitch-free note-on) followed by a *streamed tail* served from a
/// single-producer/single-consumer ring buffer that a background reader thread
/// keeps topped up.
///
/// Short samples (total length <= preload window) take a zero-overhead
/// fully-resident fast path: no ring, no background thread, free looping. This
/// makes the primitive useful for one sequential long-sample transport beside
/// resident short material. Polyphonic samplers should share decoded pages
/// through SampleStreamCacheService rather than instantiate this source, its
/// ring, and its worker once per voice (see docs/guides/streaming-sample-source.md).
///
/// Thread model:
///   * prepare()/release()/reset()  — control thread, may allocate, never on
///                                     the audio thread.
///   * pull()                       — audio thread, RT-safe after prepare:
///                                     allocation-free, lock-free, wait-free,
///                                     zero-fills on underrun.
///   * the background reader         — one owned std::thread that calls the
///                                     caller-supplied FrameReader off the audio
///                                     thread. Disable it (start_background_thread
///                                     = false) and drive pump_background()
///                                     manually for deterministic tests.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/planar_audio_ring_buffer.hpp>

namespace pulp::audio {

/// Caller-supplied random-access frame reader. Fills @p dest with up to
/// @p frames frames of planar float audio starting at source frame
/// @p start_frame and returns the number of frames actually produced (a short
/// return signals end-of-source or a read error). Runs ONLY on the background
/// reader thread (or the pump_background() caller) — never on the audio thread —
/// so it may allocate, lock, seek, and decode freely.
///
/// Decoupling the source behind a callback keeps this primitive independent of
/// any particular file/decoder: a WAV memory-map, a chunked decoder, a
/// procedurally generated stream, or a pre-rendered (e.g. tempo-stretched)
/// buffer all satisfy the same contract.
using FrameReader =
    std::function<std::uint64_t(std::uint64_t start_frame,
                                BufferView<float> dest,
                                std::uint64_t frames)>;

struct StreamingSampleSourceConfig {
    std::uint32_t channels = 0;          ///< Source channel count (1 or 2 typical).
    std::uint64_t total_frames = 0;      ///< Total length of the source in frames.
    std::uint32_t sample_rate = 0;       ///< Source sample rate (carried, not resampled here).

    /// Resident head length in frames. Clamped to [0, total_frames]. The head
    /// guarantees a glitch-free note-on even if the reader thread is busy. When
    /// preload_frames >= total_frames the source is fully resident.
    std::uint64_t preload_frames = 0;

    /// Streamed-tail ring capacity in frames. Must comfortably exceed the audio
    /// block size; a few hundred ms is typical. Ignored when fully resident.
    std::uint64_t ring_capacity_frames = 0;

    /// Background read granularity in frames (how much the reader pulls per
    /// refill step). Clamped to the ring capacity. Ignored when fully resident.
    std::uint64_t read_chunk_frames = 8192;

    /// Loop the playback. Honored only for fully-resident sources; streamed
    /// sources play once and then report finished().
    bool loop = false;
    std::uint64_t loop_start = 0;        ///< Loop start frame (resident-loop only).
    std::uint64_t loop_end = 0;          ///< Loop end frame, exclusive; 0 means total_frames.

    /// Spawn the owned background reader thread in prepare(). Set false to drive
    /// refills manually via pump_background() (deterministic testing).
    bool start_background_thread = true;
};

class StreamingSampleSource {
public:
    StreamingSampleSource() = default;
    ~StreamingSampleSource();

    StreamingSampleSource(const StreamingSampleSource&) = delete;
    StreamingSampleSource& operator=(const StreamingSampleSource&) = delete;

    /// Allocate buffers, synchronously fill the preload window via @p reader, and
    /// (optionally) start the background reader thread. Control thread only.
    /// Returns false on invalid config or a failed preload read.
    bool prepare(const StreamingSampleSourceConfig& config, FrameReader reader);

    /// Stop the reader thread and free all storage. Control thread only.
    void release() noexcept;

    /// Rewind to the start of the source (or loop_start) and refill the tail.
    /// Control thread / quiescent only — not RT-safe (it may re-read and resets
    /// the background cursor). Returns false if not prepared.
    bool reset() noexcept;

    // ── Audio thread (RT-safe after prepare) ────────────────────────────────

    /// Emit @p frames sequential frames into @p dest, advancing the play cursor.
    /// Returns the number of non-silent frames produced; the remainder of
    /// @p dest is zero-filled (end-of-source, or a streaming underrun, which is
    /// counted in stats()). Allocation-free, lock-free, wait-free.
    std::uint64_t pull(BufferView<float> dest, std::uint64_t frames) noexcept;

    /// True once a non-looping source has played to its end and the tail is
    /// drained. Always false for a resident loop. RT-safe.
    bool finished() const noexcept;

    /// Current play position in source frames. RT-safe.
    std::uint64_t position() const noexcept {
        return play_pos_.load(std::memory_order_relaxed);
    }

    /// True when the whole source fits in the preload window (no streaming).
    bool fully_resident() const noexcept { return fully_resident_; }

    bool prepared() const noexcept { return prepared_; }
    std::uint32_t channels() const noexcept { return channels_; }
    std::uint64_t total_frames() const noexcept { return total_frames_; }

    // ── Background refill ───────────────────────────────────────────────────

    /// Top the streamed-tail ring up toward full using the FrameReader. Called
    /// automatically by the owned thread; call it directly when the thread is
    /// disabled (tests). Reads at most one read_chunk per invocation that there
    /// is room and remaining source for. No-op for resident sources. Returns the
    /// number of frames pushed into the ring this call.
    std::uint64_t pump_background() noexcept;

    struct Stats {
        std::uint64_t underrun_frames = 0;   ///< Frames the audio thread had to zero-fill mid-stream.
        std::uint64_t ring_available_frames = 0;
        std::uint64_t streamed_frames = 0;   ///< Source frames pushed through the ring since prepare/reset.
        std::uint64_t read_errors = 0;       ///< Background reader short/zero returns.
        bool streaming_active = false;       ///< Tail not yet fully streamed.
    };
    Stats stats() const noexcept;

private:
    void stop_thread() noexcept;
    void reader_loop() noexcept;
    void notify_reader() noexcept;
    // Copy [src_start, src_start+n) from the resident preload window into the
    // [dest_offset, dest_offset+n) region of dest. Audio thread.
    void copy_from_preload(BufferView<float>& dest, std::uint64_t dest_offset,
                           std::uint64_t src_start, std::uint64_t n) noexcept;

    // Immutable after prepare.
    std::uint32_t channels_ = 0;
    std::uint64_t total_frames_ = 0;
    std::uint32_t sample_rate_ = 0;
    std::uint64_t preload_valid_ = 0;
    std::uint64_t read_chunk_ = 0;
    bool loop_ = false;
    std::uint64_t loop_start_ = 0;
    std::uint64_t loop_end_ = 0;
    bool fully_resident_ = false;
    bool prepared_ = false;
    bool use_thread_ = false;

    Buffer<float> preload_;          ///< Resident head, frames [0, preload_valid_).
    PlanarAudioRingBuffer ring_;     ///< Streamed tail, frames [preload_valid_, ...).
    Buffer<float> read_scratch_;     ///< Background reader scratch (off audio thread).
    FrameReader reader_;

    std::atomic<std::uint64_t> play_pos_{0};    ///< Audio-thread owned: next source frame to emit.
    std::atomic<std::uint64_t> reader_pos_{0};  ///< Background owned: next source frame to push.
    // Effective end-of-stream in source frames: starts at total_frames_ and is
    // shrunk by the background reader to the realized length if the FrameReader
    // signals an early end/error mid-stream (a short/zero return before the
    // declared total). The audio thread reads it to terminate one-shot playback
    // and report finished() at the real end instead of stalling forever waiting
    // for frames that will never arrive. Written only by the background reader,
    // read by the audio thread — hence atomic (total_frames_ itself stays an
    // immutable-after-prepare plain member that the audio thread can read freely).
    std::atomic<std::uint64_t> eos_frame_{0};
    std::atomic<std::uint64_t> streamed_frames_{0};
    std::atomic<std::uint64_t> read_errors_{0};
    std::atomic<std::uint64_t> underrun_frames_{0};  ///< Audio-thread zero-fill on a streaming miss.

    std::thread thread_;
    std::atomic<bool> run_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
};

}  // namespace pulp::audio
