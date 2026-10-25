#pragma once

// A host's floating-point timeline sample position → an integer sample.
//
// Audio Unit hosts report the timeline position as a double
// (CallHostTransportState's outCurrentSampleInTimeLine, and AUv3's
// transportStateBlock currentSamplePosition). Some hosts send values a hair
// below the true integer: REAPER reports 13823.9999 for sample 13824.
// Truncating that steps the position back one sample and then jumps it two
// forward on the next block, so the position is rounded to the nearest sample.
//
// Header-only and SDK-free so it is unit-testable without the AU SDK.

#include <cmath>
#include <cstdint>
#include <optional>

namespace pulp::format::detail {

/// The nearest integer sample to `samples`, or nullopt when the host's value
/// is not finite or does not fit in int64 (the position is then left unset).
inline std::optional<std::int64_t> host_sample_position(double samples) noexcept {
    if (!std::isfinite(samples))
        return std::nullopt;
    // std::round, not nearbyint: independent of the floating-point rounding
    // mode, and a half sample rounds away from zero.
    const double rounded = std::round(samples);
    // 2^63 is exactly representable; every double below it fits in int64.
    constexpr double kLimit = 9223372036854775808.0;
    if (rounded >= kLimit || rounded < -kLimit)
        return std::nullopt;
    return static_cast<std::int64_t>(rounded);
}

} // namespace pulp::format::detail
