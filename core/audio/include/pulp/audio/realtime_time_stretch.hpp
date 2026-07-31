#pragma once

/// @file realtime_time_stretch.hpp
/// Audio-domain facade for the prepared realtime time-stretch processor.
///
/// Higher engine layers depend on audio, not on the spectral implementation
/// in signal. Keeping these names at the audio boundary lets the underlying
/// processor evolve without inverting the timeline engine dependency floor.

#include <pulp/signal/realtime_pitch_time_processor.hpp>

#include <cstdint>

namespace pulp::audio {

using RealtimeTimeStretchQuality = signal::PitchTimeQuality;
using RealtimeTimeStretchMode = signal::PitchTimeMode;
using RealtimeTimeStretchPrepareStatus = signal::PitchTimePrepareStatus;
using RealtimeTimeStretchStreamFeedStatus = signal::PitchTimeStreamFeedStatus;
using RealtimeTimeStretchStreamFinalizeStatus = signal::PitchTimeStreamFinalizeStatus;
using RealtimeTimeStretchStreamFinalizePlanStatus =
    signal::PitchTimeStreamFinalizePlanStatus;
using RealtimeTimeStretchStreamFinalizePlan = signal::PitchTimeStreamFinalizePlan;
using RealtimeTimeStretchConfig = signal::RealtimePitchTimeConfig;
using RealtimeTimeStretchProcessor = signal::RealtimePitchTimeProcessor;

template <typename SampleType>
using RealtimeTimeStretchPreparedGeometry =
    signal::RealtimePitchTimePreparedGeometry<SampleType>;

inline constexpr int kRealtimeTimeStretchMaximumChannels =
    signal::kRealtimePitchTimeMaximumChannels;

template <typename SampleType>
RealtimeTimeStretchPrepareStatus checked_realtime_time_stretch_prepared_geometry(
    const RealtimeTimeStretchConfig& config, double max_pitch_ratio,
    std::uint64_t requested_max_bytes,
    RealtimeTimeStretchPreparedGeometry<SampleType>& prepared) noexcept {
    return signal::checked_realtime_pitch_time_prepared_geometry(
        config, max_pitch_ratio, requested_max_bytes, prepared);
}

} // namespace pulp::audio
