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
/// `clock_from_vst3()`, `clock_from_au()`, and `clock_from_lv2()` map VST3,
/// Audio Unit, and LV2 block timing (given as plain fields, so no SDK is
/// needed) onto `HostBlockClock`; `host_block_time_from_offset()`,
/// `au_event_block_time()`, and `lv2_event_block_time()` convert their event
/// times to block-relative times without clamping.
///
/// CLAP needs no separate helper: the CLAP adapter publishes
/// `clap_process_t::steady_time` and the `song_pos_seconds` transport,
/// converted from CLAP's 2^31 fixed point with `format::clap_sectime_to_samples()`,
/// on `ProcessContext::timing`. A transport rounding that is not exact still
/// reads as a seek.
///
/// Transport comparison assumes unit-rate playback. LV2 reports `time:speed`
/// and counts only speed 1 as playing. VST3 and AU carry no playback rate, so
/// a host playing at another rate (a varispeed or playrate control) moves the
/// transport by a different amount than the counter and every block would
/// read as a seek. The adapter owns that knowledge: it sets
/// `transport_non_unit_rate` on `Vst3BlockTiming` / `AuBlockTiming` when it
/// knows the rate is not 1, and the transport is then treated as not playing.
///
/// Format adapters publish the same facts, already decoded, on
/// `format::ProcessContext::timing`; `clock_from_process_context()` turns a
/// context into a `HostBlockClock`, so a Processor that owns an event timeline
/// (for example one hosting a `SignalGraph`) advances one `HostBlockTimeline`
/// per block from the context it receives. The per-format helpers above map
/// through the same `format::*_process_timing()` builders the adapters use.
///
/// A host block longer than `kHostBlockMaxFrames` is refused by `advance()`
/// with `OutOfRange`. A caller that must accept one splits it with
/// `host_block_chunk()` into consecutive sub-blocks of at most that length;
/// each chunk is a contiguous block of the same timeline, so the absolute
/// samples are identical to delivering the chunks as separate host blocks.
///
/// Everything here is fixed-size, `noexcept`, and allocation-free, so it may
/// run on the audio thread. It owns no queue and schedules nothing.

#include <pulp/format/detail/host_sample_position.hpp>
#include <pulp/format/process_context.hpp>
#include <pulp/format/process_timing.hpp>
#include <pulp/host/graph_event_contract.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

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

/// Map neutral adapter timing facts for a block of `frames` samples onto
/// `HostBlockClock`. A transport counts as playing only while it moves at a
/// rate that is not known to differ from one sample per frame; a stopped or
/// varispeed transport is still reported but is never compared.
inline constexpr HostBlockClock clock_from_process_timing(const format::ProcessTiming& timing,
                                                          std::size_t frames) noexcept {
    HostBlockClock clock;
    clock.frames = frames;
    clock.discontinuity = timing.discontinuity;
    if (timing.has_steady_sample && timing.steady_sample >= 0)
        clock.steady_sample = timing.steady_sample;
    if (timing.has_transport_sample) {
        clock.has_transport_sample = true;
        clock.transport_sample = timing.transport_sample;
        clock.transport_playing = timing.transport_tracks_samples();
    }
    return clock;
}

/// Map the context a format adapter handed `Processor::process()` onto
/// `HostBlockClock`. A non-positive `num_samples` maps to zero frames, which
/// `advance()` refuses.
inline constexpr HostBlockClock
clock_from_process_context(const format::ProcessContext& ctx) noexcept {
    return clock_from_process_timing(
        ctx.timing, ctx.num_samples > 0 ? static_cast<std::size_t>(ctx.num_samples) : 0);
}

/// The clock of frames `[offset, offset + frames)` of a host block, for a
/// caller that splits a block longer than `kHostBlockMaxFrames`. The steady
/// counter, and the transport while it is playing, advance by `offset`; a
/// stopped or varispeed transport is reported where the host left it. Only the
/// chunk at offset zero carries the block's discontinuity. An empty chunk, one
/// that does not fit in the block or in `kHostBlockMaxFrames`, or a counter
/// that would overflow is refused with `OutOfRange` and leaves `out`
/// untouched. Event times are made chunk-relative by subtracting `offset`.
inline HostEventResult host_block_chunk(const HostBlockClock& block, std::size_t offset,
                                        std::size_t frames, HostBlockClock& out) noexcept {
    if (frames == 0 || frames > kHostBlockMaxFrames || offset > block.frames ||
        frames > block.frames - offset)
        return HostEventResult::OutOfRange;
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (offset > kMax)
        return HostEventResult::OutOfRange;
    const auto step = static_cast<std::int64_t>(offset);
    HostBlockClock chunk = block;
    chunk.frames = frames;
    chunk.discontinuity = block.discontinuity && offset == 0;
    if (block.steady_sample >= 0) {
        if (block.steady_sample > std::numeric_limits<std::int64_t>::max() - step)
            return HostEventResult::OutOfRange;
        chunk.steady_sample = block.steady_sample + step;
    }
    if (block.has_transport_sample && block.transport_playing) {
        if ((step > 0 && block.transport_sample > std::numeric_limits<std::int64_t>::max() - step))
            return HostEventResult::OutOfRange;
        chunk.transport_sample = block.transport_sample + step;
    }
    out = chunk;
    return HostEventResult::Accepted;
}

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

