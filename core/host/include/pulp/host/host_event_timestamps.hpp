#pragma once

/// Projection of native, block-relative host event times into the
/// epoch-local absolute sample domain consumed by `HostEventIngress`.
///
/// Plugin formats timestamp events relative to the block being processed
/// (CLAP `clap_event_header_t::time`, VST3 `sampleOffset`, AU frame offsets)
/// and describe the block's place on the host timeline separately: a
/// monotonic sample counter (CLAP `clap_process_t::steady_time`) plus an
/// optional transport position. `HostBlockTimeline` turns that pair into a
/// `HostBlockWindow` per block and maps each event time to an absolute sample
/// inside it.
///
/// Rules:
///  * Consecutive blocks on an unbroken clock are contiguous in one epoch.
///  * A forward jump of the monotonic clock (the host skipped blocks) stays in
///    the same epoch and advances the window by the gap; `HostEventIngress`
///    evicts staged records left behind such a jump.
///  * A seek or loop (an explicit host discontinuity, or a transport position
///    that does not follow the previous playing block), a monotonic clock that
///    runs backwards, or a change in clock availability starts a new epoch at
///    sample zero. The ingress clears its staging area on a new epoch, so a
///    record timed against the old timeline can never be delivered against
///    the new one.
///  * An event time at or past the block's frame count is refused with
///    `OutOfRange`; it is never clamped into the block.
///  * An event projected against a window older than the current block is
///    refused with `Late`, and one from a retired epoch with `StaleEpoch`.
///
/// Everything here is fixed-size, `noexcept`, and allocation-free, so it may
/// run on the audio thread. It owns no queue and schedules nothing.

#include <pulp/host/graph_event_contract.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace pulp::host {

/// Timing facts a host supplies for one process block.
struct HostBlockClock {
    /// Host monotonic sample counter at the first frame of the block, or a
    /// negative value when the host does not provide one (CLAP reports -1).
    std::int64_t steady_time = -1;
    /// Frames in the block. Must be in `[1, kHostBlockMaxFrames]`.
    std::size_t frames = 0;
    /// Transport position, in samples, at the first frame of the block.
    bool has_transport_sample = false;
    std::int64_t transport_sample = 0;
    bool transport_playing = false;
    /// Host-signalled seek, loop wrap, or reset before this block.
    bool discontinuity = false;
};

/// The epoch-local sample window one block occupies. Pass `epoch`,
/// `sample_start`, and `frames` to `HostEventIngress::begin_block`.
struct HostBlockWindow {
    std::uint64_t epoch = 0;
    std::uint64_t sample_start = 0;
    std::size_t frames = 0;
};

/// How a block relates to the one before it.
enum class HostBlockBoundary : std::uint8_t {
    First,      ///< First block after construction or reset().
    Continuous, ///< Starts exactly where the previous block ended.
    Gap,        ///< Same epoch; the monotonic clock skipped forward.
    NewEpoch,   ///< Seek, loop, backwards clock, or clock-source change.
};

/// Largest block the projection accepts; equals `HostEventIngress`'s window bound.
inline constexpr std::size_t kHostBlockMaxFrames = 4096;

class HostBlockTimeline final {
  public:
    explicit HostBlockTimeline(std::uint64_t first_epoch = 1) noexcept {
        reset(first_epoch);
    }

    /// Forget the previous block. The next `advance()` opens `epoch` at
    /// sample zero. An epoch of zero is invalid and makes `advance()` refuse.
    void reset(std::uint64_t epoch) noexcept {
        next_epoch_ = epoch;
        window_ = HostBlockWindow{};
        boundary_ = HostBlockBoundary::First;
        has_block_ = false;
        prev_steady_ = -1;
        epoch_origin_steady_ = 0;
        prev_transport_valid_ = false;
        prev_transport_sample_ = 0;
    }

