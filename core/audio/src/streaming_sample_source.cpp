// streaming_sample_source.cpp — implementation of the streaming sample
// playback primitive declared in pulp/audio/streaming_sample_source.hpp.
//
// Layout invariant: the preload window holds source frames [0, preload_valid_).
// The ring holds the streamed tail in strict source order beginning at frame
// preload_valid_, so the audio thread can consume it sequentially without any
// notion of where the producer is. The background reader advances reader_pos_
// (its private cursor) and the audio thread advances play_pos_ (its private
// cursor); the only shared mutable state between them is the ring's lock-free
// FIFO plus a handful of relaxed atomics for telemetry.

#include <pulp/audio/streaming_sample_source.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace pulp::audio {

using namespace std::chrono_literals;

StreamingSampleSource::~StreamingSampleSource() { release(); }

bool StreamingSampleSource::prepare(const StreamingSampleSourceConfig& config,
                                    FrameReader reader) {
    release();

    if (config.channels == 0 || config.total_frames == 0 || !reader) {
        return false;
    }

    channels_ = config.channels;
    total_frames_ = config.total_frames;
    sample_rate_ = config.sample_rate;
    reader_ = std::move(reader);

    preload_valid_ = std::min(config.preload_frames, total_frames_);
    fully_resident_ = (preload_valid_ >= total_frames_);
    // A fully-resident source keeps the entire sample in the preload window.
    if (fully_resident_) preload_valid_ = total_frames_;

    loop_ = config.loop;
    loop_start_ = std::min(config.loop_start, total_frames_);
    loop_end_ = config.loop_end == 0 ? total_frames_
                                     : std::min(config.loop_end, total_frames_);
    if (loop_end_ <= loop_start_) {
        loop_start_ = 0;
        loop_end_ = total_frames_;
    }

    // Resident head. Read synchronously on the control thread so a note-on can
    // play immediately.
    if (preload_valid_ > 0) {
        preload_.resize(channels_, static_cast<std::size_t>(preload_valid_));
        const auto got = reader_(0, preload_.view(), preload_valid_);
        if (got < preload_valid_) {
            // Source produced fewer frames than declared — treat the realized
            // length as authoritative so we never read past valid data.
            preload_valid_ = got;
            total_frames_ = std::min(total_frames_, got);
            fully_resident_ = true;
            // Re-clamp the loop window to the realized length; otherwise a
            // resident loop would repeat frames past valid data (the preload
            // buffer was allocated at the pre-shrink size, so this is silent
            // tail, not an out-of-bounds read).
            loop_start_ = std::min(loop_start_, total_frames_);
            loop_end_ = std::min(loop_end_, total_frames_);
            if (loop_end_ <= loop_start_) {
                loop_start_ = 0;
                loop_end_ = total_frames_;
            }
        }
    }

    // Effective stream end starts at the (now finalized) declared length; the
    // background reader shrinks it if the source ends early mid-stream.
    eos_frame_.store(total_frames_, std::memory_order_relaxed);

    if (!fully_resident_) {
        const std::uint64_t ring_capacity =
            std::max<std::uint64_t>(config.ring_capacity_frames, 1);
        if (!ring_.prepare(channels_, ring_capacity)) {
            release();
            return false;
        }
        read_chunk_ = std::clamp<std::uint64_t>(
            config.read_chunk_frames == 0 ? 8192 : config.read_chunk_frames,
            1, ring_capacity);
        read_scratch_.resize(channels_, static_cast<std::size_t>(read_chunk_));
        reader_pos_.store(preload_valid_, std::memory_order_relaxed);

        // Prime the ring before any audio is pulled.
        while (pump_background() > 0) {
            if (ring_.free_frames() == 0) break;
        }

        use_thread_ = config.start_background_thread;
        if (use_thread_) {
            run_.store(true, std::memory_order_release);
            thread_ = std::thread([this] { reader_loop(); });
        }
    }

    play_pos_.store(0, std::memory_order_relaxed);
    prepared_ = true;
    return true;
}

void StreamingSampleSource::stop_thread() noexcept {
    if (thread_.joinable()) {
        run_.store(false, std::memory_order_release);
        cv_.notify_all();
        thread_.join();
    }
}

void StreamingSampleSource::release() noexcept {
    stop_thread();
    ring_.release();
    preload_ = Buffer<float>{};
    read_scratch_ = Buffer<float>{};
    reader_ = nullptr;
    channels_ = 0;
    total_frames_ = 0;
    sample_rate_ = 0;
    preload_valid_ = 0;
    read_chunk_ = 0;
    loop_ = false;
    loop_start_ = 0;
    loop_end_ = 0;
    fully_resident_ = false;
    prepared_ = false;
    use_thread_ = false;
    play_pos_.store(0, std::memory_order_relaxed);
    reader_pos_.store(0, std::memory_order_relaxed);
    eos_frame_.store(0, std::memory_order_relaxed);
    streamed_frames_.store(0, std::memory_order_relaxed);
    read_errors_.store(0, std::memory_order_relaxed);
    underrun_frames_.store(0, std::memory_order_relaxed);
}

