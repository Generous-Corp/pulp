#pragma once

/// @file buffered_content_source.hpp
/// A ring-backed source for content produced ahead of the playhead, with
/// starvation counted against what the content declared rather than against
/// what its producer conceded.
///
/// This is the buffering half of `timeline::ProductionMode::Buffered` and
/// nothing else: it owns a ring, a producer callback, and counters. It does not
/// schedule, does not follow a transport, and does not know what a producer
/// computes. Pulp supplies timing and buffering on the audio side of
/// `Producer`; everything on the other side of that call belongs to whoever
/// wrote the producer, including any dependency it needs. Pulp ships no
/// inference runtime, no model weights, and no producer that performs
/// inference.
///
/// The ring, the RT pull contract, the background-refill pump, and the
/// cooperative stop token are `audio::StreamingSampleSource`'s, reused rather
/// than restated. What this adds is the declaration it is prepared against, an
/// honest starvation count against the declaration, and repositioning that
/// stays honest under concurrency: a seek or loop wrap begins a strictly newer
/// production epoch, any producer call still in flight is stopped within one
/// chunk, and frames it produced for the superseded epoch are discarded before
/// they can reach the new ring. The underlying stream's underrun count
/// describes ring delivery; it does not carry the production declaration or
/// distinguish content that permanently missed its deadline.
///
/// Thread model:
///   * prepare()/release()        — control thread, allocates.
///   * seek()/loop_wrap()         — control thread, reallocates. No pull() may
///                                  be in flight; an in-flight producer call is
///                                  tolerated and stopped within one chunk.
///   * cancel_production()        — control thread, waits at most one chunk.
///   * pull()                     — audio thread, RT-safe after prepare:
///                                  allocation-free, lock-free, wait-free,
///                                  zero-fills whatever the producer did not
///                                  supply.
///   * pump_background()          — background or host thread. The producer
///                                  runs here and may allocate, lock, block,
///                                  and decode.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <utility>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/streaming_sample_source.hpp>
#include <pulp/timeline/production_mode.hpp>

namespace pulp::playback {

/// Identifies one continuous production interval. Zero is never a valid
/// epoch; every seek or loop wrap must begin a strictly newer one, so a frame
/// produced under an older epoch can always be recognized as stale.
using BufferedContentEpoch = std::uint64_t;

/// Caller-supplied producer of buffered content. Fills @p destination with up
/// to @p frames frames of planar float audio starting at content frame
/// @p start_frame and returns how many frames it actually produced. Runs only
/// on the background/pump thread, never on the audio thread.
///
/// @p epoch is the production epoch this call belongs to; a producer that
/// caches work across calls must key that work by epoch, because a call made
/// for a superseded epoch is discarded instead of reaching the ring.
/// @p stop_token asks the call to return early — on cancellation, teardown,
/// or reposition — and honoring it keeps those paths bounded by one chunk.
using BufferedContentProducer =
    std::function<std::uint64_t(BufferedContentEpoch epoch, std::uint64_t start_frame,
                                audio::BufferView<float> destination,
                                std::uint64_t frames, audio::FrameReaderStopToken stop_token)>;

struct BufferedContentSourceConfig {
    std::uint32_t channels = 0;
    /// Frames the content declares it will produce. Starvation is measured
    /// against this, so a producer that stops early is counted as starving
    /// rather than as having finished.
    std::uint64_t declared_frames = 0;
    std::uint32_t sample_rate = 0;
    /// Largest block the audio thread will request in one pull.
    std::uint64_t max_pull_frames = 0;
    /// Ring capacity in frames. Zero derives it from the larger of the
    /// declaration's wall-clock lookahead, @ref max_pull_frames, and
    /// @ref preroll_frames.
    std::uint64_t ring_capacity_frames = 0;
    /// How much the pump asks the producer for per refill step.
    std::uint64_t produce_chunk_frames = 4096;
    /// Frames synchronously produced into the ring during prepare() and again
    /// after every seek, guaranteed present before the next pull. Zero defers
    /// all production to the pump. A producer that cannot fill the declared
    /// preroll synchronously fails prepare()/seek() — the guarantee is
    /// enforced, not sampled.
    std::uint64_t preroll_frames = 0;
    /// Declared loop region [loop_start_frame, loop_end_frame). Both zero
    /// declares no loop. The source never wraps on its own: the transport owns
    /// looping and calls loop_wrap() at the boundary, which restarts
    /// production at the loop head under a strictly newer epoch.
    std::uint64_t loop_start_frame = 0;
    std::uint64_t loop_end_frame = 0;
    /// First production epoch. Must be nonzero; later seeks and loop wraps
    /// must each pass a strictly newer epoch than the one before.
    BufferedContentEpoch initial_epoch = 1;
    /// Spawn an owned producer thread. Off by default so a caller drives
    /// `pump_background()` deterministically.
    bool start_background_thread = false;
};

class BufferedContentSource {
  public:
    BufferedContentSource() = default;
    ~BufferedContentSource() { release(); }

