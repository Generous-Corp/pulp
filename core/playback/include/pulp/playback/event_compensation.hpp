#pragma once

#include <pulp/playback/program.hpp>
#include <pulp/playback/transport.hpp>
#include <pulp/timebase/rational_time.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace pulp::playback {

/// One event-injection point's prepared scheduling shift, counted in samples at
/// the program's compiled sample rate.
///
/// Compensation shifts the SCHEDULING WINDOW; it never rewrites event data.
/// `PlaybackProgram` is immutable and shared by the offline and realtime paths,
/// and `NoteProgramEvent::sample` is authoritative against the exact
/// `CompiledTempoMap` the program was compiled with. Folding a host-derived
/// latency into that field would make the program a function of the host graph
/// as well as the document, and would force a full recompile on every device
/// swap. Reading the stream from `origin + shift` instead leaves the program
/// untouched: the downstream chain delays the events back into place, so they
/// land on the sample the document authored.
///
/// Samples, never ticks. A device's latency is a property of its buffering, not
/// of the music, so a tempo ramp must not change it. Expressing the shift in
/// ticks would shrink it as tempo rises, which is the reason
/// `timeline::ProductionDeclaration`'s lookahead is barred from reaching a
/// delay-compensation computation in the first place.
struct EventCompensationShift {
    std::int64_t samples = 0;

    constexpr bool compensating() const noexcept {
        return samples != 0;
    }
    constexpr bool operator==(const EventCompensationShift&) const noexcept = default;
};

enum class EventCompensationCode : std::uint8_t {
    Ok,
    /// A device reported a negative latency. There is no reading of that a
    /// scheduler can honour, so it fails closed rather than clamping to zero.
    NegativeDeviceLatency,
    /// A device's own reported latency exceeds the ceiling the caller supplied.
    DeviceLatencyOutOfRange,
    /// The accumulated chain shift exceeds the ceiling the caller supplied.
    ChainShiftOutOfRange,
};

struct EventCompensationResult {
    EventCompensationCode code = EventCompensationCode::Ok;
    EventCompensationShift shift{};
    /// The offending value and the ceiling that rejected it, so a caller can
    /// report the exact number rather than only that something was too large.
    std::int64_t actual = 0;
    std::int64_t limit = 0;
    /// Index into the supplied latencies for a per-device failure.
    std::size_t index = 0;

    constexpr explicit operator bool() const noexcept {
        return code == EventCompensationCode::Ok;
    }
};

/// Accumulates the event-domain latency of one chain into a scheduling shift.
///
/// Only latency that delays the EVENTS themselves belongs here. A device that
/// converts events to audio contributes nothing: its audio output is already
/// aligned by the graph's own plug-in delay compensation, which delays every
/// sibling branch to match, so adding it again would compensate twice and pull
/// the stream early by exactly the amount the graph had already handled.
///
/// `ceiling_samples` is supplied by the caller rather than defined here so this
/// rung introduces no second latency constant beside the graph's own.
constexpr EventCompensationResult
accumulate_event_chain_shift(std::span<const int> device_latency_samples,
                             int ceiling_samples) noexcept {
    EventCompensationResult result;
    if (ceiling_samples < 0) {
        result.code = EventCompensationCode::ChainShiftOutOfRange;
        result.limit = ceiling_samples;
        return result;
    }
    const auto limit = static_cast<std::int64_t>(ceiling_samples);
    result.limit = limit;
    std::int64_t total = 0;
    for (std::size_t index = 0; index < device_latency_samples.size(); ++index) {
        const auto latency = static_cast<std::int64_t>(device_latency_samples[index]);
        if (latency < 0) {
            result.code = EventCompensationCode::NegativeDeviceLatency;
            result.actual = latency;
            result.index = index;
            return result;
        }
        if (latency > limit) {
            result.code = EventCompensationCode::DeviceLatencyOutOfRange;
            result.actual = latency;
            result.index = index;
            return result;
        }
        // Bounded by the ceiling on both the addend and the running total, so
        // the sum cannot overflow before the range check sees it.
        total += latency;
        if (total > limit) {
            result.code = EventCompensationCode::ChainShiftOutOfRange;
            result.actual = total;
            result.index = index;
            return result;
        }
    }
    result.shift = {total};
    return result;
}

/// The sample position an event stream at a compensated injection point must be
/// read from for `range`.
///
/// Applied per `TransportRange`, never per block. The loop wrap is the hazard:
/// a block that straddles a wrap carries two monotonic ranges, and reading the
/// second one from the first one's origin would replay the pre-wrap window.
/// Saturating, so a shift near the end of the sample domain clamps instead of
/// wrapping into the past.
constexpr timebase::SamplePosition shifted_range_origin(const TransportRange& range,
                                                        EventCompensationShift shift) noexcept {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto origin = range.timeline_sample_start.value;
    if (shift.samples > 0 && origin > maximum - shift.samples)
        return {maximum};
    if (shift.samples < 0 && origin < minimum - shift.samples)
        return {minimum};
    return {origin + shift.samples};
}

/// Whether a compensating shift has a defined meaning for `range`.
///
/// A host-beat-mapped range locates events by their authored tick against the
/// host's own beat window, not by a document sample, so a sample-domain shift
/// has nowhere to land in it. Converting the shift to ticks is exactly what the
/// unit rule forbids, so the combination fails closed instead of scheduling
/// against a quantity nothing defined.
constexpr bool range_admits_event_compensation(const TransportRange& range,
                                               EventCompensationShift shift) noexcept {
    return !shift.compensating() || !range.host_beat_mapping;
}