bool StreamingSampleSource::reset() noexcept {
    if (!prepared_) return false;
    const bool had_thread = use_thread_;
    stop_thread();

    play_pos_.store(0, std::memory_order_relaxed);
    streamed_frames_.store(0, std::memory_order_relaxed);
    read_errors_.store(0, std::memory_order_relaxed);
    underrun_frames_.store(0, std::memory_order_relaxed);
    // Restore the optimistic end; a re-prime that hits an early source end will
    // shrink it again.
    eos_frame_.store(total_frames_, std::memory_order_relaxed);

    if (!fully_resident_) {
        ring_.reset();
        reader_pos_.store(preload_valid_, std::memory_order_relaxed);
        while (pump_background() > 0) {
            if (ring_.free_frames() == 0) break;
        }
        if (had_thread) {
            run_.store(true, std::memory_order_release);
            thread_ = std::thread([this] { reader_loop(); });
        }
    }
    return true;
}

void StreamingSampleSource::copy_from_preload(BufferView<float>& dest,
                                              std::uint64_t dest_offset,
                                              std::uint64_t src_start,
                                              std::uint64_t n) noexcept {
    const std::size_t count = static_cast<std::size_t>(n);
    const std::size_t doff = static_cast<std::size_t>(dest_offset);
    const std::size_t soff = static_cast<std::size_t>(src_start);
    const std::uint32_t copy_ch =
        std::min<std::uint32_t>(channels_,
                                static_cast<std::uint32_t>(dest.num_channels()));
    for (std::uint32_t ch = 0; ch < copy_ch; ++ch) {
        std::memcpy(dest.channel_ptr(ch) + doff,
                    preload_.channel(ch).data() + soff,
                    count * sizeof(float));
    }
    // Zero any surplus destination channels (dest wider than the source) over
    // the copied range, matching the streamed path (PlanarAudioRingBuffer::read
    // zero-fills surplus channels) so a mono source feeding a stereo voice bus
    // doesn't leave stale data on the extra channel.
    const std::uint32_t dest_ch =
        static_cast<std::uint32_t>(dest.num_channels());
    for (std::uint32_t ch = copy_ch; ch < dest_ch; ++ch) {
        std::memset(dest.channel_ptr(ch) + doff, 0, count * sizeof(float));
    }
}

std::uint64_t StreamingSampleSource::pull(BufferView<float> dest,
                                          std::uint64_t frames) noexcept {
    // Never write past the destination, even if the caller passes a frame count
    // larger than dest (copy_from_preload does a raw memcpy).
    if (frames > static_cast<std::uint64_t>(dest.num_samples())) {
        frames = static_cast<std::uint64_t>(dest.num_samples());
    }
    if (!prepared_ || channels_ == 0 || frames == 0) {
        if (!dest.empty()) dest.slice(0, static_cast<std::size_t>(frames)).clear();
        return 0;
    }

    std::uint64_t produced = 0;
    while (produced < frames) {
        std::uint64_t pos = play_pos_.load(std::memory_order_relaxed);

        std::uint64_t segment_end;
        if (loop_ && fully_resident_) {
            if (pos >= loop_end_) {
                pos = loop_start_;
                play_pos_.store(pos, std::memory_order_relaxed);
            }
            segment_end = loop_end_;
        } else {
            // Use the effective end (may have been shrunk by the reader on an
            // early source end) so a one-shot terminates at the real last frame
            // instead of waiting forever for tail frames that never arrive.
            const std::uint64_t end = eos_frame_.load(std::memory_order_acquire);
            if (pos >= end) break;  // one-shot reached the end
            segment_end = end;
        }

        const std::uint64_t to_segment_end = segment_end - pos;
        std::uint64_t want = std::min(frames - produced, to_segment_end);
        if (want == 0) break;

        if (fully_resident_ || pos < preload_valid_) {
            const std::uint64_t from_preload =
                fully_resident_ ? want : std::min(want, preload_valid_ - pos);
            copy_from_preload(dest, produced, pos, from_preload);
            produced += from_preload;
            play_pos_.store(pos + from_preload, std::memory_order_relaxed);
        } else {
            // Streamed tail: consume sequentially from the ring. The ring is a
            // strict FIFO whose Nth frame is source frame preload_valid_+N, so
            // on an underrun we must advance play_pos_ only by what we actually
            // read — otherwise the play cursor desyncs from the ring and all
            // later audio is time-shifted (and finished()/position() go wrong).
            const std::uint64_t avail =
                std::min<std::uint64_t>(want, ring_.available_frames());
            if (avail == 0) {
                // Underrun: hold position so the late-arriving frames still play
                // in order on the next pull. Remainder is zero-filled below.
                underrun_frames_.fetch_add(want, std::memory_order_relaxed);
                break;
            }
            BufferView<float> sub =
                dest.slice(static_cast<std::size_t>(produced),
                           static_cast<std::size_t>(avail));
            ring_.read(sub, avail);
            produced += avail;
            play_pos_.store(pos + avail, std::memory_order_relaxed);
            if (avail < want) {
                underrun_frames_.fetch_add(want - avail, std::memory_order_relaxed);
                break;  // ring drained mid-block
            }
        }
    }

    if (produced < frames) {
        dest.slice(static_cast<std::size_t>(produced),
                   static_cast<std::size_t>(frames - produced))
            .clear();
    }
    return produced;
}