    BufferedContentSource(const BufferedContentSource&) = delete;
    BufferedContentSource& operator=(const BufferedContentSource&) = delete;

    struct Stats {
        /// Frames the producer supplied and the audio thread consumed.
        std::uint64_t produced_frames = 0;
        /// Declared frames the audio thread had to zero-fill because they had
        /// not been produced in time.
        std::uint64_t starved_frames = 0;
        /// Pull calls that came up short of what the declaration promised.
        std::uint64_t starvation_events = 0;
        /// Ring-level underruns reported by the underlying stream. Resets on
        /// every seek or loop wrap, with the ring it describes.
        std::uint64_t ring_underrun_frames = 0;
        /// Frames currently ready for the audio thread.
        std::uint64_t ring_available_frames = 0;
        /// Producer exceptions. A zero return is transient backpressure and is
        /// retried without incrementing this counter.
        std::uint64_t producer_errors = 0;
    };

    /// Control thread. Rejects a declaration that is not coherent, or that does
    /// not declare `Buffered` — synchronous content has no ring to prepare.
    bool prepare(const timeline::ProductionDeclaration& declaration,
                 const BufferedContentSourceConfig& config, BufferedContentProducer producer) {
        release();
        if (declaration.mode != timeline::ProductionMode::Buffered)
            return false;
        if (timeline::validate_production_declaration(declaration) !=
            timeline::ProductionDeclarationErrorCode::None)
            return false;
        if (config.channels == 0 || config.declared_frames == 0 || config.sample_rate == 0 ||
            config.max_pull_frames == 0 || !producer)
            return false;
        if (config.initial_epoch == 0)
            return false;
        if (config.preroll_frames > config.declared_frames)
            return false;
        const bool declares_loop = config.loop_start_frame != 0 || config.loop_end_frame != 0;
        if (declares_loop && (config.loop_start_frame >= config.loop_end_frame ||
                              config.loop_end_frame > config.declared_frames))
            return false;

        const std::uint64_t declared_lookahead =
            std::max<std::uint64_t>(
                lookahead_frames(declaration, config.sample_rate), 1);
        if (config.ring_capacity_frames != 0 &&
            (config.ring_capacity_frames < config.max_pull_frames ||
             config.ring_capacity_frames < config.preroll_frames))
            return false;

        declaration_ = declaration;
        declared_frames_ = config.declared_frames;
        preroll_frames_ = config.preroll_frames;
        loop_start_frame_ = config.loop_start_frame;
        loop_end_frame_ = config.loop_end_frame;
        declared_lookahead_ = declared_lookahead;
        config_ = config;
        producer_ = std::move(producer);
        epoch_.store(config.initial_epoch, std::memory_order_relaxed);
        base_frame_ = 0;
        if (!start_stream()) {
            release();
            return false;
        }
        prepared_ = true;
        return true;
    }

    /// Control thread. Stops the producer and frees all storage.
    void release() noexcept {
        // Ask an in-flight producer call to cut short before the stream joins
        // its thread, so the wait stays bounded by the remainder of one chunk.
        producer_stop_requested_.store(true, std::memory_order_release);
        stream_.release();
        producer_stop_requested_.store(false, std::memory_order_release);
        declaration_ = {};
        declared_frames_ = 0;
        preroll_frames_ = 0;
        loop_start_frame_ = 0;
        loop_end_frame_ = 0;
        declared_lookahead_ = 0;
        config_ = {};
        producer_ = nullptr;
        epoch_.store(0, std::memory_order_relaxed);
        base_frame_ = 0;
        prepared_ = false;
        produced_frames_.store(0, std::memory_order_relaxed);
        starved_frames_.store(0, std::memory_order_relaxed);
        starvation_events_.store(0, std::memory_order_relaxed);
    }

    /// Control thread; no pull() may be in flight. Repositions production to
    /// content frame @p frame under @p epoch: the in-flight producer call is
    /// asked to stop and whatever it produced for the superseded epoch is
    /// discarded with the old ring, a fresh ring is primed at the new
    /// position, and the declared preroll is filled before the call returns.
    /// @p epoch must be nonzero and strictly newer than current_epoch().
    /// Returns false on an invalid epoch, an out-of-range frame, or a preroll
    /// that cannot fit the remaining content — all without disturbing the
    /// current position — and false leaving the source released when the
    /// producer could not fill the declared preroll.
    bool seek(std::uint64_t frame, BufferedContentEpoch epoch) {
        if (!prepared_)
            return false;
        if (epoch == 0 || epoch <= current_epoch())
            return false;
        if (frame >= declared_frames_)
            return false;
        if (preroll_frames_ > declared_frames_ - frame)
            return false;

        // Publish the epoch before teardown: any producer call still running
        // against the old ring becomes recognizable as superseded, and the
        // stop request keeps its remaining run inside the current chunk.
        epoch_.store(epoch, std::memory_order_release);
        producer_stop_requested_.store(true, std::memory_order_release);
        stream_.release();
        producer_stop_requested_.store(false, std::memory_order_release);
        base_frame_ = frame;
        if (!start_stream()) {
            release();
            return false;
        }
        return true;
    }

