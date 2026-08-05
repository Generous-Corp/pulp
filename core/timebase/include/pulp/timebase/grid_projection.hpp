#pragma once

#include <pulp/timebase/beat_division.hpp>
#include <pulp/timebase/compiled_meter_map.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace pulp::timebase {

enum class GridAnchor {
    Timeline,
    Bar,
};

// Stable affine session-clock anchor shared by every host-mapped range in a
// continuous interval. `source_tick` occurs at absolute output `frame`; the
// positive slope is expressed in source ticks per output frame.
struct HostGridAnchor {
    double source_tick = 0.0;
    std::int64_t frame = 0;
    double ticks_per_frame = 0.0;
    constexpr auto operator<=>(const HostGridAnchor&) const = default;
};

// One already-resolved transport range. This mirrors the clock domains a
// playback transport publishes without making timebase depend on playback:
// document samples/ticks may wrap or seek, while monotonic ticks do neither.
struct GridProjectionRange {
    std::uint32_t frame_offset = 0;
    std::uint32_t frame_count = 0;
    SamplePosition timeline_sample_start{};
    TickPosition timeline_tick_start{};
    TickPosition timeline_tick_end{};
    MonotonicBeat monotonic_start{};
    MonotonicBeat monotonic_end{};
    std::uint64_t loop_pass_index = 0;
    bool host_beat_mapping = false;
    double host_tick_start = 0.0;
    double host_tick_end = 0.0;
    bool has_precise_host_ticks = false;
    HostGridAnchor host_anchor{};
    std::int64_t absolute_frame_start = 0;
    double document_to_source_tick_offset = 0.0;
    bool has_host_anchor = false;
    constexpr auto operator<=>(const GridProjectionRange&) const = default;
};

struct GridProjectionRequest {
    BeatDivision division = BeatDivision::Quarter;
    GridAnchor anchor = GridAnchor::Timeline;
    bool playing = true;
};

struct GridProjectionPoint {
    std::uint32_t frame_offset = 0;
    TickPosition timeline_tick{};
    MonotonicBeat transport_tick{};
    BarTickPosition bar_tick{};
    std::uint64_t loop_pass_index = 0;
    constexpr auto operator<=>(const GridProjectionPoint&) const = default;
};

inline constexpr std::size_t kMaximumGridProjectionPoints = 65'536;

enum class GridProjectionError {
    None,
    InvalidDivision,
    InvalidRange,
    SampleRangeExceeded,
    TickRangeExceeded,
    ProjectionLimitExceeded,
    OutputTooSmall,
};

struct GridProjectionResult {
    GridProjectionError error = GridProjectionError::None;
    std::size_t count = 0;
    std::size_t required = 0;

    constexpr explicit operator bool() const noexcept {
        return error == GridProjectionError::None;
    }
};

