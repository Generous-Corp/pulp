#include "timeline_graph_stretch_policy.hpp"

namespace pulp::host::detail {

std::optional<TimelineGraphProcessCode>
timeline_stretch_failure(playback::RealtimeStretchRenderCode code) noexcept {
    switch (code) {
    case playback::RealtimeStretchRenderCode::NotRequired:
    case playback::RealtimeStretchRenderCode::Rendered:
    case playback::RealtimeStretchRenderCode::GapIdentityChanged:
        return std::nullopt;
    case playback::RealtimeStretchRenderCode::StateRequired:
        return TimelineGraphProcessCode::RealtimeStretchStateRequired;
    case playback::RealtimeStretchRenderCode::StalePublication:
        return TimelineGraphProcessCode::RealtimeStretchStalePublication;
    case playback::RealtimeStretchRenderCode::UnsupportedScrubbing:
        return TimelineGraphProcessCode::RealtimeStretchUnsupportedScrubbing;
    case playback::RealtimeStretchRenderCode::ImpossibleRatio:
        return TimelineGraphProcessCode::RealtimeStretchImpossibleRatio;
    case playback::RealtimeStretchRenderCode::Backpressure:
        return TimelineGraphProcessCode::RealtimeStretchBackpressure;
    case playback::RealtimeStretchRenderCode::Underflow:
        return TimelineGraphProcessCode::RealtimeStretchUnderflow;
    }
    return TimelineGraphProcessCode::AudioRenderFailed;
}

std::optional<TimelineGraphProcessCode> preflight_timeline_stretch(
    playback::RealtimeStretchProgramRuntime& runtime,
    const playback::PlaybackProgram& program, std::span<const timeline::ItemId> track_ids,
    const playback::TransportSnapshot& transport, audio::BufferView<float> output) noexcept {
    if (!transport.is_playing) {
        runtime.reset();
        return std::nullopt;
    }
    for (const auto id : track_ids) {
        const auto* track = program.find_track(id);
        if (track == nullptr)
            return TimelineGraphProcessCode::TopologyChanged;
        if (const auto failure =
                timeline_stretch_failure(runtime.preflight_track(program, *track, transport, output)))
            return failure;
    }
    return std::nullopt;
}

} // namespace pulp::host::detail