    /// Control thread. Restarts production at the declared loop head under a
    /// strictly newer epoch — the wrap half of a loop the transport owns.
    /// Returns false when no loop is declared or the epoch is not strictly
    /// newer.
    bool loop_wrap(BufferedContentEpoch epoch) {
        if (!prepared_ || (loop_start_frame_ == 0 && loop_end_frame_ == 0))
            return false;
        return seek(loop_start_frame_, epoch);
    }

    /// Control thread. Asks the in-flight producer call to stop through its
    /// token and waits for it — bounded by one chunk: a cooperative producer
    /// returns early, one that never checks its token finishes the chunk. The
    /// interrupted chunk publishes nothing, so produced and starved counters
    /// stay coherent with the play cursor, and the next pump retries the same
    /// position. Cancellation does not latch: production resumes on the next
    /// pump. Use seek() or release() to end production.
    void cancel_production() noexcept {
        if (!prepared_)
            return;
        producer_stop_requested_.store(true, std::memory_order_release);
        while (producer_calls_in_flight_.load(std::memory_order_acquire) != 0)
            std::this_thread::yield();
        producer_stop_requested_.store(false, std::memory_order_release);
    }

    /// Audio thread. Emits up to @p frames frames into @p destination and
    /// zero-fills whatever the producer did not supply, counting the shortfall.
    /// Returns the number of produced (non-zero-filled) frames.
    std::uint64_t pull(audio::BufferView<float> destination, std::uint64_t frames) noexcept {
        frames = std::min<std::uint64_t>(frames, destination.num_samples());
        const std::uint64_t position = base_frame_ + stream_.position();
        const std::uint64_t remaining =
            declared_frames_ > position ? declared_frames_ - position : 0;
        const std::uint64_t expected = std::min(frames, remaining);
        const std::uint64_t produced = stream_.pull(destination, frames);
        produced_frames_.fetch_add(produced, std::memory_order_relaxed);
        if (produced < expected) {
            starved_frames_.fetch_add(expected - produced, std::memory_order_relaxed);
            starvation_events_.fetch_add(1, std::memory_order_relaxed);
        }
        return produced;
    }

    /// Background or host thread. Runs the producer once toward topping the ring
    /// up, and returns the number of frames it pushed. Drive this directly when
    /// the owned thread is disabled.
    std::uint64_t pump_background() noexcept {
        return stream_.pump_background();
    }

    bool prepared() const noexcept {
        return prepared_;
    }
    const timeline::ProductionDeclaration& declaration() const noexcept {
        return declaration_;
    }
    std::uint64_t declared_frames() const noexcept {
        return declared_frames_;
    }
    /// The epoch production currently belongs to. RT-safe.
    BufferedContentEpoch current_epoch() const noexcept {
        return epoch_.load(std::memory_order_acquire);
    }
    /// Declared loop region; both zero when no loop is declared.
    std::uint64_t loop_start_frame() const noexcept {
        return loop_start_frame_;
    }
    std::uint64_t loop_end_frame() const noexcept {
        return loop_end_frame_;
    }
    /// Audio-thread position in content frames of the current epoch.
    std::uint64_t position() const noexcept {
        return base_frame_ + stream_.position();
    }
    /// True once every declared frame has been accounted for, produced or not.
    bool exhausted() const noexcept {
        return position() >= declared_frames_;
    }

    Stats stats() const noexcept {
        const auto stream_stats = stream_.stats();
        Stats result;
        result.produced_frames = produced_frames_.load(std::memory_order_relaxed);
        result.starved_frames = starved_frames_.load(std::memory_order_relaxed);
        result.starvation_events = starvation_events_.load(std::memory_order_relaxed);
        result.ring_underrun_frames = stream_stats.underrun_frames;
        result.ring_available_frames = stream_stats.ring_available_frames;
        result.producer_errors = stream_stats.read_errors;
        return result;
    }