namespace detail {

constexpr bool checked_grid_add(std::int64_t lhs, std::int64_t rhs, std::int64_t& result) noexcept {
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    if ((rhs > 0 && lhs > max - rhs) || (rhs < 0 && lhs < min - rhs))
        return false;
    result = lhs + rhs;
    return true;
}

constexpr bool checked_grid_subtract(std::int64_t lhs, std::int64_t rhs,
                                     std::int64_t& result) noexcept {
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    if ((rhs > 0 && lhs < min + rhs) || (rhs < 0 && lhs > max + rhs))
        return false;
    result = lhs - rhs;
    return true;
}

// Exact ceiling to a positive quantum. Avoid forming floor(value / quantum) *
// quantum: for INT64_MIN that intermediate can lie below the signed domain even
// when the requested ceiling is representable.
constexpr bool ceil_grid(std::int64_t value, std::int64_t quantum, std::int64_t& result) noexcept {
    if (quantum <= 0)
        return false;
    const auto remainder = value % quantum;
    if (remainder == 0) {
        result = value;
        return true;
    }
    if (remainder < 0)
        return checked_grid_subtract(value, remainder, result);
    return checked_grid_add(value, quantum - remainder, result);
}

inline GridProjectionError validate_range(const GridProjectionRange& range) noexcept {
    if (range.timeline_tick_end < range.timeline_tick_start ||
        range.monotonic_end < range.monotonic_start)
        return GridProjectionError::InvalidRange;
    std::int64_t timeline_duration = 0;
    std::int64_t monotonic_duration = 0;
    if (!checked_grid_subtract(range.timeline_tick_end.value, range.timeline_tick_start.value,
                               timeline_duration) ||
        !checked_grid_subtract(range.monotonic_end.position.value,
                               range.monotonic_start.position.value, monotonic_duration))
        return GridProjectionError::TickRangeExceeded;
    if (timeline_duration != monotonic_duration)
        return GridProjectionError::InvalidRange;
    std::int64_t sample_end = 0;
    if (!checked_grid_add(range.timeline_sample_start.value, range.frame_count, sample_end))
        return GridProjectionError::SampleRangeExceeded;
    std::uint64_t frame_end = 0;
    frame_end = static_cast<std::uint64_t>(range.frame_offset) + range.frame_count;
    if (frame_end > std::numeric_limits<std::uint32_t>::max())
        return GridProjectionError::InvalidRange;
    if (range.host_beat_mapping) {
        const auto tick_start = range.has_precise_host_ticks
                                    ? static_cast<long double>(range.host_tick_start)
                                    : static_cast<long double>(range.timeline_tick_start.value);
        const auto tick_end = range.has_precise_host_ticks
                                  ? static_cast<long double>(range.host_tick_end)
                                  : static_cast<long double>(range.timeline_tick_end.value);
        if (!std::isfinite(tick_start) || !std::isfinite(tick_end) ||
            (range.frame_count != 0 && !(tick_start < tick_end)))
            return GridProjectionError::InvalidRange;
        std::int64_t absolute_frame_end = 0;
        if (!range.has_host_anchor || !std::isfinite(range.host_anchor.source_tick) ||
            !std::isfinite(range.host_anchor.ticks_per_frame) ||
            !(range.host_anchor.ticks_per_frame > 0.0) ||
            !std::isfinite(range.document_to_source_tick_offset) ||
            !checked_grid_add(range.absolute_frame_start, range.frame_count, absolute_frame_end))
            return GridProjectionError::InvalidRange;
    }
    return GridProjectionError::None;
}

inline bool host_mapped_grid_output_offset(const GridProjectionRange& range,
                                           TickPosition document_tick,
                                           std::uint32_t& output_offset) noexcept {
    const auto tick_start = range.has_precise_host_ticks
                                ? static_cast<long double>(range.host_tick_start)
                                : static_cast<long double>(range.timeline_tick_start.value);
    const auto tick_end = range.has_precise_host_ticks
                              ? static_cast<long double>(range.host_tick_end)
                              : static_cast<long double>(range.timeline_tick_end.value);
    if (!range.host_beat_mapping || range.frame_count == 0 || !(tick_start < tick_end))
        return false;
    const auto document = static_cast<long double>(document_tick.value);
    if (document < tick_start || !(document < tick_end))
        return false;
    const auto source_tick = document + range.document_to_source_tick_offset;
    const auto projected =
        static_cast<long double>(range.host_anchor.frame) +
        (source_tick - range.host_anchor.source_tick) / range.host_anchor.ticks_per_frame;
    const auto minimum = static_cast<long double>(std::numeric_limits<std::int64_t>::min());
    const auto maximum = static_cast<long double>(std::numeric_limits<std::int64_t>::max());
    if (projected < minimum || projected > maximum)
        return false;
    auto absolute_frame = static_cast<std::int64_t>(std::floor(projected));
    std::int64_t absolute_frame_end = 0;
    if (!checked_grid_add(range.absolute_frame_start, range.frame_count, absolute_frame_end))
        return false;
    // A loop split rounds its first range up to the next output frame. A source
    // tick just after the wrap therefore belongs to the second range but may
    // floor to the preceding frame on the stable clock; clamp it to the first
    // frame that actually represents the post-wrap document interval.
    if (absolute_frame < range.absolute_frame_start)
        absolute_frame = range.absolute_frame_start;
    if (absolute_frame >= absolute_frame_end)
        absolute_frame = absolute_frame_end - 1;
    const auto local = static_cast<std::uint64_t>(absolute_frame) -
                       static_cast<std::uint64_t>(range.absolute_frame_start);
    const auto total = static_cast<std::uint64_t>(range.frame_offset) + local;
    if (total > std::numeric_limits<std::uint32_t>::max())
        return false;
    output_offset = static_cast<std::uint32_t>(total);
    return true;
}

inline std::uint64_t grid_candidate_count(std::int64_t start, std::int64_t end,
                                          std::int64_t quantum, bool inclusive_end) noexcept {
    if (inclusive_end ? start > end : start >= end)
        return 0;
    const auto last = inclusive_end ? end : end - 1;
    std::int64_t first = 0;
    if (!ceil_grid(start, quantum, first) || first > last)
        return 0;
    const auto distance = static_cast<std::uint64_t>(last) - static_cast<std::uint64_t>(first);
    return distance / static_cast<std::uint64_t>(quantum) + 1U;
}

inline GridProjectionError preflight_candidate_bound(const CompiledMeterMap& meter,
                                                     const GridProjectionRange& range,
                                                     TickDuration grid, GridAnchor anchor,
                                                     std::uint64_t& candidates) noexcept {
    candidates = 0;
    const auto validation = validate_range(range);
    if (validation != GridProjectionError::None)
        return validation;
    if (range.frame_count == 0)
        return GridProjectionError::None;
    // The rounded tick end is only a search bound. Document-clock membership
    // is decided by the half-open sample interval below: on a sparse tick map a
    // one-frame range may have equal rounded endpoints while still owning the
    // grid tick at its first sample.
    const auto inclusive_end = true;
    const auto grid_count = grid_candidate_count(
        range.timeline_tick_start.value, range.timeline_tick_end.value, grid.value, inclusive_end);
    if (grid_count > kMaximumGridProjectionPoints)
        return GridProjectionError::ProjectionLimitExceeded;
    if (anchor == GridAnchor::Timeline) {
        candidates = grid_count;
        return GridProjectionError::None;
    }

    const auto last_tick =
        inclusive_end ? range.timeline_tick_end.value : range.timeline_tick_end.value - 1;
    const auto first_bar = meter.tick_to_bar(range.timeline_tick_start).bar.value;
    const auto last_bar = meter.tick_to_bar({last_tick}).bar.value;
    if (last_bar < first_bar)
        return GridProjectionError::InvalidRange;
    const auto bar_count =
        static_cast<std::uint64_t>(last_bar) - static_cast<std::uint64_t>(first_bar) + 1U;
    // Sum ceil(bar_length / grid) is bounded by the global grid count plus
    // one reset opportunity per bar. Reject conservatively before walking bars.
    if (bar_count > kMaximumGridProjectionPoints ||
        grid_count > kMaximumGridProjectionPoints - bar_count)
        return GridProjectionError::ProjectionLimitExceeded;
    candidates = grid_count + bar_count;
    return GridProjectionError::None;
}

template <typename Emit>
GridProjectionError enumerate_range(const CompiledTempoMap& tempo, const CompiledMeterMap& meter,
                                    const GridProjectionRange& range, TickDuration grid,
                                    GridAnchor anchor, Emit&& emit) noexcept {
    const auto validation = validate_range(range);
    if (validation != GridProjectionError::None)
        return validation;
    if (range.frame_count == 0)
        return GridProjectionError::None;

    std::int64_t sample_end = 0;
    (void)checked_grid_add(range.timeline_sample_start.value, range.frame_count, sample_end);

    auto emit_timeline = [&](std::int64_t timeline_tick) noexcept {
        std::uint32_t output_frame = 0;
        if (range.host_beat_mapping) {
            if (!host_mapped_grid_output_offset(range, {timeline_tick}, output_frame))
                return GridProjectionError::None;
        } else {
            if (timeline_tick < range.timeline_tick_start.value ||
                timeline_tick > range.timeline_tick_end.value)
                return GridProjectionError::None;
            const auto sample = tempo.ticks_to_samples({timeline_tick});
            if (sample.value < range.timeline_sample_start.value || sample.value >= sample_end)
                return GridProjectionError::None;
            const auto local_frame =
                static_cast<std::uint64_t>(sample.value - range.timeline_sample_start.value);
            const auto projected_frame =
                static_cast<std::uint64_t>(range.frame_offset) + local_frame;
            if (projected_frame > std::numeric_limits<std::uint32_t>::max())
                return GridProjectionError::InvalidRange;
            output_frame = static_cast<std::uint32_t>(projected_frame);
        }
        std::int64_t local_tick = 0;
        std::int64_t transport_tick = 0;
        if (!checked_grid_subtract(timeline_tick, range.timeline_tick_start.value, local_tick) ||
            !checked_grid_add(range.monotonic_start.position.value, local_tick, transport_tick))
            return GridProjectionError::TickRangeExceeded;
        return emit(GridProjectionPoint{output_frame,
                                        {timeline_tick},
                                        {{transport_tick}},
                                        meter.tick_to_bar({timeline_tick}),
                                        range.loop_pass_index});
    };

    if (anchor == GridAnchor::Timeline) {
        std::int64_t candidate = 0;
        if (!ceil_grid(range.timeline_tick_start.value, grid.value, candidate))
            return GridProjectionError::TickRangeExceeded;
        while (candidate <= range.timeline_tick_end.value) {
            const auto error = emit_timeline(candidate);
            if (error != GridProjectionError::None)
                return error;
            if (!checked_grid_add(candidate, grid.value, candidate))
                break;
        }
        return GridProjectionError::None;
    }

    auto bar = meter.tick_to_bar(range.timeline_tick_start).bar;
    while (true) {
        const auto bar_start = meter.bar_to_tick(bar);
        if (bar_start > range.timeline_tick_end)
            break;
        if (bar.value == std::numeric_limits<std::int64_t>::max())
            return GridProjectionError::TickRangeExceeded;
        const auto next_bar = meter.bar_to_tick({bar.value + 1});
        if (next_bar <= bar_start)
            return GridProjectionError::TickRangeExceeded;
        std::int64_t local_begin = 0;
        if (range.timeline_tick_start > bar_start &&
            !checked_grid_subtract(range.timeline_tick_start.value, bar_start.value, local_begin))
            return GridProjectionError::TickRangeExceeded;
        std::int64_t local = 0;
        if (!ceil_grid(local_begin, grid.value, local))
            return GridProjectionError::TickRangeExceeded;
        std::int64_t bar_duration = 0;
        if (!checked_grid_subtract(next_bar.value, bar_start.value, bar_duration))
            return GridProjectionError::TickRangeExceeded;
        while (local < bar_duration) {
            std::int64_t candidate = 0;
            if (!checked_grid_add(bar_start.value, local, candidate))
                return GridProjectionError::TickRangeExceeded;
            if (candidate > range.timeline_tick_end.value)
                break;
            const auto error = emit_timeline(candidate);
            if (error != GridProjectionError::None)
                return error;
            if (!checked_grid_add(local, grid.value, local))
                return GridProjectionError::TickRangeExceeded;
        }
        bar.value += 1;
    }
    return GridProjectionError::None;
}

template <typename Emit>
GridProjectionError enumerate_ranges(const CompiledTempoMap& tempo, const CompiledMeterMap& meter,
                                     std::span<const GridProjectionRange> ranges, TickDuration grid,
                                     GridAnchor anchor, Emit&& emit) noexcept {
    std::uint64_t previous_end = 0;
    bool have_previous = false;
    for (const auto& range : ranges) {
        if (have_previous && range.frame_offset < previous_end)
            return GridProjectionError::InvalidRange;
        const auto error = enumerate_range(tempo, meter, range, grid, anchor, emit);
        if (error != GridProjectionError::None)
            return error;
        previous_end = static_cast<std::uint64_t>(range.frame_offset) + range.frame_count;
        have_previous = true;
    }
    return GridProjectionError::None;
}

} // namespace detail

