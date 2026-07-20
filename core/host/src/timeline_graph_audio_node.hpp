#pragma once

#include <pulp/host/timeline_graph_binding.hpp>

#include "timeline_pdc_schedule.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace pulp::host::detail {

struct TimelineGraphSharedBlockState {
    std::atomic<const playback::PlaybackProgramBlock*> block{nullptr};
    std::atomic<TimelineGraphProcessCode> audio_code{TimelineGraphProcessCode::Ok};
};

CustomNodeType make_timeline_graph_audio_node_type(
    std::string type_id, std::string name, int output_ports,
    const std::shared_ptr<TimelineGraphSharedBlockState>& shared,
    const std::shared_ptr<TimelinePdcAudioTransportSlot>& transport_slot,
    const std::shared_ptr<playback::ArrangementAudioTrackRenderer>& renderer,
    playback::AudioRendererLimits limits);

} // namespace pulp::host::detail
