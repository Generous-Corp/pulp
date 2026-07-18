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

struct Division {
    std::int64_t quotient = 0;
    std::int64_t remainder = 0;
};

// Euclidean division for a positive divisor. Keeping the remainder directly
// avoids reconstructing it as value - quotient * divisor: that multiplication
// can fall below INT64_MIN for negative, non-divisible values.
Division floor_divmod(std::int64_t value, std::int64_t divisor) noexcept {
    auto quotient = value / divisor;
    auto remainder = value % divisor;
    if (remainder < 0) {
        --quotient;
        remainder += divisor;
    }
    return {quotient, remainder};
}

std::int64_t saturating_multiply_add(std::int64_t base, std::int64_t multiplier,
                                     std::int64_t first, std::int64_t second) noexcept {
    // Decompose both addends before combining them. This preserves cancellation
    // across all three terms in base * multiplier + first + second; saturating
    // either addend or the product early would produce the wrong endpoint.
    // MeterMap validation guarantees multiplier >= 11,025. The sum of two
    // int64 quotients (and its optional carry) therefore remains in int64.
    const auto first_parts = floor_divmod(first, multiplier);
    const auto second_parts = floor_divmod(second, multiplier);
    auto correction = first_parts.quotient + second_parts.quotient;
    std::int64_t remainder = 0;
    if (first_parts.remainder >= multiplier - second_parts.remainder) {
        ++correction;
        remainder = first_parts.remainder - (multiplier - second_parts.remainder);
    } else {
        remainder = first_parts.remainder + second_parts.remainder;
    }
    if (correction > 0 && base > std::numeric_limits<std::int64_t>::max() - correction)
        return std::numeric_limits<std::int64_t>::max();
    if (correction < 0 && base < std::numeric_limits<std::int64_t>::min() - correction)
        return std::numeric_limits<std::int64_t>::min();
    const auto coefficient = base + correction;

    const auto minimum_parts =
        floor_divmod(std::numeric_limits<std::int64_t>::min(), multiplier);
    const auto maximum_parts =
        floor_divmod(std::numeric_limits<std::int64_t>::max(), multiplier);
    if (coefficient < minimum_parts.quotient ||
        (coefficient == minimum_parts.quotient &&
         remainder < minimum_parts.remainder))
        return std::numeric_limits<std::int64_t>::min();
    if (coefficient > maximum_parts.quotient ||
        (coefficient == maximum_parts.quotient &&
         remainder > maximum_parts.remainder))
        return std::numeric_limits<std::int64_t>::max();

    // The minimum quotient's product alone is below INT64_MIN; materialize
    // the already-proven in-range result relative to INT64_MIN instead.
    if (coefficient == minimum_parts.quotient) {
        return std::numeric_limits<std::int64_t>::min() +
               (remainder - minimum_parts.remainder);
    }
    return coefficient * multiplier + remainder;
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
            const auto delta = detail::saturating_subtract(points[index].tick.value,
                                                           points[index - 1].tick.value);
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
            const auto delta = detail::saturating_subtract(
                point.tick.value, map.points()[index - 1].tick.value);
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
    const auto delta = detail::saturating_subtract(tick.value, segment.start_tick.value);
    const auto parts = floor_divmod(delta, segment.ticks_per_bar);
    return {{detail::saturating_add(segment.start_bar.value, parts.quotient)},
            {parts.remainder}};
}

TickPosition CompiledMeterMap::bar_to_tick(BarPosition bar, TickDuration tick_in_bar) const noexcept {
    const auto it = std::upper_bound(segments_.begin(), segments_.end(), bar,
                                     [](BarPosition value, const Segment& segment) {
                                         return value < segment.start_bar;
                                     });
    const auto& segment = it == segments_.begin() ? segments_.front() : *std::prev(it);
    const auto bars = detail::saturating_subtract(bar.value, segment.start_bar.value);
    return {saturating_multiply_add(bars, segment.ticks_per_bar,
                                    segment.start_tick.value, tick_in_bar.value)};
}

MeterSignature CompiledMeterMap::meter_at_tick(TickPosition tick) const noexcept {
    const auto it = std::upper_bound(segments_.begin(), segments_.end(), tick,
                                     [](TickPosition value, const Segment& segment) {
                                         return value < segment.start_tick;
                                     });
    return (it == segments_.begin() ? segments_.front() : *std::prev(it)).signature;
}

} // namespace pulp::timebase
