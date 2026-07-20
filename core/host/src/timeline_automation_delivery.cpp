#include "timeline_automation_delivery.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pulp::host::detail {
namespace {

TimelineGraphAdmission route_error(
    TimelineGraphAdmissionCode code, std::uint64_t actual = 0,
    std::uint64_t limit = 0, timeline::ItemId item = {}, NodeId node = 0) noexcept {
    return {code, actual, limit, item, node};
}

std::vector<TimelineDeviceGraphRoute>
canonical_routes(std::span<const TimelineDeviceGraphRoute> routes) {
    std::vector<TimelineDeviceGraphRoute> result(routes.begin(), routes.end());
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.device_placement_id < rhs.device_placement_id;
    });
    return result;
}

} // namespace

TimelineGraphAdmission validate_timeline_automation_routes(
    const SignalGraph& graph, const playback::TrackProgram& track,
    std::span<const TimelineDeviceGraphRoute> routes,
    std::vector<NodeId>& claimed_nodes,
    std::vector<TimelineAutomationRouteMetadata>& metadata) {
    std::vector<timeline::ItemId> expected(track.ordered_device_placement_ids().begin(),
                                           track.ordered_device_placement_ids().end());
    std::sort(expected.begin(), expected.end());
    const auto provided = canonical_routes(routes);
    for (std::size_t index = 0; index < provided.size(); ++index) {
        const auto& device = provided[index];
        if (!device.device_placement_id.valid()) {
            return route_error(TimelineGraphAdmissionCode::MissingDevicePlacement,
                               0, 1, {}, device.plugin_node);
        }
        if (index != 0 && provided[index - 1].device_placement_id ==
                              device.device_placement_id) {
            return route_error(TimelineGraphAdmissionCode::DuplicateDevicePlacement,
                               2, 1, device.device_placement_id,
                               device.plugin_node);
        }
    }
    if (provided.size() < expected.size()) {
        return route_error(TimelineGraphAdmissionCode::MissingDevicePlacement,
                           provided.size(), expected.size(), track.id());
    }
    if (provided.size() > expected.size()) {
        return route_error(TimelineGraphAdmissionCode::UnexpectedDevicePlacement,
                           provided.size(), expected.size(), track.id());
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (provided[index].device_placement_id != expected[index]) {
            const bool missing = provided[index].device_placement_id > expected[index];
            return route_error(
                missing ? TimelineGraphAdmissionCode::MissingDevicePlacement
                        : TimelineGraphAdmissionCode::UnexpectedDevicePlacement,
                0, 1, missing ? expected[index]
                              : provided[index].device_placement_id,
                provided[index].plugin_node);
        }
    }

    for (const auto& device : provided) {
        const auto* node = graph.node(device.plugin_node);
        if (node == nullptr) {
            return route_error(TimelineGraphAdmissionCode::MissingDeviceNode,
                               0, 1, device.device_placement_id,
                               device.plugin_node);
        }
        if (node->type != NodeType::Plugin) {
            return route_error(TimelineGraphAdmissionCode::DeviceNodeNotPlugin,
                               0, 1, device.device_placement_id,
                               device.plugin_node);
        }
        if (!node->plugin || !node->plugin->is_loaded()) {
            return route_error(TimelineGraphAdmissionCode::UnresolvedDevicePlugin,
                               0, 1, device.device_placement_id,
                               device.plugin_node);
        }
        metadata.push_back({device, node->plugin->parameters()});
    }

    return validate_timeline_automation_routes(track, metadata, claimed_nodes);
}

