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
// `au_event_raw_offset()` is the event placement the AUv3 MIDI admission,
// the parameter path and the host event timeline share; each applies its own
// range policy.
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

/// Width of the `AUEventSampleTimeImmediate` window: event times in
/// [immediate, immediate + kAuImmediateWindow) are already block offsets.
/// Apple's sentinel is negative (`0xffffffff00000000`), so non-negative
/// sample times remain absolute positions on the render axis.
inline constexpr std::int64_t kAuImmediateWindow = 4096;

/// The unbounded offset of an AUv3 render event from its block's first frame,
/// or nullopt when the event cannot be placed. `immediate` is
/// AUEventSampleTimeImmediate. A time inside the immediate window is already an
/// offset. Any other time is absolute on the `mSampleTime` axis and is placed
/// against `block_sample_time` rounded to the nearest sample; when the block
/// time is absent or unrepresentable, or the difference overflows, the event
/// cannot be placed.
///
/// The result is not range-checked. Each caller applies its own policy: the
/// parameter path clamps (`au_event_block_offset`), while MIDI admission
/// (`pulp::format::admit_au_event_time`) and the host timeline
/// (`pulp::host::au_event_block_time`) refuse anything outside the block.
inline std::optional<std::int64_t>
au_event_raw_offset(std::int64_t event_sample_time, std::int64_t immediate,
                    std::optional<double> block_sample_time) noexcept {
    if (event_sample_time >= immediate && event_sample_time < immediate + kAuImmediateWindow)
        return event_sample_time - immediate;
    if (!block_sample_time)
        return std::nullopt;
    const auto start = host_sample_position(*block_sample_time);
    // Portable overflow check (MSVC builds this header too).
    if (!start || (*start > 0 && event_sample_time < INT64_MIN + *start) ||
        (*start < 0 && event_sample_time > INT64_MAX + *start))
        return std::nullopt;
    return event_sample_time - *start;
}

/// An AUv3 render event's offset into the block, clamped to
/// [0, frame_count - 1]. The offset is `au_event_raw_offset()`; when that
/// cannot place the event, the event time itself is taken as the offset. This
/// is the parameter-event delivery policy: an out-of-block time is moved to the
/// nearest block edge rather than dropped.
inline std::int32_t au_event_block_offset(std::int64_t event_sample_time, std::int64_t immediate,
                                          std::optional<double> block_sample_time,
                                          std::uint32_t frame_count) noexcept {
    const std::int64_t offset = au_event_raw_offset(event_sample_time, immediate, block_sample_time)
                                    .value_or(event_sample_time);
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
/// the block time is unrepresentable, or the sum overflows, the event is sent
/// at `immediate + sample_offset` (AUEventSampleTimeImmediate plus the offset).
inline std::int64_t au_event_sample_time(double block_sample_time, std::int32_t sample_offset,
                                         std::int64_t immediate) noexcept {
    const auto start = host_sample_position(block_sample_time);
    if (start && !(sample_offset > 0 && *start > INT64_MAX - sample_offset) &&
        !(sample_offset < 0 && *start < INT64_MIN - sample_offset))
        return *start + sample_offset;
    return immediate + sample_offset;
}

} // namespace pulp::format::detail
