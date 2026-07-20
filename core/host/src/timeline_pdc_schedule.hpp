#pragma once

#include <pulp/host/timeline_graph_binding.hpp>
#include <pulp/playback/schedule_ahead.hpp>
#include <pulp/playback/track_automation_renderer.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::host::detail {

struct TimelinePdcAudioTransportSlot {
    std::atomic<const playback::TransportSnapshot*> transport{nullptr};
};

struct TimelinePdcDeviceRequirement {
    timeline::ItemId device_placement_id;
    NodeId plugin_node = 0;
};

struct TimelinePdcTrackRequirement {
    timeline::ItemId track_id;
    NodeId audio_source = 0;
    NodeId midi_destination = 0;
    std::shared_ptr<TimelinePdcAudioTransportSlot> audio_slot;
    std::vector<TimelinePdcDeviceRequirement> devices;
    std::vector<timeline::ItemId> automation_device_ids;
};

struct TimelinePdcDeviceSchedule {
    timeline::ItemId device_placement_id;
    std::int64_t lead_samples = 0;
    playback::TransportSnapshot projected;
};

struct TimelinePdcTrackSchedule {
    timeline::ItemId track_id;
    std::int64_t audio_lead_samples = 0;
    std::int64_t midi_lead_samples = 0;
    playback::TransportSnapshot audio_projection;
    playback::TransportSnapshot midi_projection;
    std::shared_ptr<TimelinePdcAudioTransportSlot> audio_slot;
    std::vector<TimelinePdcDeviceSchedule> devices;
    std::vector<timeline::ItemId> automation_device_ids;
    std::vector<playback::DeviceAutomationTransportView> device_views;
};

struct TimelinePdcSchedulePlan {
    std::vector<TimelinePdcTrackSchedule> tracks;
};

runtime::Result<TimelinePdcSchedulePlan, TimelineGraphAdmission>
build_timeline_pdc_schedule(
    const SignalGraph::PreparedTopologyEdit& edit,
    std::span<const TimelinePdcTrackRequirement> requirements);

TimelinePdcSchedulePlan
clone_timeline_pdc_schedule(const TimelinePdcSchedulePlan& source);

timeline::ItemId retarget_timeline_pdc_automation_devices(
    TimelinePdcTrackSchedule& track,
    std::span<const timeline::ItemId> device_ids);

TimelineGraphProcessCode project_timeline_pdc_schedule(
    TimelinePdcSchedulePlan& plan,
    const playback::TransportSnapshot& base) noexcept;

void clear_timeline_pdc_audio_slots(TimelinePdcSchedulePlan& plan) noexcept;

} // namespace pulp::host::detail
