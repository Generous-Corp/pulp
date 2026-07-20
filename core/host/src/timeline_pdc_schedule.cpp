#include "timeline_pdc_schedule.hpp"

#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <algorithm>
#include <utility>

namespace pulp::host::detail {
namespace {

TimelineGraphAdmission latency_failure(const LatencyToOutputResult& result,
                                       timeline::ItemId item,
                                       NodeId queried_node) noexcept {
    using Status = LatencyToOutputResult::Status;
    auto code = TimelineGraphAdmissionCode::LatencyNoCompiledSnapshot;
    switch (result.status) {
    case Status::Available:
        break;
    case Status::Unsupported:
        code = TimelineGraphAdmissionCode::LatencyUnsupported;
        break;
    case Status::QueryFailed:
        code = TimelineGraphAdmissionCode::LatencyQueryFailed;
        break;
    case Status::NoOutputPath:
        code = TimelineGraphAdmissionCode::LatencyNoOutputPath;
        break;
    case Status::AmbiguousOutputLatency:
        code = TimelineGraphAdmissionCode::LatencyAmbiguousOutput;
        break;
    case Status::UnknownNode:
        code = TimelineGraphAdmissionCode::LatencyUnknownNode;
        break;
    case Status::NoCompiledSnapshot:
        code = TimelineGraphAdmissionCode::LatencyNoCompiledSnapshot;
        break;
    }
    return {code, 0, 0, item,
            result.offending_node != 0 ? result.offending_node : queried_node};
}

runtime::Result<std::int64_t, TimelineGraphAdmission>
query_lead(const SignalGraph::PreparedTopologyEdit& edit, NodeId node,
           NodeLatencyBoundary boundary, timeline::ItemId item) {
    const auto result = edit.prepared_latency_to_output(node, boundary);
    if (result.status != LatencyToOutputResult::Status::Available)
        return runtime::Err(latency_failure(result, item, node));
    return runtime::Ok(result.samples);
}

void rebuild_views(TimelinePdcTrackSchedule& track) {
    track.device_views.clear();
    track.device_views.reserve(track.automation_device_ids.size());
    for (const auto device_id : track.automation_device_ids) {
        const auto device = std::lower_bound(
            track.devices.begin(), track.devices.end(), device_id,
            [](const TimelinePdcDeviceSchedule& candidate,
               timeline::ItemId id) {
                return candidate.device_placement_id < id;
            });
        if (device == track.devices.end() ||
            device->device_placement_id != device_id) {
            continue;
        }
        track.device_views.push_back(
            {device->device_placement_id, &device->projected});
    }
}

} // namespace

timeline::ItemId retarget_timeline_pdc_automation_devices(
    TimelinePdcTrackSchedule& track,
    std::span<const timeline::ItemId> device_ids) {
    track.automation_device_ids.assign(device_ids.begin(), device_ids.end());
    std::sort(track.automation_device_ids.begin(),
              track.automation_device_ids.end());
    track.automation_device_ids.erase(
        std::unique(track.automation_device_ids.begin(),
                    track.automation_device_ids.end()),
        track.automation_device_ids.end());
    for (const auto device_id : track.automation_device_ids) {
        const auto found = std::lower_bound(
            track.devices.begin(), track.devices.end(), device_id,
            [](const TimelinePdcDeviceSchedule& candidate,
               timeline::ItemId id) {
                return candidate.device_placement_id < id;
            });
        if (found == track.devices.end() ||
            found->device_placement_id != device_id) {
            track.device_views.clear();
            return device_id;
        }
    }
    rebuild_views(track);
    return {};
}

runtime::Result<TimelinePdcSchedulePlan, TimelineGraphAdmission>
build_timeline_pdc_schedule(
    const SignalGraph::PreparedTopologyEdit& edit,
    std::span<const TimelinePdcTrackRequirement> requirements) {
    TimelinePdcSchedulePlan plan;
    plan.tracks.reserve(requirements.size());
    for (const auto& requirement : requirements) {
        TimelinePdcTrackSchedule track;
        track.track_id = requirement.track_id;
        track.audio_slot = requirement.audio_slot;
        auto audio = query_lead(edit, requirement.audio_source,
                                NodeLatencyBoundary::Output,
                                requirement.track_id);
        if (!audio) return runtime::Err(audio.error());
        track.audio_lead_samples = audio.value();
        if (requirement.midi_destination != 0) {
            auto midi = query_lead(edit, requirement.midi_destination,
                                   NodeLatencyBoundary::Input,
                                   requirement.track_id);
            if (!midi) return runtime::Err(midi.error());
            track.midi_lead_samples = midi.value();
        }
        track.devices.reserve(requirement.devices.size());
        for (const auto& device : requirement.devices) {
            auto lead = query_lead(edit, device.plugin_node,
                                   NodeLatencyBoundary::Input,
                                   device.device_placement_id);
            if (!lead) return runtime::Err(lead.error());
            track.devices.push_back(
                {device.device_placement_id, lead.value(), {}});
        }
        const auto missing_device = retarget_timeline_pdc_automation_devices(
            track, requirement.automation_device_ids);
        if (missing_device.valid()) {
            return runtime::Err(TimelineGraphAdmission{
                TimelineGraphAdmissionCode::MissingDevicePlacement,
                0, 1, missing_device});
        }
        plan.tracks.push_back(std::move(track));
    }
    return runtime::Ok(std::move(plan));
}

TimelinePdcSchedulePlan
clone_timeline_pdc_schedule(const TimelinePdcSchedulePlan& source) {
    TimelinePdcSchedulePlan clone;
    clone.tracks.reserve(source.tracks.size());
    for (const auto& source_track : source.tracks) {
        TimelinePdcTrackSchedule track;
        track.track_id = source_track.track_id;
        track.audio_lead_samples = source_track.audio_lead_samples;
        track.midi_lead_samples = source_track.midi_lead_samples;
        track.audio_slot = source_track.audio_slot;
        track.automation_device_ids = source_track.automation_device_ids;
        track.devices.reserve(source_track.devices.size());
        for (const auto& source_device : source_track.devices) {
            track.devices.push_back({source_device.device_placement_id,
                                     source_device.lead_samples, {}});
        }
        rebuild_views(track);
        clone.tracks.push_back(std::move(track));
    }
    return clone;
}

TimelineGraphProcessCode project_timeline_pdc_schedule(
    TimelinePdcSchedulePlan& plan,
    const playback::TransportSnapshot& base) noexcept {
    for (auto& track : plan.tracks) {
        if (playback::project_schedule_ahead(
                base, track.audio_lead_samples,
                track.audio_projection) != playback::ScheduleAheadCode::Ok ||
            playback::project_schedule_ahead(
                base, track.midi_lead_samples,
                track.midi_projection) != playback::ScheduleAheadCode::Ok) {
            clear_timeline_pdc_audio_slots(plan);
            return TimelineGraphProcessCode::ScheduleProjectionFailed;
        }
        for (auto& device : track.devices) {
            if (playback::project_schedule_ahead(
                    base, device.lead_samples,
                    device.projected) != playback::ScheduleAheadCode::Ok) {
                clear_timeline_pdc_audio_slots(plan);
                return TimelineGraphProcessCode::ScheduleProjectionFailed;
            }
        }
        track.audio_slot->transport.store(&track.audio_projection,
                                          std::memory_order_release);
    }
    return TimelineGraphProcessCode::Ok;
}

void clear_timeline_pdc_audio_slots(TimelinePdcSchedulePlan& plan) noexcept {
    for (auto& track : plan.tracks) {
        track.audio_slot->transport.store(nullptr, std::memory_order_release);
    }
}

} // namespace pulp::host::detail
