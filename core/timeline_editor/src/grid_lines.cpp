#include <pulp/timeline_editor/grid_lines.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

namespace pulp::timeline_editor {
namespace {

bool checked_add(std::int64_t lhs, std::int64_t rhs, std::int64_t& result) noexcept {
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    if ((rhs > 0 && lhs > max - rhs) || (rhs < 0 && lhs < min - rhs))
        return false;
    result = lhs + rhs;
    return true;
}

std::int64_t ticks_per_beat(timebase::MeterSignature signature) noexcept {
    return (4 * timebase::kTicksPerQuarter) /
           static_cast<std::int64_t>(signature.denominator);
}

} // namespace

GridLineResult generate_grid_lines(const TickProjection& projection,
                                   const timebase::CompiledMeterMap& meter,
                                   float minimum_spacing_px,
                                   std::span<GridLine> output) noexcept {
    if (!std::isfinite(minimum_spacing_px))
        return {GridLineError::NonFiniteSpacing, 0};
    if (!(minimum_spacing_px > 0.0f))
        return {GridLineError::NonPositiveSpacing, 0};

    const auto start = projection.visible_start().value;
    const auto end = projection.visible_end().value;
    auto bar = meter.tick_to_bar(projection.visible_start()).bar;
    std::size_t count = 0;

    const auto append = [&](std::int64_t tick, GridLineLevel level) noexcept {
        if (count == output.size())
            return false;
        output[count++] = GridLine{{tick}, projection.x_at({tick}), level};
        return true;
    };

    while (true) {
        const auto bar_start = meter.bar_to_tick(bar);
        if (bar_start.value > end)
            break;
        if (bar_start.value >= start &&
            !append(bar_start.value, GridLineLevel::Bar))
            return {GridLineError::OutputTooSmall, count};

        const auto signature = meter.meter_at_tick(bar_start);
        const auto beat_ticks = ticks_per_beat(signature);
        if (beat_ticks <= 0)
            return {GridLineError::RangeOverflow, count};
        if (bar.value == std::numeric_limits<std::int64_t>::max())
            return {GridLineError::RangeOverflow, count};
        const auto next_bar = meter.bar_to_tick({bar.value + 1});
        if (next_bar.value <= bar_start.value)
            return {GridLineError::RangeOverflow, count};

        std::int64_t beat = 0;
        if (!checked_add(bar_start.value, beat_ticks, beat))
            return {GridLineError::RangeOverflow, count};
        const auto bar_x = projection.x_at(bar_start);
        while (beat < next_bar.value && beat <= end) {
            if (beat >= start &&
                std::fabs(projection.x_at({beat}) - bar_x) >= minimum_spacing_px &&
                !append(beat, GridLineLevel::Beat))
                return {GridLineError::OutputTooSmall, count};
            if (!checked_add(beat, beat_ticks, beat))
                return {GridLineError::RangeOverflow, count};
        }
        bar.value += 1;
    }
    return {GridLineError::None, count};
}

} // namespace pulp::timeline_editor