    /// Frames the declaration's wall-clock lookahead covers at @p sample_rate.
    static std::uint64_t lookahead_frames(const timeline::ProductionDeclaration& declaration,
                                          std::uint32_t sample_rate) noexcept {
        return declaration.lookahead_ms * static_cast<std::uint64_t>(sample_rate) / 1000u;
    }

  private:
    /// Builds and primes the underlying stream at the current base frame. The
    /// producer is rebound so stream-relative requests land on content frames
    /// of the current epoch. Returns false when the stream refuses the
    /// configuration or the producer cannot fill the declared preroll.
    bool start_stream() {
        audio::StreamingSampleSourceConfig stream_config;
        stream_config.channels = config_.channels;
        stream_config.total_frames = declared_frames_ - base_frame_;
        stream_config.sample_rate = config_.sample_rate;
        // No resident head: buffered content is produced on demand, so every
        // frame must travel through the ring where it can be counted.
        stream_config.preload_frames = 0;
        stream_config.ring_capacity_frames =
            config_.ring_capacity_frames != 0
                ? config_.ring_capacity_frames
                : std::max({declared_lookahead_, config_.max_pull_frames, preroll_frames_});
        stream_config.read_chunk_frames = config_.produce_chunk_frames;
        stream_config.start_background_thread = config_.start_background_thread;
        // Generated content is tied to playhead deadlines. Once a frame has
        // been emitted as starvation silence, producing it later must not move
        // subsequent content out of time.
        stream_config.underrun_policy =
            audio::StreamingUnderrunPolicy::AdvancePosition;
        stream_config.max_read_ahead_frames =
            std::max(declared_lookahead_, preroll_frames_);

        audio::FrameReaderBinding binding;
        binding.read = [this](std::uint64_t stream_frame, audio::BufferView<float> destination,
                              std::uint64_t frames, audio::FrameReaderStopToken stop_token) {
            return produce_for_stream(stream_frame, destination, frames, stop_token);
        };
        if (!stream_.prepare(stream_config, std::move(binding)))
            return false;
        // The stream primes its ring synchronously during prepare; the
        // declared preroll is the part of that fill the source guarantees.
        if (preroll_frames_ > 0 &&
            stream_.stats().ring_available_frames < preroll_frames_) {
            stream_.release();
            return false;
        }
        return true;
    }

    /// Runs the caller's producer for one stream-relative request. A call that
    /// straddles a cancellation or teardown publishes nothing: it was asked to
    /// stop through the wrapper token, and whatever it returned anyway answers
    /// a question the current ring no longer asks. Seeks and loop wraps set
    /// the same stop flag before releasing the old ring (which joins this
    /// call), so no epoch comparison is needed here — a stale-epoch return is
    /// unreachable by contract.
    std::uint64_t produce_for_stream(std::uint64_t stream_frame,
                                     audio::BufferView<float> destination,
                                     std::uint64_t frames,
                                     audio::FrameReaderStopToken stream_stop_token) {
        if (stream_stop_token.stop_requested())
            return 0;
        const BufferedContentEpoch call_epoch = epoch_.load(std::memory_order_acquire);
        producer_calls_in_flight_.fetch_add(1, std::memory_order_acq_rel);
        struct InFlight {
            std::atomic<std::uint32_t>& count;
            ~InFlight() {
                count.fetch_sub(1, std::memory_order_acq_rel);
            }
        } in_flight{producer_calls_in_flight_};
        const std::uint64_t produced =
            producer_(call_epoch, base_frame_ + stream_frame, destination, frames,
                      audio::FrameReaderStopToken{producer_stop_requested_});
        // The producer may ignore the stop token and return frames anyway; an
        // interrupted chunk publishes nothing either way.
        if (producer_stop_requested_.load(std::memory_order_acquire))
            return 0;
        return produced;
    }

    audio::StreamingSampleSource stream_;
    timeline::ProductionDeclaration declaration_;
    std::uint64_t declared_frames_ = 0;
    std::uint64_t preroll_frames_ = 0;
    std::uint64_t loop_start_frame_ = 0;
    std::uint64_t loop_end_frame_ = 0;
    std::uint64_t declared_lookahead_ = 0;
    BufferedContentSourceConfig config_{};
    BufferedContentProducer producer_;
    /// Content frame the current stream's frame zero sits at. Written only by
    /// the control thread while no pull or producer call referencing the old
    /// value can still be running.
    std::uint64_t base_frame_ = 0;
    bool prepared_ = false;

    std::atomic<BufferedContentEpoch> epoch_{0};
    std::atomic<bool> producer_stop_requested_{false};
    std::atomic<std::uint32_t> producer_calls_in_flight_{0};

    std::atomic<std::uint64_t> produced_frames_{0};
    std::atomic<std::uint64_t> starved_frames_{0};
    std::atomic<std::uint64_t> starvation_events_{0};
};

} // namespace pulp::playback
