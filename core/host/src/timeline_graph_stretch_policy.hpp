#pragma once

#include <pulp/host/timeline_graph_binding.hpp>
#include <pulp/playback/realtime_stretch_renderer.hpp>

#include <optional>
#include <span>

namespace pulp::host::detail {

std::optional<TimelineGraphProcessCode>
timeline_stretch_failure(playback::RealtimeStretchRenderCode code) noexcept;

/// Program-wide, mutation-free live-Stretch gate. It runs before automation,
/// note rendering, or graph dispatch so one invalid lane suppresses every
/// sibling side effect in the callback.
std::optional<TimelineGraphProcessCode> preflight_timeline_stretch(
    playback::RealtimeStretchProgramRuntime& runtime,
    const playback::PlaybackProgram& program, std::span<const timeline::ItemId> track_ids,
    const playback::TransportSnapshot& transport, audio::BufferView<float> output) noexcept;

} // namespace pulp::host::detail
