#pragma once

#include <pulp/host/timeline_graph_binding.hpp>
#include <pulp/playback/track_automation_renderer.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::host::detail {

struct TimelineAutomationRouteMetadata {
    TimelineDeviceGraphRoute route;
    std::vector<HostParamInfo> parameters;
};

struct TimelineGraphAutomationTrack {
    timeline::ItemId id;
    std::shared_ptr<const playback::TrackAutomationProgram> program;
    std::unique_ptr<playback::TrackAutomationRenderer> renderer;
    std::unique_ptr<class TimelineAutomationDelivery> delivery;
    std::vector<TimelineAutomationRouteMetadata> route_metadata;
};

runtime::Result<std::shared_ptr<TimelineGraphAutomationTrack>,
                TimelineGraphAdmission>
make_timeline_automation_track(
    const playback::TrackProgram& track,
    const TimelineTrackGraphRoute& route,
    std::vector<TimelineAutomationRouteMetadata> route_metadata,
    playback::AutomationPlaybackLimits automation_limits,
    bool reuse_previous,
    const std::shared_ptr<TimelineGraphAutomationTrack>& previous = {});

TimelineGraphAdmission validate_timeline_automation_routes(
    const SignalGraph& graph, const playback::TrackProgram& track,
    std::span<const TimelineDeviceGraphRoute> routes,
    std::vector<NodeId>& claimed_nodes,
    std::vector<TimelineAutomationRouteMetadata>& metadata);

TimelineGraphAdmission validate_timeline_automation_routes(
    const playback::TrackProgram& track,
    std::span<const TimelineAutomationRouteMetadata> metadata,
    std::vector<NodeId>& claimed_nodes);

enum class TimelineAutomationDeliveryCode : std::uint8_t {
    Ok,
    UnknownDevicePlacement,
    DuplicateDeviceBatch,
    InvalidEvent,
    QueueCapacityExceeded,
    SnapshotInjectionFailed,
    SnapshotCleanupFailed,
};

struct TimelineAutomationDeliveryResult {
    TimelineAutomationDeliveryCode code = TimelineAutomationDeliveryCode::Ok;
    std::uint32_t injected_events = 0;
    timeline::ItemId device_placement_id;
    NodeId plugin_node = 0;

    constexpr explicit operator bool() const noexcept {
        return code == TimelineAutomationDeliveryCode::Ok;
    }
};

class TimelineAutomationDelivery {
  public:
    static std::unique_ptr<TimelineAutomationDelivery>
    create(std::span<const TimelineDeviceGraphRoute> routes);

    TimelineAutomationDeliveryResult deliver(
        std::span<const playback::DeviceAutomationBatch> batches,
        std::uint32_t frame_count,
        const SignalGraph::ExecutionSnapshot& snapshot) noexcept;
    bool clear(const SignalGraph::ExecutionSnapshot& snapshot) noexcept;

    std::span<const TimelineDeviceGraphRoute> mappings() const noexcept {
        return mappings_;
    }
    bool matches(std::span<const TimelineDeviceGraphRoute> routes) const;

  private:
    std::vector<TimelineDeviceGraphRoute> mappings_;
    std::vector<std::unique_ptr<state::ParameterEventQueue>> queues_;
    std::vector<std::size_t> published_indices_;
    std::size_t published_count_ = 0;
};

} // namespace pulp::host::detail
