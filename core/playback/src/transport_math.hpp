#pragma once

#include <pulp/playback/transport.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

namespace pulp::playback::detail {

inline timebase::BarPosition bar_at_tick(timebase::TickPosition tick,
                                         timebase::TickPosition anchor_tick,
                                         timebase::BarPosition anchor_bar,
                                         MeterSignature meter) noexcept {
    const long double ticks_per_bar =
        static_cast<long double>(timebase::kTicksPerQuarter) *
        static_cast<long double>(meter.numerator) * 4.0L /
        static_cast<long double>(meter.denominator);
    const long double relative_tick = static_cast<long double>(tick.value) -
                                      static_cast<long double>(anchor_tick.value);
    const long double projected = static_cast<long double>(anchor_bar.value) +
                                  std::floor(relative_tick / ticks_per_bar);
    if (projected >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return {std::numeric_limits<std::int64_t>::max()};
    if (projected <= static_cast<long double>(std::numeric_limits<std::int64_t>::min()))
        return {std::numeric_limits<std::int64_t>::min()};
    return {static_cast<std::int64_t>(projected)};
}

} // namespace pulp::playback::detail
