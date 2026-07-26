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
/// than restated. What this adds is the declaration it is prepared against and
/// an honest starvation count against the declaration. The underlying stream's
/// underrun count describes ring delivery; it does not carry the production
/// declaration or distinguish content that permanently missed its deadline.
///
/// Thread model:
///   * prepare()/release()  — control thread, allocates.
///   * pull()               — audio thread, RT-safe after prepare:
///                            allocation-free, lock-free, wait-free,
///                            zero-fills whatever the producer did not supply.
///   * pump_background()    — background or host thread. The producer runs here
///                            and may allocate, lock, block, and decode.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <utility>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/streaming_sample_source.hpp>
#include <pulp/timeline/production_mode.hpp>

namespace pulp::playback {

/// Caller-supplied producer of buffered content. Fills @p destination with up
/// to @p frames frames of planar float audio starting at content frame
/// @p start_frame and returns how many frames it actually produced. Runs only
/// on the background/pump thread, never on the audio thread.
using BufferedContentProducer =
    std::function<std::uint64_t(std::uint64_t start_frame, audio::BufferView<float> destination,
                                std::uint64_t frames, audio::FrameReaderStopToken stop_token)>;

struct BufferedContentSourceConfig {
    std::uint32_t channels = 0;
    /// Frames the content declares it will produce. Starvation is measured
    /// against this, so a producer that stops early is counted as starving
    /// rather than as having finished.
    std::uint64_t declared_frames = 0;
    std::uint32_t sample_rate = 0;
    /// Ring capacity in frames. Zero derives it from the declaration's
    /// wall-clock lookahead at @ref sample_rate.
    std::uint64_t ring_capacity_frames = 0;
    /// How much the pump asks the producer for per refill step.
    std::uint64_t produce_chunk_frames = 4096;
    /// Spawn an owned producer thread. Off by default so a caller drives
    /// `pump_background()` deterministically.
    bool start_background_thread = false;
};

class BufferedContentSource {
  public:
    BufferedContentSource() = default;

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
        /// Ring-level underruns reported by the underlying stream.
        std::uint64_t ring_underrun_frames = 0;
        /// Frames currently ready for the audio thread.
        std::uint64_t ring_available_frames = 0;
        /// Producer calls that returned nothing.
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
            !producer)
            return false;

        audio::StreamingSampleSourceConfig stream_config;
        stream_config.channels = config.channels;
        stream_config.total_frames = config.declared_frames;
        stream_config.sample_rate = config.sample_rate;
        // No resident head: buffered content is produced on demand, so every
        // frame must travel through the ring where it can be counted.
        stream_config.preload_frames = 0;
        stream_config.ring_capacity_frames =
            config.ring_capacity_frames != 0
                ? config.ring_capacity_frames
                : std::max<std::uint64_t>(lookahead_frames(declaration, config.sample_rate),
                                          config.produce_chunk_frames);
        stream_config.read_chunk_frames = config.produce_chunk_frames;
        stream_config.start_background_thread = config.start_background_thread;
        // Generated content is tied to playhead deadlines. Once a frame has
        // been emitted as starvation silence, producing it later must not move
        // subsequent content out of time.
        stream_config.advance_on_underrun = true;

        audio::FrameReaderBinding binding;
        binding.read = std::move(producer);
        if (!stream_.prepare(stream_config, std::move(binding)))
            return false;

        declaration_ = declaration;
        declared_frames_ = config.declared_frames;
        prepared_ = true;
        return true;
    }

    /// Control thread. Stops the producer and frees all storage.
    void release() noexcept {
        stream_.release();
        declaration_ = {};
        declared_frames_ = 0;
        prepared_ = false;
        produced_frames_.store(0, std::memory_order_relaxed);
        starved_frames_.store(0, std::memory_order_relaxed);
        starvation_events_.store(0, std::memory_order_relaxed);
    }

    /// Audio thread. Emits up to @p frames frames into @p destination and
    /// zero-fills whatever the producer did not supply, counting the shortfall.
    /// Returns the number of produced (non-zero-filled) frames.
    std::uint64_t pull(audio::BufferView<float> destination, std::uint64_t frames) noexcept {
        frames = std::min<std::uint64_t>(frames, destination.num_samples());
        const std::uint64_t position = stream_.position();
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
    /// Audio-thread position in content frames.
    std::uint64_t position() const noexcept {
        return stream_.position();
    }
    /// True once every declared frame has been accounted for, produced or not.
    bool exhausted() const noexcept {
        return stream_.position() >= declared_frames_;
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
    audio::StreamingSampleSource stream_;
    timeline::ProductionDeclaration declaration_;
    std::uint64_t declared_frames_ = 0;
    bool prepared_ = false;

    std::atomic<std::uint64_t> produced_frames_{0};
    std::atomic<std::uint64_t> starved_frames_{0};
    std::atomic<std::uint64_t> starvation_events_{0};
};

} // namespace pulp::playback
