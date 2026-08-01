#include <pulp/timeline_editor/snap_grid.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace pulp::timeline_editor {
namespace {

std::int64_t ticks_per_bar(timebase::MeterSignature signature) noexcept {
    const auto ticks_per_beat =
        4 * timebase::kTicksPerQuarter / static_cast<std::int64_t>(signature.denominator);
    return static_cast<std::int64_t>(signature.numerator) * ticks_per_beat;
}

std::uint64_t distance(std::int64_t later, std::int64_t earlier) noexcept {
    // Unsigned subtraction is defined across the full signed span. With the
    // operands ordered it is the exact distance, including INT64_MIN→MAX.
    return static_cast<std::uint64_t>(later) - static_cast<std::uint64_t>(earlier);
}

void add_candidate(std::array<std::int64_t, 8>& candidates, std::size_t& count,
                   std::int64_t candidate) noexcept {
    const auto end = candidates.begin() + static_cast<std::ptrdiff_t>(count);
    if (count < candidates.size() && std::find(candidates.begin(), end, candidate) == end) {
        candidates[count++] = candidate;
    }
}

} // namespace

runtime::Result<SnapGrid, SnapGridError> SnapGrid::create(timebase::TickDuration interval,
                                                          timebase::SwingRatio swing) noexcept {
    if (!timebase::valid_swing_grid(interval))
        return runtime::Err(SnapGridError::InvalidInterval);
    if (!timebase::valid_swing_ratio(swing))
        return runtime::Err(SnapGridError::InvalidSwingRatio);
    return runtime::Ok(SnapGrid(interval, swing));
}

timebase::TickPosition SnapGrid::snap(const timebase::CompiledMeterMap& meter_map,
                                      timebase::TickPosition position,
                                      SnapDirection direction) const noexcept {
    const auto bar_position = meter_map.tick_to_bar(position);
    const auto bar_length = ticks_per_bar(meter_map.meter_at_tick(position));

    // Swing is monotonic. Straightening the pointer first identifies the two
    // neighboring straight-grid indices; warping only those candidates avoids
    // floating-point beat math and remains exact at the int64 endpoints.
    const auto straight =
        timebase::unswing_position({bar_position.tick_in_bar.value}, interval_, swing_).value;
    const auto central_index = straight / interval_.value;

    std::array<std::int64_t, 8> local_candidates{};
    std::size_t candidate_count = 0;
    add_candidate(local_candidates, candidate_count, 0);
    add_candidate(local_candidates, candidate_count, bar_length);

    for (std::int64_t offset = -2; offset <= 2; ++offset) {
        if (offset < 0 && central_index < -offset)
            continue;
        const auto index = central_index + offset;
        if (index < 0 || index > bar_length / interval_.value)
            continue;
        const auto straight_boundary = index * interval_.value;
        const auto swung = timebase::swing_position({straight_boundary}, interval_, swing_).value;
        if (swung > 0 && swung < bar_length)
            add_candidate(local_candidates, candidate_count, swung);
    }

    auto before = std::numeric_limits<std::int64_t>::min();
    auto after = std::numeric_limits<std::int64_t>::max();
    for (std::size_t index = 0; index < candidate_count; ++index) {
        const auto candidate =
            meter_map.bar_to_tick(bar_position.bar, {local_candidates[index]}).value;
        if (candidate <= position.value)
            before = std::max(before, candidate);
        if (candidate >= position.value)
            after = std::min(after, candidate);
    }

    if (direction == SnapDirection::AtOrBefore)
        return {before};
    if (direction == SnapDirection::AtOrAfter)
        return {after};

    const auto distance_before = distance(position.value, before);
    const auto distance_after = distance(after, position.value);
    return {distance_after <= distance_before ? after : before};
}

} // namespace pulp::timeline_editor