// Projects exact document grid points through transport-owned ranges. Loop
// passes intentionally reuse the same document sample interval; seeks may pair
// any timeline anchor with an independent monotonic anchor. With no mutable
// phase, splitting a range at a callback boundary cannot change coordinates.
// On capacity failure output is untouched.
inline GridProjectionResult project_grid(const CompiledTempoMap& tempo,
                                         const CompiledMeterMap& meter,
                                         const GridProjectionRequest& request,
                                         std::span<const GridProjectionRange> ranges,
                                         std::span<GridProjectionPoint> output) noexcept {
    const auto grid_result = division_ticks(request.division);
    if (!grid_result)
        return {GridProjectionError::InvalidDivision, 0, 0};
    if (!request.playing)
        return {GridProjectionError::None, 0, 0};

    if (ranges.size() > kMaximumGridProjectionPoints)
        return {GridProjectionError::ProjectionLimitExceeded, 0, 0};
    std::uint64_t candidate_total = 0;
    for (const auto& range : ranges) {
        std::uint64_t candidates = 0;
        const auto bound = detail::preflight_candidate_bound(meter, range, grid_result.value(),
                                                             request.anchor, candidates);
        if (bound != GridProjectionError::None)
            return {bound, 0, 0};
        if (candidates > kMaximumGridProjectionPoints - candidate_total)
            return {GridProjectionError::ProjectionLimitExceeded, 0, 0};
        candidate_total += candidates;
    }

    std::size_t required = 0;
    const auto count_error =
        detail::enumerate_ranges(tempo, meter, ranges, grid_result.value(), request.anchor,
                                 [&](const GridProjectionPoint&) noexcept {
                                     if (required == kMaximumGridProjectionPoints)
                                         return GridProjectionError::ProjectionLimitExceeded;
                                     ++required;
                                     return GridProjectionError::None;
                                 });
    if (count_error != GridProjectionError::None)
        return {count_error, 0, required};
    if (required > output.size())
        return {GridProjectionError::OutputTooSmall, 0, required};

    std::size_t count = 0;
    const auto write_error =
        detail::enumerate_ranges(tempo, meter, ranges, grid_result.value(), request.anchor,
                                 [&](const GridProjectionPoint& point) noexcept {
                                     output[count++] = point;
                                     return GridProjectionError::None;
                                 });
    if (write_error != GridProjectionError::None)
        return {write_error, 0, required};
    return {GridProjectionError::None, count, required};
}

} // namespace pulp::timebase
