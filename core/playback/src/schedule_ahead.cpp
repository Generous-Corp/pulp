#include <pulp/playback/schedule_ahead.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include "transport_math.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace pulp::playback {
namespace {

bool same_rate(timebase::RationalRate lhs, timebase::RationalRate rhs) noexcept {
    return lhs.valid() && rhs.valid() && lhs.normalized() == rhs.normalized();
}

std::uint64_t distance(timebase::SamplePosition start,
                       timebase::SamplePosition end) noexcept {
    if (end <= start) return 0;
    return static_cast<std::uint64_t>(end.value) -
           static_cast<std::uint64_t>(start.value);
}

timebase::SamplePosition add_unsigned(timebase::SamplePosition start,
                                      std::uint64_t offset) noexcept {
    const auto bits = std::bit_cast<std::uint64_t>(start.value) + offset;
    return {std::bit_cast<std::int64_t>(bits)};
}

timebase::TickDuration multiply_duration(timebase::TickDuration duration,
                                         std::uint64_t multiplier) noexcept {
    if (duration.value <= 0 || multiplier == 0) return {};
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    if (multiplier > static_cast<std::uint64_t>(maximum / duration.value))
        return {maximum};
    return {duration.value * static_cast<std::int64_t>(multiplier)};
}

std::int64_t saturating_add(std::int64_t lhs, std::int64_t rhs) noexcept {
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    if (rhs > 0 && lhs > maximum - rhs) return maximum;
    return lhs + rhs;
}

} // namespace