std::uint64_t StreamingSampleSource::pump_background() noexcept {
    // Note: deliberately not gated on prepared_ — prepare() primes the ring via
    // this method before flipping prepared_ true.
    if (fully_resident_ || !reader_) return 0;

    const std::uint64_t rpos = reader_pos_.load(std::memory_order_relaxed);
    if (rpos >= total_frames_) return 0;  // tail fully streamed

    const std::uint64_t room = ring_.free_frames();
    if (room == 0) return 0;

    std::uint64_t want = std::min({read_chunk_, total_frames_ - rpos, room,
                                   static_cast<std::uint64_t>(
                                       read_scratch_.num_samples())});
    if (want == 0) return 0;

    BufferView<float> scratch = read_scratch_.view();
    // The FrameReader may allocate/decode and is not noexcept; a throw must not
    // escape this noexcept function (→ std::terminate). Treat it as a read error.
    std::uint64_t got = 0;
    try {
        got = reader_(rpos, scratch, want);
    } catch (...) {
        got = 0;
    }
    if (got == 0) {
        // EOF or read error before the declared end. All frames [0, rpos) are
        // already available (preload + what's in the ring), so rpos is the true
        // length. Publish it so the audio thread terminates one-shot playback
        // and reports finished() here instead of stalling on never-arriving
        // tail frames. release-store pairs with the acquire-load in pull().
        read_errors_.fetch_add(1, std::memory_order_relaxed);
        eos_frame_.store(rpos, std::memory_order_release);
        reader_pos_.store(total_frames_, std::memory_order_relaxed);
        return 0;
    }
    const std::uint64_t clamped = std::min(got, want);
    const std::uint64_t written = ring_.write(read_scratch_.view(), clamped);
    reader_pos_.store(rpos + written, std::memory_order_relaxed);
    streamed_frames_.fetch_add(written, std::memory_order_relaxed);
    return written;
}

void StreamingSampleSource::reader_loop() noexcept {
    while (run_.load(std::memory_order_acquire)) {
        const std::uint64_t pushed = pump_background();
        std::unique_lock<std::mutex> lock(mutex_);
        // Self-paced poll: a ring sized for tens of ms is refilled long before
        // it drains, so a short sleep keeps the audio thread free of any
        // wake-up signaling (notify from the audio thread would not be
        // RT-safe). Wakes immediately on shutdown.
        cv_.wait_for(lock, pushed > 0 ? 1ms : 5ms,
                     [this] { return !run_.load(std::memory_order_acquire); });
    }
}

void StreamingSampleSource::notify_reader() noexcept { cv_.notify_all(); }

bool StreamingSampleSource::finished() const noexcept {
    // A zero-length source is immediately finished — there is nothing to play or
    // loop over. This happens when a looping voice's preload read yields 0 frames
    // (total_frames_ shrinks to 0); without this guard the resident-loop branch
    // below would loop silence forever and never report finished, leaking the
    // voice. (total_frames_ is immutable after prepare(), so this read is safe.)
    if (total_frames_ == 0) return true;
    if (loop_ && fully_resident_) return false;  // resident loop never ends
    // Compare against the effective end so a stream that ended early (reader
    // returned 0 mid-tail) still reports finished once its realized frames have
    // played out — not only when play_pos_ reaches the optimistic declared total.
    return play_pos_.load(std::memory_order_relaxed) >=
           eos_frame_.load(std::memory_order_acquire);
}

StreamingSampleSource::Stats StreamingSampleSource::stats() const noexcept {
    Stats s;
    s.underrun_frames = underrun_frames_.load(std::memory_order_relaxed);
    s.ring_available_frames = ring_.available_frames();
    s.streamed_frames = streamed_frames_.load(std::memory_order_relaxed);
    s.read_errors = read_errors_.load(std::memory_order_relaxed);
    s.streaming_active =
        !fully_resident_ &&
        reader_pos_.load(std::memory_order_relaxed) < total_frames_;
    return s;
}

}  // namespace pulp::audio