TimelineGraphAdmission validate_timeline_automation_routes(
    const playback::TrackProgram& track,
    std::span<const TimelineAutomationRouteMetadata> metadata,
    std::vector<NodeId>& claimed_nodes) {
    std::vector<timeline::ItemId> expected(
        track.ordered_device_placement_ids().begin(),
        track.ordered_device_placement_ids().end());
    std::sort(expected.begin(), expected.end());
    if (metadata.size() < expected.size()) {
        return route_error(TimelineGraphAdmissionCode::MissingDevicePlacement,
                           metadata.size(), expected.size(), track.id());
    }
    if (metadata.size() > expected.size()) {
        return route_error(TimelineGraphAdmissionCode::UnexpectedDevicePlacement,
                           metadata.size(), expected.size(), track.id());
    }
    for (std::size_t index = 0; index < metadata.size(); ++index) {
        const auto& cached = metadata[index];
        if (cached.route.device_placement_id != expected[index]) {
            const bool missing = cached.route.device_placement_id > expected[index];
            return route_error(
                missing ? TimelineGraphAdmissionCode::MissingDevicePlacement
                        : TimelineGraphAdmissionCode::UnexpectedDevicePlacement,
                0, 1,
                missing ? expected[index] : cached.route.device_placement_id,
                cached.route.plugin_node);
        }
    }

    const auto* automation = track.automation_program();
    if (automation == nullptr) return {};
    std::vector<std::pair<NodeId, timeline::ItemId>> track_claims;
    for (const auto& lane : automation->programs()) {
        const auto target = lane->target();
        const auto mapping = std::lower_bound(
            metadata.begin(), metadata.end(), target.device_placement_id(),
            [](const auto& candidate, timeline::ItemId id) {
                return candidate.route.device_placement_id < id;
            });
        if (mapping == metadata.end()
            || mapping->route.device_placement_id
                != target.device_placement_id()) {
            return route_error(TimelineGraphAdmissionCode::MissingDevicePlacement,
                               0, 1, target.device_placement_id());
        }
        const auto param = std::find_if(
            mapping->parameters.begin(), mapping->parameters.end(),
            [&](const HostParamInfo& info) { return info.id == target.param_id; });
        if (param == mapping->parameters.end()) {
            return route_error(TimelineGraphAdmissionCode::UnknownAutomationParameter,
                               target.param_id, 0, lane->lane_id(),
                               mapping->route.plugin_node);
        }
        if (param->flags.read_only) {
            return route_error(TimelineGraphAdmissionCode::ReadOnlyAutomationParameter,
                               target.param_id, 0, lane->lane_id(),
                               mapping->route.plugin_node);
        }
        if (!param->flags.automatable) {
            return route_error(
                TimelineGraphAdmissionCode::NonAutomatableAutomationParameter,
                target.param_id, 0, lane->lane_id(),
                mapping->route.plugin_node);
        }
        const auto claim = std::lower_bound(
            track_claims.begin(), track_claims.end(), mapping->route.plugin_node,
            [](const auto& candidate, NodeId node) {
                return candidate.first < node;
            });
        if (claim != track_claims.end()
            && claim->first == mapping->route.plugin_node
            && claim->second != mapping->route.device_placement_id) {
            return route_error(
                TimelineGraphAdmissionCode::DuplicateDeviceNodeOwnership,
                2, 1, mapping->route.device_placement_id,
                mapping->route.plugin_node);
        }
        if (claim == track_claims.end()
            || claim->first != mapping->route.plugin_node) {
            track_claims.insert(
                claim, {mapping->route.plugin_node,
                        mapping->route.device_placement_id});
        }
    }
    for (const auto& [node, placement] : track_claims) {
        (void)placement;
        const auto owner = std::lower_bound(
            claimed_nodes.begin(), claimed_nodes.end(), node);
        if (owner != claimed_nodes.end() && *owner == node) {
            return route_error(
                TimelineGraphAdmissionCode::DuplicateDeviceNodeOwnership,
                2, 1, track.id(), node);
        }
        claimed_nodes.insert(owner, node);
    }
    return {};
}

std::unique_ptr<TimelineAutomationDelivery>
TimelineAutomationDelivery::create(std::span<const TimelineDeviceGraphRoute> routes) {
    auto result = std::make_unique<TimelineAutomationDelivery>();
    result->mappings_ = canonical_routes(routes);
    for (std::size_t index = 1; index < result->mappings_.size(); ++index) {
        if (result->mappings_[index - 1].device_placement_id ==
            result->mappings_[index].device_placement_id) {
            return nullptr;
        }
    }
    result->queues_.reserve(result->mappings_.size());
    for (std::size_t index = 0; index < result->mappings_.size(); ++index)
        result->queues_.push_back(std::make_unique<state::ParameterEventQueue>());
    result->published_indices_.resize(result->mappings_.size());
    return result;
}

bool TimelineAutomationDelivery::matches(
    std::span<const TimelineDeviceGraphRoute> routes) const {
    const auto canonical = canonical_routes(routes);
    if (canonical.size() != mappings_.size()) return false;
    for (std::size_t index = 0; index < canonical.size(); ++index) {
        if (canonical[index].device_placement_id !=
                mappings_[index].device_placement_id ||
            canonical[index].plugin_node != mappings_[index].plugin_node) {
            return false;
        }
    }
    return true;
}

runtime::Result<std::shared_ptr<TimelineGraphAutomationTrack>,
                TimelineGraphAdmission>
make_timeline_automation_track(
    const playback::TrackProgram& track,
    const TimelineTrackGraphRoute& route,
    std::vector<TimelineAutomationRouteMetadata> route_metadata,
    playback::AutomationPlaybackLimits automation_limits,
    bool reuse_previous,
    const std::shared_ptr<TimelineGraphAutomationTrack>& previous) {
    auto delivery = TimelineAutomationDelivery::create(route.device_routes);
    if (!delivery) {
        return runtime::Err(TimelineGraphAdmission{
            TimelineGraphAdmissionCode::DuplicateDevicePlacement,
            2, 1, route.track_id});
    }
    const auto& program = track.automation_program_owner();
    if (reuse_previous && previous && previous->program == program
        && previous->delivery->matches(delivery->mappings())
        && previous->route_metadata.size() == route_metadata.size()) {
        return runtime::Ok(previous);
    }
    auto result = std::make_shared<TimelineGraphAutomationTrack>();
    result->id = route.track_id;
    result->program = program;
    result->delivery = std::move(delivery);
    result->route_metadata = std::move(route_metadata);
    if (program) {
        auto renderer = playback::TrackAutomationRenderer::create(
            program, automation_limits);
        if (!renderer) {
            return runtime::Err(TimelineGraphAdmission{
                TimelineGraphAdmissionCode::AutomationRendererRejected,
                static_cast<std::uint64_t>(renderer.error().code), 0,
                renderer.error().lane_id});
        }
        result->renderer = std::make_unique<playback::TrackAutomationRenderer>(
            std::move(renderer).value());
    }
    return runtime::Ok(std::move(result));
}