/// Whether a compensating shift's read-ahead stays inside an enabled loop.
///
/// A window that stays inside reads one contiguous run from the shifted origin
/// and needs no fold. A window that does not is not refused: `plan_compensated_read`
/// folds it back around the loop point so the read-ahead returns the post-wrap
/// content, which is what the musician hears when those frames reach the device.
/// The fold costs a second run and a wrap-time release, so the predicate stays
/// worth asking. `loop_end_samples` is the loop end in the program's own
/// document samples; a disabled loop is unbounded and the caller passes nothing.
constexpr bool loop_admits_event_compensation(const TransportRange& range,
                                              EventCompensationShift shift,
                                              timebase::SamplePosition loop_end_samples) noexcept {
    if (!shift.compensating())
        return true;
    const auto origin = shifted_range_origin(range, shift);
    const auto frames = static_cast<std::int64_t>(range.frame_count);
    return origin.value <= loop_end_samples.value - frames;
}

/// One contiguous run of a compensated read window after an enabled loop has
/// been applied to it.
struct CompensatedReadSegment {
    /// Document sample the run reads the program from.
    timebase::SamplePosition document_start{};
    /// Length of the run, in frames.
    std::uint32_t frame_count = 0;
    /// Where the run starts inside its `TransportRange`. Add `range.sample_offset`
    /// to place an emitted event in the block.
    std::uint32_t range_offset = 0;
    /// Loop passes between `range.loop_pass_index` and the pass this run reads.
    /// A run past the loop point reads the NEXT pass, so a per-pass note modifier
    /// must be resolved against that pass and not against the transport's.
    std::uint64_t pass_offset = 0;
};

/// How a compensated read window resolves against an enabled loop.
///
/// Reading ahead near a loop end wants the content from AFTER the wrap. An event
/// at document position `loop_end + n` never sounds in this pass, while the one
/// at `loop_start + n` is exactly what is heard when the window's frames reach
/// the device a shift later. The window is therefore folded back around the loop
/// point rather than read past it, which splits it into at most two runs — at
/// most, because the transport refuses a loop shorter than one maximum block, so
/// a window spans the loop point once and never twice.
struct CompensatedReadPlan {
    std::array<CompensatedReadSegment, 2> segments{};
    std::uint8_t segment_count = 0;
    /// True when the loop point falls INSIDE this window. The event stream wraps
    /// there, ahead of the transport's own wrap, so whatever the earlier pass left
    /// sounding is released at `segments[1].range_offset` and the transport's
    /// later discontinuity must not release it a second time.
    bool wraps_inside_range = false;
};

/// The one run a window that no loop bounds resolves to. The unlooped, the
/// uncompensated, and the host-beat-mapped cases all take this shape, so a
/// consumer walks segments once rather than carrying two code paths.
constexpr CompensatedReadPlan plan_compensated_read(const TransportRange& range,
                                                    EventCompensationShift shift) noexcept {
    CompensatedReadPlan plan;
    plan.segments[0] = {shifted_range_origin(range, shift), range.frame_count, 0, 0};
    plan.segment_count = 1;
    return plan;
}

/// Wrap-aware read-ahead: the runs a compensated window reads when an enabled
/// loop bounds it.
///
/// The shifted origin is first folded into `[loop_start, loop_end)`, which is a
/// no-op for an ordinary window and recovers the post-wrap position for one that
/// has already been carried past the loop point by the shift. The number of loop
/// lengths that fold consumed is the pass the run belongs to. Whatever remains of
/// the window past the loop point becomes a second run reading from `loop_start`.
constexpr CompensatedReadPlan
plan_compensated_read(const TransportRange& range, EventCompensationShift shift,
                      timebase::SamplePosition loop_start_samples,
                      timebase::SamplePosition loop_end_samples) noexcept {
    CompensatedReadPlan plan;
    auto start = shifted_range_origin(range, shift);
    const auto length = loop_end_samples.value - loop_start_samples.value;
    if (length <= 0)
        return plan_compensated_read(range, shift);
    const auto span = static_cast<std::uint64_t>(length);
    std::uint64_t pass_offset = 0;
    if (start.value >= loop_end_samples.value) {
        const auto past = static_cast<std::uint64_t>(start.value - loop_start_samples.value);
        pass_offset = past / span;
        start = {loop_start_samples.value + static_cast<std::int64_t>(past % span)};
    }
    auto head = range.frame_count;
    if (start.value < loop_end_samples.value) {
        const auto until_wrap = static_cast<std::uint64_t>(loop_end_samples.value - start.value);
        if (until_wrap < head)
            head = static_cast<std::uint32_t>(until_wrap);
    }
    plan.segments[0] = {start, head, 0, pass_offset};
    plan.segment_count = 1;
    if (head < range.frame_count) {
        plan.segments[1] = {loop_start_samples, range.frame_count - head, head, pass_offset + 1};
        plan.segment_count = 2;
        plan.wraps_inside_range = true;
    }
    return plan;
}

/// Whether a track may offer live input while its event chain is compensated.
///
/// Compensation works by reading the future: the program is lowered over the
/// whole timeline before playback begins, so a scheduled event can be issued
/// early and arrive on time. Live input has no future. A note played into a
/// chain with event-domain latency L is heard L late however the software is
/// arranged, so a compensated chain and a live provider cannot both be honoured
/// and the pair is refused rather than silently diverging by L. A per-track
/// monitor bypass is the eventual answer; refusing documents the absence
/// instead of shipping a divergence nothing reports.
constexpr bool event_compensation_admits_live_input(EventCompensationShift shift,
                                                    ProviderSelectorProgram provider) noexcept {
    return !shift.compensating() || !provider.available(ProviderKind::ExternalInput);
}

} // namespace pulp::playback
