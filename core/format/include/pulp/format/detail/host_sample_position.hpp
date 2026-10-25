#pragma once

// A host's floating-point timeline sample position → an integer sample.
//
// Audio Unit hosts report the timeline position as a double
// (CallHostTransportState's outCurrentSampleInTimeLine, and AUv3's
// transportStateBlock currentSamplePosition). Some hosts send values a hair
// below the true integer: REAPER reports 13823.9999 for sample 13824.
// Truncating that steps the position back one sample and then jumps it two
// forward on the next block, so the position is rounded to the nearest sample.
// The same rounding places AUv3 render events within the block and stamps
// AUv3 MIDI output, and bounds CLAP's seconds-to-samples conversion.
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

/// An AUv3 render event's offset into the block, clamped to
/// [0, frame_count - 1]. `immediate` is AUEventSampleTimeImmediate: times in
/// [immediate, immediate + 4096) are already block offsets. Otherwise the
/// offset is the event time minus the block's start, `block_sample_time`
/// rounded to the nearest sample; when the block time is unavailable or
/// unrepresentable, the event time itself is taken as the offset.
inline std::int32_t au_event_block_offset(std::int64_t event_sample_time, std::int64_t immediate,
                                          std::optional<double> block_sample_time,
                                          std::uint32_t frame_count) noexcept {
    constexpr std::int64_t kImmediateWindow = 4096;
    std::int64_t offset = event_sample_time;
    if (event_sample_time >= immediate && event_sample_time < immediate + kImmediateWindow) {
        offset = event_sample_time - immediate;
    } else if (block_sample_time) {
        if (const auto start = host_sample_position(*block_sample_time))
            offset = event_sample_time - *start;
    }
    if (offset < 0)
        return 0;
    if (frame_count > 0 && offset >= static_cast<std::int64_t>(frame_count))
        return static_cast<std::int32_t>(frame_count - 1);
    if (offset > static_cast<std::int64_t>(INT32_MAX))
        return INT32_MAX;
    return static_cast<std::int32_t>(offset);
}

/// The AUv3 event time for a MIDI event `sample_offset` frames into a block
/// that starts at `block_sample_time`, rounded to the nearest sample. When
/// the block time is unrepresentable the event is sent `immediate +
/// sample_offset` (AUEventSampleTimeImmediate plus the offset).
inline std::int64_t au_event_sample_time(double block_sample_time, std::int32_t sample_offset,
                                         std::int64_t immediate) noexcept {
    if (const auto start = host_sample_position(block_sample_time))
        return *start + sample_offset;
    return immediate + sample_offset;
}

} // namespace pulp::format::detail