TimelineAutomationDeliveryResult TimelineAutomationDelivery::deliver(
    std::span<const playback::DeviceAutomationBatch> batches,
    std::uint32_t frame_count,
    const SignalGraph::ExecutionSnapshot& snapshot) noexcept {
    TimelineAutomationDeliveryResult result;
    published_count_ = 0;
    timeline::ItemId previous_device;
    for (const auto& batch : batches) {
        if (previous_device.valid() && batch.device_placement_id <= previous_device) {
            result.code = TimelineAutomationDeliveryCode::DuplicateDeviceBatch;
            result.device_placement_id = batch.device_placement_id;
            return result;
        }
        previous_device = batch.device_placement_id;
        const auto mapping = std::lower_bound(
            mappings_.begin(), mappings_.end(), batch.device_placement_id,
            [](const auto& candidate, timeline::ItemId id) {
                return candidate.device_placement_id < id;
            });
        if (mapping == mappings_.end() ||
            mapping->device_placement_id != batch.device_placement_id) {
            result.code = TimelineAutomationDeliveryCode::UnknownDevicePlacement;
            result.device_placement_id = batch.device_placement_id;
            return result;
        }
        const auto mapping_index = static_cast<std::size_t>(mapping - mappings_.begin());
        auto& queue = *queues_[mapping_index];
        queue.clear();
        for (const auto& event : batch.events) {
            if (event.sample_offset >
                    static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
                event.ramp_duration_sample_frames >
                    static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
                event.sample_offset >= frame_count ||
                event.ramp_duration_sample_frames > frame_count - event.sample_offset ||
                !std::isfinite(event.value)) {
                result.code = TimelineAutomationDeliveryCode::InvalidEvent;
                result.device_placement_id = batch.device_placement_id;
                result.plugin_node = mapping->plugin_node;
                return result;
            }
            const auto delivered_offset = event.ramp_duration_sample_frames == 0
                ? event.sample_offset
                : std::min(event.sample_offset
                               + event.ramp_duration_sample_frames,
                           frame_count - 1);
            if (!queue.push({event.param_id,
                             static_cast<std::int32_t>(delivered_offset),
                             event.value, 0})) {
                result.code = TimelineAutomationDeliveryCode::QueueCapacityExceeded;
                result.device_placement_id = batch.device_placement_id;
                result.plugin_node = mapping->plugin_node;
                return result;
            }
        }
    }

    std::size_t published_count = 0;
    for (const auto& batch : batches) {
        const auto mapping = std::lower_bound(
            mappings_.begin(), mappings_.end(), batch.device_placement_id,
            [](const auto& candidate, timeline::ItemId id) {
                return candidate.device_placement_id < id;
            });
        const auto mapping_index = static_cast<std::size_t>(mapping - mappings_.begin());
        if (!snapshot.inject_parameter_events(mapping->plugin_node,
                                              *queues_[mapping_index])) {
            bool cleanup_ok = true;
            for (std::size_t index = 0; index < published_count; ++index) {
                const auto published = published_indices_[index];
                queues_[published]->clear();
                cleanup_ok = snapshot.inject_parameter_events(
                                 mappings_[published].plugin_node,
                                 *queues_[published])
                    && cleanup_ok;
            }
            result.code = cleanup_ok
                ? TimelineAutomationDeliveryCode::SnapshotInjectionFailed
                : TimelineAutomationDeliveryCode::SnapshotCleanupFailed;
            result.device_placement_id = batch.device_placement_id;
            result.plugin_node = mapping->plugin_node;
            result.injected_events = 0;
            return result;
        }
        published_indices_[published_count++] = mapping_index;
        result.injected_events += static_cast<std::uint32_t>(batch.events.size());
    }
    published_count_ = published_count;
    return result;
}

bool TimelineAutomationDelivery::clear(
    const SignalGraph::ExecutionSnapshot& snapshot) noexcept {
    bool cleared = true;
    for (std::size_t index = 0; index < published_count_; ++index) {
        const auto mapping_index = published_indices_[index];
        queues_[mapping_index]->clear();
        cleared = snapshot.inject_parameter_events(
                      mappings_[mapping_index].plugin_node,
                      *queues_[mapping_index]) &&
                  cleared;
    }
    published_count_ = 0;
    return cleared;
}

} // namespace pulp::host::detail
