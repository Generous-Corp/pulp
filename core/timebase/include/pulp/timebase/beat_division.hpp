#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timebase/tick.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace pulp::timebase {

// Persisted ordinal vocabulary. New divisions are appended immediately before
// Count; existing entries must never be reordered or reused.
enum class BeatDivision : std::uint8_t {
    Whole = 0,
    WholeDotted = 1,
    WholeTriplet = 2,
    Half = 3,
    HalfDotted = 4,
    HalfTriplet = 5,
    Quarter = 6,
    QuarterDotted = 7,
    QuarterTriplet = 8,
    Eighth = 9,
    EighthDotted = 10,
    EighthTriplet = 11,
    Sixteenth = 12,
    SixteenthDotted = 13,
    SixteenthTriplet = 14,
    ThirtySecond = 15,
    ThirtySecondDotted = 16,
    ThirtySecondTriplet = 17,
    SixtyFourth = 18,
    SixtyFourthDotted = 19,
    SixtyFourthTriplet = 20,
    Count = 21,
};

struct BeatFraction {
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;
    constexpr auto operator<=>(const BeatFraction&) const = default;
};

enum class BeatDivisionError {
    InvalidDivision,
    NotExactlyRepresentable,
    RangeExceeded,
};

// Exact quarter-note beat value. Fractions are reduced and positive.
inline runtime::Result<BeatFraction, BeatDivisionError>
beat_fraction(BeatDivision division) noexcept {
    constexpr BeatFraction values[] = {
        {4, 1}, {6, 1}, {8, 3}, {2, 1}, {3, 1}, {4, 3},  {1, 1},  {3, 2},  {2, 3},  {1, 2},  {3, 4},
        {1, 3}, {1, 4}, {3, 8}, {1, 6}, {1, 8}, {3, 16}, {1, 12}, {1, 16}, {3, 32}, {1, 24},
    };
    static_assert(sizeof(values) / sizeof(values[0]) ==
                  static_cast<std::size_t>(BeatDivision::Count));
    const auto index = static_cast<std::uint8_t>(division);
    if (index >= static_cast<std::uint8_t>(BeatDivision::Count))
        return runtime::Err(BeatDivisionError::InvalidDivision);
    return runtime::Ok(values[index]);
}

// Converts the vocabulary to the repository's exact document tick lattice.
// Failure is explicit if a future appended fraction does not divide that
// lattice or would exceed TickDuration.
inline runtime::Result<TickDuration, BeatDivisionError>
division_ticks(BeatDivision division) noexcept {
    const auto fraction = beat_fraction(division);
    if (!fraction)
        return runtime::Err(fraction.error());
    const auto value = fraction.value();
    if (value.denominator <= 0 || kTicksPerQuarter % value.denominator != 0)
        return runtime::Err(BeatDivisionError::NotExactlyRepresentable);
    const auto unit = kTicksPerQuarter / value.denominator;
    if (value.numerator > 0 && unit > std::numeric_limits<std::int64_t>::max() / value.numerator)
        return runtime::Err(BeatDivisionError::RangeExceeded);
    return runtime::Ok(TickDuration{unit * value.numerator});
}

} // namespace pulp::timebase
