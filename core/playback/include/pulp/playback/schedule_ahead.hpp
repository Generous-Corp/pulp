#pragma once

#include <pulp/playback/transport.hpp>

#include <cstdint>

namespace pulp::playback {

enum class ScheduleAheadCode : std::uint8_t {
    Ok,
    InvalidTransport,
    InvalidLead,
    InvalidLoop,
    SampleRangeExceeded,
};

inline constexpr audio::RtSafetyClass project_schedule_ahead_rt_safety_class =
    audio::RtSafetyClass::AudioCallbackSafeWithImmutableInputs;

/// Projects one callback's source-query window ahead by `lead_samples` while
/// preserving its block-local offsets. The projection is allocation-free and
/// host-agnostic. A stopped transport is copied unchanged. Projected snapshots
/// are renderer-query windows only: their inherited callback control flags must
/// not be projected into a graph-wide ProcessContext.
ScheduleAheadCode project_schedule_ahead(
    const TransportSnapshot& base, std::int64_t lead_samples,
    TransportSnapshot& projected) noexcept;

} // namespace pulp::playback
