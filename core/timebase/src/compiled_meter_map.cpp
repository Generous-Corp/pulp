#include <pulp/timebase/compiled_meter_map.hpp>

#include <algorithm>
#include <limits>

namespace pulp::timebase {
namespace {

bool valid_signature(MeterSignature signature) noexcept {
    if (signature.numerator <= 0 || signature.denominator <= 0)
        return false;
    const auto denominator = static_cast<std::uint32_t>(signature.denominator);
    return (denominator & (denominator - 1)) == 0 &&
           denominator <= static_cast<std::uint32_t>(4 * kTicksPerQuarter) &&
           (4 * kTicksPerQuarter) % denominator == 0;
}

std::int64_t ticks_per_bar(MeterSignature signature) noexcept {
    return static_cast<std::int64_t>(signature.numerator) *
           (4 * kTicksPerQuarter / signature.denominator);
}

std::int64_t floor_div(std::int64_t value, std::int64_t divisor) noexcept {
    auto quotient = value / divisor;
    if (value < 0 && value % divisor != 0)
        --quotient;
    return quotient;
}

std::int64_t saturating_multiply_add(std::int64_t base, std::int64_t multiplier,
                                     std::int64_t value) noexcept {
    if (base > 0 && multiplier > 0 &&
        base > std::numeric_limits<std::int64_t>::max() / multiplier)
        return std::numeric_limits<std::int64_t>::max();
    if (base < 0 && multiplier > 0 &&
        base < std::numeric_limits<std::int64_t>::min() / multiplier)
        return std::numeric_limits<std::int64_t>::min();
    return detail::saturating_add(base * multiplier, value);
}

} // namespace

runtime::Result<MeterMap, MeterMapError>
MeterMap::create(std::span<const MeterPoint> points) noexcept {
    if (points.empty())
        return runtime::Err(MeterMapError::Empty);
    if (points.front().tick.value != 0)
        return runtime::Err(MeterMapError::MissingTickZero);
    for (std::size_t index = 0; index < points.size(); ++index) {
        if (!valid_signature(points[index].signature))
            return runtime::Err(MeterMapError::InvalidSignature);
        if (index > 0 && points[index].tick <= points[index - 1].tick)
            return runtime::Err(MeterMapError::UnorderedPoints);
        if (index > 0) {
            const auto delta = points[index].tick.value - points[index - 1].tick.value;
            const auto previous_per_bar = ticks_per_bar(points[index - 1].signature);
            if (delta % previous_per_bar != 0)
                return runtime::Err(MeterMapError::ChangeNotOnBarBoundary);
        }
    }
    return runtime::Ok(MeterMap(std::vector<MeterPoint>(points.begin(), points.end())));
}

runtime::Result<CompiledMeterMap, MeterMapError>
CompiledMeterMap::compile(std::span<const MeterPoint> points) noexcept {
    auto map = MeterMap::create(points);
    if (!map)
        return runtime::Err(map.error());
    return compile(map.value());
}

runtime::Result<CompiledMeterMap, MeterMapError>
CompiledMeterMap::compile(const MeterMap& map) noexcept {
    std::vector<Segment> segments;
    segments.reserve(map.points().size());
    BarPosition bar{};
    for (std::size_t index = 0; index < map.points().size(); ++index) {
        const auto& point = map.points()[index];
        const auto per_bar = ticks_per_bar(point.signature);
        if (index > 0) {
            const auto delta = point.tick.value - map.points()[index - 1].tick.value;
            const auto previous_per_bar = segments.back().ticks_per_bar;
            if (delta % previous_per_bar != 0)
                return runtime::Err(MeterMapError::ChangeNotOnBarBoundary);
            const auto added = delta / previous_per_bar;
            if (bar.value > std::numeric_limits<std::int64_t>::max() - added)
                return runtime::Err(MeterMapError::RangeExceeded);
            bar.value += added;
        }
        segments.push_back({point.tick, bar, per_bar, point.signature});
    }
    return runtime::Ok(CompiledMeterMap(std::move(segments)));
}

BarTickPosition CompiledMeterMap::tick_to_bar(TickPosition tick) const noexcept {
    const auto it = std::upper_bound(segments_.begin(), segments_.end(), tick,
                                     [](TickPosition value, const Segment& segment) {
                                         return value < segment.start_tick;
                                     });
    const auto& segment = it == segments_.begin() ? segments_.front() : *std::prev(it);
    const auto delta = tick.value - segment.start_tick.value;
    const auto bars = floor_div(delta, segment.ticks_per_bar);
    return {{saturating_multiply_add(1, segment.start_bar.value, bars)},
            {delta - bars * segment.ticks_per_bar}};
}

TickPosition CompiledMeterMap::bar_to_tick(BarPosition bar, TickDuration tick_in_bar) const noexcept {
    const auto it = std::upper_bound(segments_.begin(), segments_.end(), bar,
                                     [](BarPosition value, const Segment& segment) {
                                         return value < segment.start_bar;
                                     });
    const auto& segment = it == segments_.begin() ? segments_.front() : *std::prev(it);
    const auto bars = bar.value - segment.start_bar.value;
    return {saturating_multiply_add(bars, segment.ticks_per_bar,
                                    detail::saturating_add(segment.start_tick.value,
                                                           tick_in_bar.value))};
}

MeterSignature CompiledMeterMap::meter_at_tick(TickPosition tick) const noexcept {
    const auto it = std::upper_bound(segments_.begin(), segments_.end(), tick,
                                     [](TickPosition value, const Segment& segment) {
                                         return value < segment.start_tick;
                                     });
    return (it == segments_.begin() ? segments_.front() : *std::prev(it)).signature;
}

} // namespace pulp::timebase