/// Place the block a format adapter handed `Processor::process()` on
/// `timeline`, from the neutral facts on `ctx.timing`. This is the per-block
/// step of a Processor or host binding that owns an event timeline. A block
/// longer than `kHostBlockMaxFrames` is refused with `OutOfRange`; split it
/// with `host_block_chunk()` first.
inline HostEventResult advance_from_context(HostBlockTimeline& timeline,
                                            const format::ProcessContext& ctx,
                                            HostBlockWindow& window) noexcept {
    return timeline.advance(clock_from_process_context(ctx), window);
}

/// Build a `HostEventRecord` from a signed block-relative offset, such as the
/// `midi::MidiEvent::sample_offset` an adapter delivered with the block. A
/// negative offset, or one at or past the window's frame count, is refused
/// with `OutOfRange` and never clamped; `out` is written only on success.
inline HostEventResult make_record_from_block_offset(const HostBlockTimeline& timeline,
                                                     const HostBlockWindow& window,
                                                     std::int64_t offset, std::uint32_t producer,
                                                     std::uint64_t producer_sequence,
                                                     const midi::MidiEvent& event,
                                                     HostEventRecord& out) noexcept;

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

inline HostEventResult make_record_from_block_offset(const HostBlockTimeline& timeline,
                                                     const HostBlockWindow& window,
                                                     std::int64_t offset, std::uint32_t producer,
                                                     std::uint64_t producer_sequence,
                                                     const midi::MidiEvent& event,
                                                     HostEventRecord& out) noexcept {
    std::uint32_t block_time = 0;
    if (const auto result = host_block_time_from_offset(offset, block_time);
        result != HostEventResult::Accepted)
        return result;
    return timeline.make_record(window, block_time, producer, producer_sequence, event, out);
}

/// `Steinberg::Vst::ProcessContext::StatesAndFlags::kPlaying`.
inline constexpr std::uint32_t kVst3ContextPlaying = format::kVst3ContextPlaying;
/// `Steinberg::Vst::ProcessContext::StatesAndFlags::kContTimeValid`.
inline constexpr std::uint32_t kVst3ContextContTimeValid = format::kVst3ContextContTimeValid;

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
/// | `transport_non_unit_rate` | adapter-owned (no VST3 field; set when the    |
/// |                           | host plays at a rate other than 1)            |
struct Vst3BlockTiming {
    bool has_process_context = false;
    std::uint32_t state = 0;
    std::int64_t project_time_samples = 0;
    std::int64_t continuous_time_samples = 0;
    std::int32_t num_samples = 0;
    bool discontinuity = false;
    bool transport_non_unit_rate = false;
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
/// playing state, unless `transport_non_unit_rate` is set: a transport
/// playing at another rate never advances by the block length, so it counts as
/// not playing and is not compared. A missing context yields no counter and no
/// transport. A non-positive `numSamples` maps to zero frames, which
/// `advance()` refuses.
inline constexpr HostBlockClock clock_from_vst3(const Vst3BlockTiming& timing) noexcept {
    return clock_from_process_timing(
        format::vst3_process_timing(timing.has_process_context, timing.state,
                                    timing.project_time_samples, timing.continuous_time_samples,
                                    timing.transport_non_unit_rate, timing.discontinuity),
        timing.num_samples > 0 ? static_cast<std::size_t>(timing.num_samples) : 0);
}

/// `kAudioTimeStampSampleTimeValid` from CoreAudioTypes.
inline constexpr std::uint32_t kAuTimeStampSampleTimeValid = format::kAuTimeStampSampleTimeValid;
/// `AUEventSampleTimeImmediate` from AudioToolbox: event times in
/// `[immediate, immediate + kAuImmediateWindow)` are already block offsets.
inline constexpr std::int64_t kAuEventSampleTimeImmediate =
    static_cast<std::int64_t>(0xffffffff00000000ULL);
inline constexpr std::int64_t kAuImmediateWindow = format::detail::kAuImmediateWindow;

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
/// | `transport_non_unit_rate`   | adapter-owned (no AU field; set when the host plays at |
/// |                             | a rate other than 1)                                   |
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
    bool transport_non_unit_rate = false;
};

