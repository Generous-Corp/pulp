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
///    evicts staged records left behind such a jump. When both blocks are
///    playing, the transport must have advanced by the same number of samples
///    as the counter for the jump to count as a gap.
///  * A seek or loop (an explicit host discontinuity, or a playing transport
///    whose delta differs from the counter's delta, or from the block length
///    when the host has no counter), a counter that runs backwards, or a
///    change in clock availability starts a new epoch at sample zero. A
///    transport that is not playing in either block is never compared, so
///    stopping, starting, or relocating while stopped is not a seek. The ingress clears its staging area on a new epoch, so a
///    record timed against the old timeline can never be delivered against
///    the new one.
///  * An event time at or past the block's frame count is refused with
///    `OutOfRange`; it is never clamped into the block.
///  * An event projected against a window older than the current block is
///    refused with `Late`, and one from a retired epoch with `StaleEpoch`.
///
/// `clock_from_vst3()` and `clock_from_au()` map VST3 and Audio Unit block
/// timing (given as plain fields, so no SDK is needed) onto `HostBlockClock`;
/// `host_block_time_from_offset()` and `au_event_block_time()` convert their
/// event times to block-relative times without clamping.
///
/// Everything here is fixed-size, `noexcept`, and allocation-free, so it may
/// run on the audio thread. It owns no queue and schedules nothing.

#include <pulp/format/detail/host_sample_position.hpp>
#include <pulp/host/graph_event_contract.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace pulp::host {

/// Timing facts a host supplies for one process block.
struct HostBlockClock {
    /// Host monotonic sample counter at the first frame of the block, or a
    /// negative value when the host does not provide one (CLAP reports -1).
    /// CLAP calls this sample-domain value `clap_process_t::steady_time`; it
    /// is not a wall-clock duration and must not be converted from nanoseconds.
    std::int64_t steady_sample = -1;
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
        const bool has_steady = clock.steady_sample >= 0;

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
                start = static_cast<std::uint64_t>(clock.steady_sample - epoch_origin_steady_);
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
            epoch_origin_steady_ = has_steady ? clock.steady_sample : 0;
        window_ = HostBlockWindow{epoch, start, clock.frames};
        boundary_ = boundary;
        has_block_ = true;
        prev_steady_ = has_steady ? clock.steady_sample : -1;
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
    static bool checked_delta(std::int64_t to, std::int64_t from, std::int64_t& delta) noexcept {
        constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
        constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
        if ((from > 0 && to < kMin + from) || (from < 0 && to > kMax + from))
            return false;
        delta = to - from;
        return true;
    }

