#include "timeline_graph_binding_internal.hpp"

#include <algorithm>
#include <vector>

namespace pulp::host::detail::timeline_graph_binding {

TimelineGraphAdmission validate_timeline_graph_routes(
    const SignalGraph& graph, const playback::PlaybackProgram& program,
    std::span<const TimelineTrackGraphRoute> ordered, const TimelineGraphBindingConfig& config,
    std::vector<NodeId>& claimed_device_nodes,
    std::vector<std::vector<TimelineAutomationRouteMetadata>>& route_metadata) {
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        const auto& route = ordered[index];
        if (!route.track_id.valid())
            return reject(TimelineGraphAdmissionCode::MissingTrack, 0, 0, route.track_id);
        if (index != 0 && route.track_id == ordered[index - 1].track_id)
            return reject(TimelineGraphAdmissionCode::DuplicateTrack, 2, 1, route.track_id);
        const auto* program_track = program.find_track(route.track_id);
        if (program_track == nullptr)
            return reject(TimelineGraphAdmissionCode::MissingTrack, 0, 1, route.track_id);
        const auto* destination = graph.node(route.audio_destination);
        if (destination == nullptr)
            return reject(TimelineGraphAdmissionCode::MissingDestination, 0, 1, route.track_id,
                          route.audio_destination);
        const auto first = static_cast<std::uint64_t>(route.audio_destination_first_port);
        const auto last = first + config.audio_channels;
        if (last > static_cast<std::uint64_t>(std::max(0, destination->num_input_ports)))
            return reject(TimelineGraphAdmissionCode::DestinationPortRange, last,
                          std::max(0, destination->num_input_ports), route.track_id,
                          route.audio_destination);
        if (route.midi_destination != 0 && graph.node(route.midi_destination) == nullptr)
            return reject(TimelineGraphAdmissionCode::MissingDestination, 0, 1, route.track_id,
                          route.midi_destination);
        const bool has_post_source = route.post_device_audio_source != 0;
        const bool has_post_destination = route.post_mixer_audio_destination != 0;
        if (has_post_source != has_post_destination)
            return reject(TimelineGraphAdmissionCode::IncompletePostDeviceRoute, 1, 2,
                          route.track_id);
        if (!program_track->ordered_device_placement_ids().empty() &&
            !program_track->mixer().transparent() && !has_post_source)
            return reject(TimelineGraphAdmissionCode::MissingPostDeviceRoute, 0, 1, route.track_id);
        if (has_post_source) {
            const auto* source = graph.node(route.post_device_audio_source);
            const auto* post_destination = graph.node(route.post_mixer_audio_destination);
            if (!source)
                return reject(TimelineGraphAdmissionCode::MissingDestination, 0, 1, route.track_id,
                              route.post_device_audio_source);
            if (!post_destination)
                return reject(TimelineGraphAdmissionCode::MissingDestination, 0, 1, route.track_id,
                              route.post_mixer_audio_destination);
            const auto source_last =
                static_cast<std::uint64_t>(route.post_device_audio_source_first_port) +
                config.audio_channels;
            if (source_last > static_cast<std::uint64_t>(std::max(0, source->num_output_ports)))
                return reject(TimelineGraphAdmissionCode::SourcePortRange, source_last,
                              std::max(0, source->num_output_ports), route.track_id,
                              route.post_device_audio_source);
            const auto destination_last =
                static_cast<std::uint64_t>(route.post_mixer_audio_destination_first_port) +
                config.audio_channels;
            if (destination_last >
                static_cast<std::uint64_t>(std::max(0, post_destination->num_input_ports)))
                return reject(TimelineGraphAdmissionCode::DestinationPortRange, destination_last,
                              std::max(0, post_destination->num_input_ports), route.track_id,
                              route.post_mixer_audio_destination);
        }
        const auto automation_admission = detail::validate_timeline_automation_routes(
            graph, *program_track, route.device_routes, claimed_device_nodes,
            route_metadata[index]);
        if (!automation_admission)
            return automation_admission;
    }

    return {};
}