    /// Place the next host block on the epoch-local timeline. On refusal the
    /// previous window is left current and `out` is untouched.
    HostEventResult advance(const HostBlockClock& clock, HostBlockWindow& out) noexcept {
        if (clock.frames == 0 || clock.frames > kHostBlockMaxFrames)
            return HostEventResult::OutOfRange;
        const bool has_steady = clock.steady_time >= 0;

        HostBlockBoundary boundary = HostBlockBoundary::First;
        std::uint64_t epoch = 0;
        std::uint64_t start = 0;
        if (!has_block_) {
            if (next_epoch_ == 0)
                return HostEventResult::StaleEpoch;
            epoch = next_epoch_;
        } else {
            boundary = classify(clock, has_steady);
            const auto prev_end = window_.sample_start + window_.frames;
            switch (boundary) {
            case HostBlockBoundary::Continuous:
                epoch = window_.epoch;
                start = prev_end;
                break;
            case HostBlockBoundary::Gap:
                epoch = window_.epoch;
                start = static_cast<std::uint64_t>(clock.steady_time - epoch_origin_steady_);
                break;
            case HostBlockBoundary::NewEpoch:
            case HostBlockBoundary::First:
                if (window_.epoch == std::numeric_limits<std::uint64_t>::max())
                    return HostEventResult::Overflow;
                epoch = window_.epoch + 1;
                start = 0;
                boundary = HostBlockBoundary::NewEpoch;
                break;
            }
        }
        // Absolute samples are carried as int64 in HostEventRecord.
        constexpr auto kMaxAbsolute =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        if (start > kMaxAbsolute - clock.frames)
            return HostEventResult::Overflow;

        if (boundary == HostBlockBoundary::First || boundary == HostBlockBoundary::NewEpoch)
            epoch_origin_steady_ = has_steady ? clock.steady_time : 0;
        window_ = HostBlockWindow{epoch, start, clock.frames};
        boundary_ = boundary;
        has_block_ = true;
        prev_steady_ = has_steady ? clock.steady_time : -1;
        prev_transport_valid_ = clock.has_transport_sample && clock.transport_playing;
        prev_transport_sample_ = clock.transport_sample;
        out = window_;
        return HostEventResult::Accepted;
    }

    /// Map a block-relative event time to its epoch-local absolute sample.
    /// `window` must be the window returned by the most recent `advance()`.
    HostEventResult project(const HostBlockWindow& window, std::uint32_t block_time,
                            std::int64_t& absolute_sample) const noexcept {
        if (!has_block_ || window.epoch != window_.epoch)
            return HostEventResult::StaleEpoch;
        if (window.sample_start < window_.sample_start)
            return HostEventResult::Late;
        if (window.sample_start != window_.sample_start || window.frames != window_.frames)
            return HostEventResult::OutOfRange;
        if (static_cast<std::size_t>(block_time) >= window.frames)
            return HostEventResult::OutOfRange;
        absolute_sample = static_cast<std::int64_t>(window.sample_start + block_time);
        return HostEventResult::Accepted;
    }

    /// Build a `HostEventRecord` for one block-relative MIDI event. `out` is
    /// written only on success.
    HostEventResult make_record(const HostBlockWindow& window, std::uint32_t block_time,
                                std::uint32_t producer, std::uint64_t producer_sequence,
                                const midi::MidiEvent& event, HostEventRecord& out) const noexcept {
        std::int64_t absolute = 0;
        const auto result = project(window, block_time, absolute);
        if (result != HostEventResult::Accepted)
            return result;
        HostEventRecord record;
        record.epoch = window.epoch;
        record.absolute_sample = absolute;
        record.producer_sequence = producer_sequence;
        record.producer = producer;
        record.midi = event;
        record.midi.sample_offset = static_cast<std::int32_t>(block_time);
        out = record;
        return HostEventResult::Accepted;
    }

    const HostBlockWindow& current() const noexcept {
        return window_;
    }
    HostBlockBoundary last_boundary() const noexcept {
        return boundary_;
    }

  private:
    HostBlockBoundary classify(const HostBlockClock& clock, bool has_steady) const noexcept {
        if (clock.discontinuity)
            return HostBlockBoundary::NewEpoch;
        // A transport position that does not follow the previous playing block
        // is a seek or loop wrap even when the monotonic clock is unbroken.
        if (prev_transport_valid_ && clock.has_transport_sample && clock.transport_playing) {
            const auto prev = prev_transport_sample_;
            const auto step = static_cast<std::int64_t>(window_.frames);
            if (prev > std::numeric_limits<std::int64_t>::max() - step ||
                clock.transport_sample != prev + step)
                return HostBlockBoundary::NewEpoch;
        }
        const bool prev_has_steady = prev_steady_ >= 0;
        if (has_steady != prev_has_steady)
            return HostBlockBoundary::NewEpoch;
        if (!has_steady)
            return HostBlockBoundary::Continuous;
        const auto step = static_cast<std::int64_t>(window_.frames);
        if (prev_steady_ > std::numeric_limits<std::int64_t>::max() - step)
            return HostBlockBoundary::NewEpoch;
        const auto expected = prev_steady_ + step;
        if (clock.steady_time == expected)
            return HostBlockBoundary::Continuous;
        // A backwards clock would retime samples that were already delivered.
        if (clock.steady_time < expected)
            return HostBlockBoundary::NewEpoch;
        return HostBlockBoundary::Gap;
    }

    HostBlockWindow window_{};
    std::uint64_t next_epoch_ = 1;
    std::int64_t prev_steady_ = -1;
    std::int64_t epoch_origin_steady_ = 0;
    std::int64_t prev_transport_sample_ = 0;
    HostBlockBoundary boundary_ = HostBlockBoundary::First;
    bool has_block_ = false;
    bool prev_transport_valid_ = false;
};

} // namespace pulp::host