/// Map an Audio Unit render call onto `HostBlockClock`.
///
/// `mSampleTime` is the host's render sample counter, which keeps running
/// across seeks and loops, so it is the steady clock when
/// `kAudioTimeStampSampleTimeValid` is set and the rounded value is
/// representable and non-negative; otherwise the block has no steady clock.
/// The transport position is used only when the host's transport callback
/// succeeded and the position is representable; it counts as playing only
/// while moving at unit rate (`transport_non_unit_rate` unset).
inline HostBlockClock clock_from_au(const AuBlockTiming& timing) noexcept {
    return clock_from_process_timing(
        format::au_process_timing(timing.timestamp_flags, timing.sample_time, timing.has_transport,
                                  timing.transport_moving, timing.transport_sample_position,
                                  timing.transport_non_unit_rate, timing.discontinuity),
        timing.frames);
}

/// Convert an AUv3 render event's `eventSampleTime` to a block-relative time.
///
/// A time inside the `AUEventSampleTimeImmediate` window is already an
/// offset. Any other time is absolute on the `mSampleTime` axis and is placed
/// relative to the block's rounded `mSampleTime`; without a valid block
/// sample time such an event cannot be placed and is refused. The range rule
/// is the adapter's own `format::admit_au_event_time()`, so the host accepts
/// exactly what an AU v3 adapter delivers: offsets before the block, at or
/// after its frame count, or past the `int32` offset the adapter delivers are
/// refused with `OutOfRange`, never clamped. `block_time` is written only on
/// acceptance.
inline HostEventResult au_event_block_time(std::int64_t event_sample_time,
                                           const AuBlockTiming& timing,
                                           std::uint32_t& block_time) noexcept {
    std::optional<double> block_sample_time;
    if ((timing.timestamp_flags & kAuTimeStampSampleTimeValid) != 0)
        block_sample_time = timing.sample_time;
    format::EventTimeAdmission admission{static_cast<std::int64_t>(timing.frames)};
    const auto offset = format::admit_au_event_time(admission, event_sample_time,
                                                    kAuEventSampleTimeImmediate, block_sample_time);
    if (!offset)
        return HostEventResult::OutOfRange;
    block_time = static_cast<std::uint32_t>(*offset);
    return HostEventResult::Accepted;
}

/// One LV2 `run()` call's timing facts.
///
/// | field           | LV2 source                                                  |
/// |-----------------|-------------------------------------------------------------|
/// | `sample_count`  | `run(instance, sample_count)`                               |
/// | `has_frame`     | a `time:Position` carrying `time:frame` has been received   |
/// | `frame`         | `time:frame` at the first frame of this block               |
/// | `has_speed`     | that `time:Position` carried `time:speed`                   |
/// | `speed`         | `time:speed` (0 = stopped, 1 = normal-rate playback)        |
/// | `discontinuity` | adapter-owned (e.g. the first `run()` after `activate()`)   |
///
/// LV2 hosts are not required to send a `time:Position` every cycle, so
/// `frame` is normally the adapter's latched position advanced by the
/// previous block's length while the transport rolls. A `time:Position`
/// event at time `t` describes the transport at frame `t` of the block; the
/// caller converts it to the block start before filling `frame`.
struct Lv2BlockTiming {
    std::uint32_t sample_count = 0;
    bool has_frame = false;
    std::int64_t frame = 0;
    bool has_speed = false;
    double speed = 0.0;
    bool discontinuity = false;
};

/// Map an LV2 `run()` call onto `HostBlockClock`.
///
/// LV2 defines no monotonic sample counter, so the clock never has a steady
/// sample and a skipped block cannot be told from a contiguous one; the
/// timeline classifies LV2 blocks on the transport alone. `time:frame` is the
/// transport position whenever one is known. Only normal-rate playback
/// (`time:speed` exactly 1) counts as playing: at any other non-zero speed the
/// transport does not move one sample per frame, so its delta cannot be
/// compared with the block length and a relocation at that speed is not
/// detected unless the adapter sets `discontinuity`.
inline constexpr HostBlockClock clock_from_lv2(const Lv2BlockTiming& timing) noexcept {
    return clock_from_process_timing(format::lv2_process_timing(timing.has_frame, timing.frame,
                                                                timing.has_speed, timing.speed,
                                                                timing.discontinuity),
                                     timing.sample_count);
}

/// Convert an LV2 atom event's time stamp to a block-relative time.
///
/// `sequence_unit` is `LV2_Atom_Sequence_Body::unit`: 0 or the host's URID
/// for `atom:frameTime` (`frame_time_urid`) mean `event_frames` is
/// `LV2_Atom_Event::time.frames`, a frame offset into the block. A sequence in
/// any other unit (`atom:beatTime` stores a beat count in the same bytes) is
/// refused with `OutOfRange` rather than reinterpreted as frames. A negative
/// offset is refused too; the upper bound is enforced by `project()`.
inline HostEventResult lv2_event_block_time(std::uint32_t sequence_unit,
                                            std::uint32_t frame_time_urid,
                                            std::int64_t event_frames,
                                            std::uint32_t& block_time) noexcept {
    if (sequence_unit != 0 && (frame_time_urid == 0 || sequence_unit != frame_time_urid))
        return HostEventResult::OutOfRange;
    return host_block_time_from_offset(event_frames, block_time);
}

} // namespace pulp::host