TimelineGraphAdmission
reconcile_track_connections(const std::unique_ptr<SignalGraph::PreparedTopologyEdit>& edit,
                            const std::shared_ptr<TimelineGraphBoundTrack>& track,
                            const TimelineTrackGraphRoute& route,
                            const TimelineGraphBindingConfig& config) {
    const bool explicit_post = route.post_device_audio_source != 0;
    const auto mixer_destination =
        explicit_post ? route.post_mixer_audio_destination : route.audio_destination;
    const auto mixer_destination_first_port = explicit_post
                                                  ? route.post_mixer_audio_destination_first_port
                                                  : route.audio_destination_first_port;
    for (const auto& connection : std::vector<Connection>(edit->connections())) {
        bool desired = false;
        if (connection.source_node == track->audio_node && !connection.midi &&
            !connection.feedback && !connection.automation && !connection.audio_rate_modulation &&
            !connection.sidechain && connection.source_port < config.audio_channels) {
            desired = connection.dest_node ==
                          (explicit_post ? route.audio_destination : track->mixer_node) &&
                      connection.dest_port ==
                          static_cast<PortIndex>(
                              (explicit_post ? route.audio_destination_first_port : 0) +
                              connection.source_port);
        } else if (connection.dest_node == track->mixer_node && !connection.midi &&
                   !connection.feedback && !connection.automation &&
                   !connection.audio_rate_modulation && !connection.sidechain &&
                   connection.dest_port < config.audio_channels) {
            desired = explicit_post && connection.source_node == route.post_device_audio_source &&
                      connection.source_port ==
                          static_cast<PortIndex>(route.post_device_audio_source_first_port +
                                                 connection.dest_port);
        } else if (connection.source_node == track->mixer_node && !connection.midi &&
                   !connection.feedback && !connection.automation &&
                   !connection.audio_rate_modulation && !connection.sidechain &&
                   connection.source_port < config.audio_channels) {
            desired = connection.dest_node == mixer_destination &&
                      connection.dest_port == static_cast<PortIndex>(mixer_destination_first_port +
                                                                     connection.source_port);
        } else if (connection.source_node == track->midi_node && connection.midi) {
            desired = route.midi_destination != 0 && connection.dest_node == route.midi_destination;
        } else if (connection.source_node != track->audio_node &&
                   connection.dest_node != track->mixer_node &&
                   connection.source_node != track->mixer_node &&
                   connection.source_node != track->midi_node) {
            continue;
        }
        if (!desired && !edit->disconnect(connection.source_node, connection.source_port,
                                          connection.dest_node, connection.dest_port)) {
            return reject(TimelineGraphAdmissionCode::GraphMutationFailed, 0, 0, route.track_id,
                          connection.source_node);
        }
    }
    for (std::uint32_t channel = 0; channel < config.audio_channels; ++channel) {
        const auto source_port = static_cast<PortIndex>(channel);
        const auto pre_dest_node = explicit_post ? route.audio_destination : track->mixer_node;
        const auto pre_dest_port = static_cast<PortIndex>(
            (explicit_post ? route.audio_destination_first_port : 0) + channel);
        const bool exists =
            std::any_of(edit->connections().begin(), edit->connections().end(),
                        [&](const Connection& connection) {
                            return connection.source_node == track->audio_node &&
                                   connection.source_port == source_port &&
                                   connection.dest_node == pre_dest_node &&
                                   connection.dest_port == pre_dest_port && !connection.midi &&
                                   !connection.feedback && !connection.automation &&
                                   !connection.audio_rate_modulation && !connection.sidechain;
                        });
        if (!exists && !edit->connect(track->audio_node, source_port, pre_dest_node, pre_dest_port))
            return reject(TimelineGraphAdmissionCode::GraphMutationFailed, channel, 0,
                          route.track_id, track->audio_node);
        if (explicit_post) {
            const auto post_source_port =
                static_cast<PortIndex>(route.post_device_audio_source_first_port + channel);
            const bool post_input_exists =
                std::any_of(edit->connections().begin(), edit->connections().end(),
                            [&](const Connection& connection) {
                                return connection.source_node == route.post_device_audio_source &&
                                       connection.source_port == post_source_port &&
                                       connection.dest_node == track->mixer_node &&
                                       connection.dest_port == source_port && !connection.midi &&
                                       !connection.feedback && !connection.automation &&
                                       !connection.audio_rate_modulation && !connection.sidechain;
                            });
            if (!post_input_exists &&
                !edit->connect(route.post_device_audio_source, post_source_port, track->mixer_node,
                               source_port))
                return reject(TimelineGraphAdmissionCode::GraphMutationFailed, channel, 0,
                              route.track_id, route.post_device_audio_source);
        }
        const auto final_dest_port = static_cast<PortIndex>(mixer_destination_first_port + channel);
        const bool post_output_exists =
            std::any_of(edit->connections().begin(), edit->connections().end(),
                        [&](const Connection& connection) {
                            return connection.source_node == track->mixer_node &&
                                   connection.source_port == source_port &&
                                   connection.dest_node == mixer_destination &&
                                   connection.dest_port == final_dest_port && !connection.midi &&
                                   !connection.feedback && !connection.automation &&
                                   !connection.audio_rate_modulation && !connection.sidechain;
                        });
        if (!post_output_exists &&
            !edit->connect(track->mixer_node, source_port, mixer_destination, final_dest_port))
            return reject(TimelineGraphAdmissionCode::GraphMutationFailed, channel, 0,
                          route.track_id, track->mixer_node);
    }
    const bool midi_exists = route.midi_destination != 0 &&
                             std::any_of(edit->connections().begin(), edit->connections().end(),
                                         [&](const Connection& connection) {
                                             return connection.source_node == track->midi_node &&
                                                    connection.midi &&
                                                    connection.dest_node == route.midi_destination;
                                         });
    if (route.midi_destination != 0 && !midi_exists &&
        !edit->connect_midi(track->midi_node, route.midi_destination))
        return reject(TimelineGraphAdmissionCode::GraphMutationFailed, 0, 0, route.track_id,
                      track->midi_node);
    return {};
}

} // namespace pulp::host::detail::timeline_graph_binding
