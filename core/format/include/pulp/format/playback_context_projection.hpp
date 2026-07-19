#pragma once

#include <pulp/format/process_context.hpp>
#include <pulp/playback/transport.hpp>

#include <cstdint>
#include <limits>

namespace pulp::format {

/// Format-layer metadata that is intentionally absent from the engine-owned
/// transport snapshot. Defaults preserve ProcessContext's host-unavailable
/// sentinels.
struct PlaybackContextProjectionOptions {
    ProcessMode process_mode = ProcessMode::Realtime;
    RenderSpeedHint render_speed_hint = RenderSpeedHint::Unknown;
    bool is_bypassed = false;
    bool is_tail_drain = false;
    std::int64_t host_time_ns = 0;
    FrameRate frame_rate = FrameRate::unknown;
};

/// Lossy, one-way adapter from the integer-authoritative playback snapshot to
/// the public format ABI. Engine code never includes a format header.
inline ProcessContext project_process_context(
    const playback::TransportSnapshot& snapshot,
    const playback::TransportRange& range,
    const PlaybackContextProjectionOptions& options = {}) noexcept {
    ProcessContext context;
    context.sample_rate = static_cast<double>(snapshot.sample_rate.as_long_double());
    context.num_samples = range.frame_count >
                                  static_cast<std::uint32_t>(std::numeric_limits<int>::max())
                              ? std::numeric_limits<int>::max()
                              : static_cast<int>(range.frame_count);
    context.process_mode = options.process_mode;
    context.is_bypassed = options.is_bypassed;
    context.is_tail_drain = options.is_tail_drain;
    context.render_speed_hint = options.render_speed_hint;
    context.is_playing = snapshot.is_playing;
    context.is_recording = false;
    context.tempo_bpm = range.tempo_bpm;
    context.position_beats = static_cast<double>(range.timeline_tick_start.value) /
                             static_cast<double>(timebase::kTicksPerQuarter);
    context.position_samples = range.timeline_sample_start.value;
    context.time_sig_numerator = snapshot.meter.numerator;
    context.time_sig_denominator = snapshot.meter.denominator;
    context.bar = range.bar_start.value;
    context.is_looping = snapshot.loop.enabled;
    if (snapshot.loop.enabled) {
        context.loop_start_beats = static_cast<double>(snapshot.loop.start.value) /
                                   static_cast<double>(timebase::kTicksPerQuarter);
        context.loop_end_beats = static_cast<double>(snapshot.loop.end.value) /
                                 static_cast<double>(timebase::kTicksPerQuarter);
    }
    context.host_time_ns = options.host_time_ns;
    context.frame_rate = options.frame_rate;

    const bool first_range = range.sample_offset == 0;
    context.tempo_changed = range.tempo_changed;
    context.time_sig_changed = first_range && snapshot.time_sig_changed;
    context.transport_changed = first_range && snapshot.transport_changed;
    context.transport_started = first_range && snapshot.transport_started;
    context.reset_requested = first_range && snapshot.reset_requested;
    context.transport_jump = range.discontinuity;
    return context;
}

} // namespace pulp::format
