#include <pulp/host/timeline_offline_graph.hpp>

#include <limits>

namespace pulp::host {

TimelineDeviceFreeGraph build_device_free_timeline_graph(SignalGraph& graph,
                                                         const playback::PlaybackProgram& program,
                                                         std::uint32_t output_channels) {
    TimelineDeviceFreeGraph result;
    if (output_channels == 0 ||
        output_channels > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        result.code = TimelineDeviceFreeGraphCode::InvalidChannelCount;
        return result;
    }

    const auto output_node = graph.add_output_node(static_cast<int>(output_channels),
                                                   "timeline bounce output");
    if (output_node == 0) {
        result.code = TimelineDeviceFreeGraphCode::OutputNodeRejected;
        return result;
    }

    result.routes.reserve(program.tracks().size());
    for (const auto& track : program.tracks()) {
        if (!track)
            continue;
        result.routes.push_back(TimelineTrackGraphRoute{track->id(), output_node, 0, 0});
    }

    result.code = TimelineDeviceFreeGraphCode::Ok;
    result.output_node = output_node;
    return result;
}

} // namespace pulp::host