ScheduleAheadCode project_schedule_ahead(
    const TransportSnapshot& base, std::int64_t lead_samples,
    TransportSnapshot& projected) noexcept {
    runtime::ScopedNoAlloc no_alloc;
    if (!valid_transport_ranges(base) ||
        !same_rate(base.sample_rate, base.tempo_map->sample_rate())) {
        return ScheduleAheadCode::InvalidTransport;
    }
    if (lead_samples < 0) return ScheduleAheadCode::InvalidLead;
    projected = base;
    if (!base.is_playing || lead_samples == 0) return ScheduleAheadCode::Ok;
    if (base.ranges[0].discontinuity &&
        base.ranges[0].discontinuity_reason ==
            TransportDiscontinuityReason::None &&
        !base.reset_requested) {
        return ScheduleAheadCode::InvalidTransport;
    }

    const auto lead = static_cast<std::uint64_t>(lead_samples);
    const auto base_start = base.ranges[0].timeline_sample_start;
    auto projected_start = base_start;
    auto projected_tick = base.ranges[0].timeline_tick_start;
    auto projected_monotonic = base.ranges[0].monotonic_start;
    bool wrap_at_projected_start = false;
    timebase::SamplePosition loop_start{};
    timebase::SamplePosition loop_end{};
    std::uint64_t loop_length = 0;

    if (!base.loop.enabled) {
        const auto maximum = std::numeric_limits<std::int64_t>::max();
        if (base_start.value > maximum - static_cast<std::int64_t>(lead)) {
            return ScheduleAheadCode::SampleRangeExceeded;
        }
        projected_start.value += static_cast<std::int64_t>(lead);
        if (projected_start.value >
            maximum - static_cast<std::int64_t>(base.frame_count)) {
            return ScheduleAheadCode::SampleRangeExceeded;
        }
        projected_tick = base.tempo_map->resolve_sample(projected_start).tick;
        projected_monotonic = projected_monotonic +
            (projected_tick - base.ranges[0].timeline_tick_start);
    } else {
        loop_start = base.tempo_map->ticks_to_samples(base.loop.start);
        loop_end = base.tempo_map->ticks_to_samples(base.loop.end);
        loop_length = distance(loop_start, loop_end);
        if (loop_length == 0 || loop_length < base.frame_count ||
            base_start >= loop_end) {
            return ScheduleAheadCode::InvalidLoop;
        }
        const auto until_wrap = distance(base_start, loop_end);
        const bool crosses_wrap = lead >= until_wrap;
        const auto after_first_wrap = crosses_wrap ? lead - until_wrap : 0;
        const auto advanced_offset = crosses_wrap
            ? after_first_wrap % loop_length
            : 0;
        projected_start = crosses_wrap
            ? add_unsigned(loop_start, advanced_offset)
            : add_unsigned(base_start, lead);
        projected_tick = crosses_wrap && advanced_offset == 0
            ? base.loop.start
            : base.tempo_map->resolve_sample(projected_start).tick;
        wrap_at_projected_start = crosses_wrap && advanced_offset == 0;

        timebase::TickDuration lead_ticks{};
        if (!crosses_wrap) {
            lead_ticks = projected_tick - base.ranges[0].timeline_tick_start;
        } else {
            lead_ticks = base.loop.end - base.ranges[0].timeline_tick_start;
            const auto complete_loops = after_first_wrap / loop_length;
            lead_ticks = {saturating_add(
                lead_ticks.value,
                multiply_duration(base.loop.end - base.loop.start,
                                  complete_loops).value)};
            lead_ticks = {saturating_add(
                lead_ticks.value,
                (projected_tick - base.loop.start).value)};
        }
        projected_monotonic = projected_monotonic + lead_ticks;
    }

    projected.ranges = {};
    const auto make_range = [&](std::uint8_t index, std::uint32_t sample_offset,
                                std::uint32_t frame_count,
                                timebase::SamplePosition sample_start,
                                timebase::TickPosition tick_start,
                                timebase::MonotonicBeat monotonic_start,
                                TransportDiscontinuityReason discontinuity,
                                const timebase::TickPosition* forced_end = nullptr) {
        auto& range = projected.ranges[index];
        range.sample_offset = sample_offset;
        range.frame_count = frame_count;
        range.timeline_sample_start = sample_start;
        range.timeline_tick_start = tick_start;
        range.timeline_tick_end = forced_end != nullptr
            ? *forced_end
            : base.tempo_map->resolve_sample(
                  {sample_start.value + static_cast<std::int64_t>(frame_count)}).tick;
        if (range.timeline_tick_end < range.timeline_tick_start)
            range.timeline_tick_end = range.timeline_tick_start;
        range.monotonic_start = monotonic_start;
        range.monotonic_end = monotonic_start +
                              (range.timeline_tick_end - range.timeline_tick_start);
        range.bar_start = detail::bar_at_tick(
            range.timeline_tick_start, projected.meter_anchor_tick,
            projected.meter_anchor_bar, projected.meter);
        range.tempo_bpm = base.tempo_map->tempo_at_tick(range.timeline_tick_start);
        range.tempo_changed = index == 0
            ? base.ranges[0].tempo_changed ||
                  range.tempo_bpm != base.ranges[0].tempo_bpm
            : range.tempo_bpm != projected.ranges[index - 1].tempo_bpm;
        range.discontinuity_reason = discontinuity;
        range.discontinuity = discontinuity != TransportDiscontinuityReason::None;
    };

    auto external_discontinuity = TransportDiscontinuityReason::None;
    if (base.reset_requested ||
        base.ranges[0].discontinuity_reason == TransportDiscontinuityReason::Seek) {
        external_discontinuity = TransportDiscontinuityReason::Seek;
    } else if (base.ranges[0].discontinuity_reason ==
               TransportDiscontinuityReason::LoopConfiguration) {
        external_discontinuity = TransportDiscontinuityReason::LoopConfiguration;
    }
    const auto first_discontinuity = external_discontinuity !=
            TransportDiscontinuityReason::None
        ? external_discontinuity
        : wrap_at_projected_start
            ? TransportDiscontinuityReason::LoopWrap
            : TransportDiscontinuityReason::None;
    if (!base.loop.enabled) {
        make_range(0, 0, base.frame_count, projected_start, projected_tick,
                   projected_monotonic, first_discontinuity);
        projected.range_count = 1;
    } else {
        const auto until_wrap = distance(projected_start, loop_end);
        const auto first_count = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(base.frame_count, until_wrap));
        if (first_count != 0) {
            const auto* forced_end = first_count == until_wrap
                ? &base.loop.end
                : nullptr;
            make_range(0, 0, first_count, projected_start, projected_tick,
                       projected_monotonic, first_discontinuity, forced_end);
            projected.range_count = 1;
        }
        const auto remaining = base.frame_count - first_count;
        if (remaining != 0) {
            const auto index = projected.range_count;
            const auto monotonic_start = index == 0
                ? projected_monotonic
                : projected.ranges[index - 1].monotonic_end;
            make_range(index, first_count, remaining, loop_start, base.loop.start,
                       monotonic_start, TransportDiscontinuityReason::LoopWrap);
            ++projected.range_count;
        }
    }
    projected.tempo_bpm = projected.ranges[0].tempo_bpm;
    return ScheduleAheadCode::Ok;
}

} // namespace pulp::playback