    HostBlockBoundary classify(const HostBlockClock& clock, bool has_steady) const noexcept {
        if (clock.discontinuity)
            return HostBlockBoundary::NewEpoch;
        const bool prev_has_steady = prev_steady_ >= 0;
        if (has_steady != prev_has_steady)
            return HostBlockBoundary::NewEpoch;
        const auto frames = static_cast<std::int64_t>(window_.frames);
        // Transport positions are compared only when both blocks were playing;
        // a stopped transport does not advance, so its position says nothing
        // about elapsed time.
        const bool transport_checked =
            prev_transport_valid_ && clock.has_transport_sample && clock.transport_playing;
        std::int64_t transport_delta = 0;
        if (transport_checked &&
            !checked_delta(clock.transport_sample, prev_transport_sample_, transport_delta))
            return HostBlockBoundary::NewEpoch;

        if (!has_steady) {
            // Without a sample counter elapsed time cannot be measured, so a
            // transport that does not follow the previous block is a seek.
            if (transport_checked && transport_delta != frames)
                return HostBlockBoundary::NewEpoch;
            return HostBlockBoundary::Continuous;
        }
        // Both counters are non-negative, so this subtraction cannot overflow.
        const auto steady_delta = clock.steady_sample - prev_steady_;
        // A counter that runs backwards or overlaps the previous block would
        // retime samples that were already delivered.
        if (steady_delta < frames)
            return HostBlockBoundary::NewEpoch;
        // While playing, the transport must move by exactly the elapsed
        // samples. A different delta is a seek or loop wrap; an equal delta
        // larger than the block is a forward gap on one unbroken timeline.
        if (transport_checked && transport_delta != steady_delta)
            return HostBlockBoundary::NewEpoch;
        return steady_delta == frames ? HostBlockBoundary::Continuous : HostBlockBoundary::Gap;
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

// ---------------------------------------------------------------------------
// Format timing inputs
//
// The structs below carry the native per-block timing fields of VST3 and
// Audio Unit hosts as plain values, so the mapping is usable and testable
// without either SDK. The constants mirror the SDK values and are checked
// against the real headers by the timestamp tests wherever those headers
// exist.
// ---------------------------------------------------------------------------

/// Convert a signed block-relative event offset (VST3 `Event::sampleOffset`,
/// AUv2 `inOffsetSampleFrame`) to the unsigned time `project()` takes. A
/// negative offset is refused with `OutOfRange` rather than wrapped into a
/// huge time or clamped to zero; the upper bound is enforced by `project()`.
inline HostEventResult host_block_time_from_offset(std::int64_t offset,
                                                   std::uint32_t& block_time) noexcept {
    if (offset < 0 || offset > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()))
        return HostEventResult::OutOfRange;
    block_time = static_cast<std::uint32_t>(offset);
    return HostEventResult::Accepted;
}

/// `Steinberg::Vst::ProcessContext::StatesAndFlags::kPlaying`.
inline constexpr std::uint32_t kVst3ContextPlaying = 1u << 1;
/// `Steinberg::Vst::ProcessContext::StatesAndFlags::kContTimeValid`.
inline constexpr std::uint32_t kVst3ContextContTimeValid = 1u << 17;

/// One VST3 `process()` call's timing facts.
///
/// | field                     | VST3 source                                   |
/// |---------------------------|-----------------------------------------------|
/// | `has_process_context`     | `ProcessData::processContext != nullptr`      |
/// | `state`                   | `ProcessContext::state`                       |
/// | `project_time_samples`    | `ProcessContext::projectTimeSamples`          |
/// | `continuous_time_samples` | `ProcessContext::continousTimeSamples`        |
/// | `num_samples`             | `ProcessData::numSamples`                     |
/// | `discontinuity`           | adapter-owned (no VST3 flag; e.g. the first   |
/// |                           | block after `setProcessing(true)`)            |
struct Vst3BlockTiming {
    bool has_process_context = false;
    std::uint32_t state = 0;
    std::int64_t project_time_samples = 0;
    std::int64_t continuous_time_samples = 0;
    std::int32_t num_samples = 0;
    bool discontinuity = false;
};

/// Map a VST3 block onto `HostBlockClock`.
///
/// `continousTimeSamples` is VST3's monotonic sample counter and becomes the
/// steady clock only while `kContTimeValid` is set and the value is
/// non-negative; otherwise the block has no steady clock, exactly like a CLAP
/// host reporting `steady_time == -1`. `projectTimeSamples` has no validity
/// bit of its own (`kProjectTimeMusicValid` covers `projectTimeMusic`, the
/// beat position) and is required whenever a context is supplied, so it is the
/// transport position for every block that has a context; `kPlaying` is the
/// playing state. A missing context yields no counter and no transport. A
/// non-positive `numSamples` maps to zero frames, which `advance()` refuses.
inline constexpr HostBlockClock clock_from_vst3(const Vst3BlockTiming& timing) noexcept {
    HostBlockClock clock;
    clock.frames = timing.num_samples > 0 ? static_cast<std::size_t>(timing.num_samples) : 0;
    clock.discontinuity = timing.discontinuity;
    if (!timing.has_process_context)
        return clock;
    if ((timing.state & kVst3ContextContTimeValid) != 0 && timing.continuous_time_samples >= 0)
        clock.steady_sample = timing.continuous_time_samples;
    clock.has_transport_sample = true;
    clock.transport_sample = timing.project_time_samples;
    clock.transport_playing = (timing.state & kVst3ContextPlaying) != 0;
    return clock;
}

/// `kAudioTimeStampSampleTimeValid` from CoreAudioTypes.
inline constexpr std::uint32_t kAuTimeStampSampleTimeValid = 1u << 0;
/// `AUEventSampleTimeImmediate` from AudioToolbox: event times in
/// `[immediate, immediate + kAuImmediateWindow)` are already block offsets.
inline constexpr std::int64_t kAuEventSampleTimeImmediate =
    static_cast<std::int64_t>(0xffffffff00000000ULL);
inline constexpr std::int64_t kAuImmediateWindow = 4096;

/// One Audio Unit render call's timing facts (AUv2 `Render` / AUv3 render
/// block).
///
/// | field                       | AU source                                              |
/// |-----------------------------|--------------------------------------------------------|
/// | `timestamp_flags`           | `AudioTimeStamp::mFlags`                               |
/// | `sample_time`               | `AudioTimeStamp::mSampleTime`                          |
/// | `frames`                    | render frame count (`frameCount` / `inNumberFrames`)   |
/// | `has_transport`             | AUv3 `transportStateBlock` returned YES, or AUv2       |
/// |                             | `CallHostTransportState` returned `noErr`              |
/// | `transport_moving`          | `AUHostTransportStateMoving` / AUv2 `outIsPlaying`     |
/// | `transport_sample_position` | AUv3 `currentSamplePosition` / AUv2                    |
/// |                             | `outCurrentSampleInTimeLine`                           |
/// | `discontinuity`             | adapter-owned (e.g. the first render after `reset`)    |
///
/// The hosts' `AUHostTransportStateChanged` / `outTransportStateChanged`
/// flag is deliberately not a discontinuity: it also fires on play and stop,
/// which are not seeks. A relocation while playing is detected from the
/// transport delta instead.
struct AuBlockTiming {
    std::uint32_t timestamp_flags = 0;
    double sample_time = 0.0;
    std::uint32_t frames = 0;
    bool has_transport = false;
    bool transport_moving = false;
    double transport_sample_position = 0.0;
    bool discontinuity = false;
};

namespace detail {
/// Nearest integer sample to a host double, using the same rounding the AU
/// adapters apply to host positions (REAPER reports 13823.9999 for 13824).
inline bool au_host_sample(double samples, std::int64_t& out) noexcept {
    const auto rounded = format::detail::host_sample_position(samples);
    if (!rounded)
        return false;
    out = *rounded;
    return true;
}
} // namespace detail

/// Map an Audio Unit render call onto `HostBlockClock`.
///
/// `mSampleTime` is the host's render sample counter, which keeps running
/// across seeks and loops, so it is the steady clock when
/// `kAudioTimeStampSampleTimeValid` is set and the rounded value is
/// representable and non-negative; otherwise the block has no steady clock.
/// The transport position is used only when the host's transport callback
/// succeeded and the position is representable.
inline HostBlockClock clock_from_au(const AuBlockTiming& timing) noexcept {
    HostBlockClock clock;
    clock.frames = timing.frames;
    clock.discontinuity = timing.discontinuity;
    std::int64_t sample = 0;
    if ((timing.timestamp_flags & kAuTimeStampSampleTimeValid) != 0 &&
        detail::au_host_sample(timing.sample_time, sample) && sample >= 0)
        clock.steady_sample = sample;
    if (timing.has_transport && detail::au_host_sample(timing.transport_sample_position, sample)) {
        clock.has_transport_sample = true;
        clock.transport_sample = sample;
        clock.transport_playing = timing.transport_moving;
    }
    return clock;
}

/// Convert an AUv3 render event's `eventSampleTime` to a block-relative time.
///
/// A time inside the `AUEventSampleTimeImmediate` window is already an
/// offset. Any other time is absolute on the `mSampleTime` axis and is placed
/// relative to the block's rounded `mSampleTime`; without a valid block
/// sample time such an event cannot be placed and is refused. Offsets before
/// the block or at/after its frame count are refused with `OutOfRange`,
/// never clamped.
inline HostEventResult au_event_block_time(std::int64_t event_sample_time,
                                           const AuBlockTiming& timing,
                                           std::uint32_t& block_time) noexcept {
    std::int64_t offset = 0;
    if (event_sample_time >= kAuEventSampleTimeImmediate &&
        event_sample_time < kAuEventSampleTimeImmediate + kAuImmediateWindow) {
        offset = event_sample_time - kAuEventSampleTimeImmediate;
    } else {
        std::int64_t start = 0;
        if ((timing.timestamp_flags & kAuTimeStampSampleTimeValid) == 0 ||
            !detail::au_host_sample(timing.sample_time, start))
            return HostEventResult::OutOfRange;
        if ((start > 0 && event_sample_time < std::numeric_limits<std::int64_t>::min() + start) ||
            (start < 0 && event_sample_time > std::numeric_limits<std::int64_t>::max() + start))
            return HostEventResult::OutOfRange;
        offset = event_sample_time - start;
    }
    if (offset < 0 || offset >= static_cast<std::int64_t>(timing.frames))
        return HostEventResult::OutOfRange;
    block_time = static_cast<std::uint32_t>(offset);
    return HostEventResult::Accepted;
}

} // namespace pulp::host
