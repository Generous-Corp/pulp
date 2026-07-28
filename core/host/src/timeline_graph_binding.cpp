#include <pulp/host/timeline_graph_binding.hpp>

#include <pulp/format/playback_context_projection.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include "timeline_graph_binding_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace pulp::host {
using namespace detail::timeline_graph_binding;

namespace {

std::atomic<std::uint64_t> next_binding_instance_id{1};

} // namespace

TimelineGraphPlaybackBinding::TimelineGraphPlaybackBinding(
    SignalGraph& graph, const playback::PlaybackProgramStore& store)
    : graph_(graph), store_(store),
      shared_(std::make_shared<detail::TimelineGraphSharedBlockState>()),
      automation_claim_owner_(detail::make_exact_parameter_ingress_owner()),
      binding_instance_id_(next_binding_instance_id.fetch_add(1, std::memory_order_relaxed)) {
    graph_.acquire_routed_only_execution();
}

TimelineGraphPlaybackBinding::~TimelineGraphPlaybackBinding() {
    remove_all_owned_nodes();
    graph_.release_routed_only_execution();
}

void TimelineGraphPlaybackBinding::remove_all_owned_nodes() noexcept {
    const auto current = state_.live();
    if (!current) return;
    bool publication_prepared = false;
    try {
        state_.prepare_publish();
        publication_prepared = true;
        auto edit = graph_.begin_prepared_topology_edit();
        bool mutated = true;
        for (const auto& edge : current->detached_post_device_bypasses) {
            if (edit->node(edge.source_node) == nullptr ||
                edit->node(edge.dest_node) == nullptr)
                continue;
            const bool already_present = std::any_of(
                edit->connections().begin(), edit->connections().end(),
                [&](const Connection& connection) {
                    return is_plain_audio_edge(connection, edge);
                });
            if (!already_present)
                mutated = edit->connect(edge.source_node, edge.source_port,
                                        edge.dest_node, edge.dest_port) &&
                          mutated;
        }
        for (const auto& track : current->tracks) {
            if (edit->node(track->audio_node) != nullptr)
                mutated = edit->remove_node(track->audio_node) && mutated;
            if (edit->node(track->mixer_node) != nullptr)
                mutated = edit->remove_node(track->mixer_node) && mutated;
            if (edit->node(track->midi_node) != nullptr)
                mutated = edit->remove_node(track->midi_node) && mutated;
        }
        for (const auto& track : current->tracks) {
            mutated = edit->unregister_custom_node_type(
                          custom_type_id(binding_instance_id_, track->id), 1) &&
                      mutated;
            mutated = edit->unregister_custom_node_type(
                          mixer_custom_type_id(binding_instance_id_, track->id), 1) &&
                      mutated;
        }
        const std::span<const NodeId> no_claims;
        mutated = edit->set_exact_parameter_event_nodes(
                      automation_claim_owner_, no_claims,
                      ExactParameterNodeClaimPasskey{})
            && mutated;
        if (mutated &&
            edit->prepare(current->prepared_sample_rate,
                          static_cast<int>(current->prepared_max_block_size)) ==
                SignalGraph::PreparedTopologyEdit::Result::Prepared &&
            edit->routed_execution_ready(
                static_cast<int>(current->prepared_max_block_size)) &&
            edit->commit() ==
                SignalGraph::PreparedTopologyEdit::Result::Committed) {
            state_.publish_prepared({});
            return;
        }
    } catch (...) {
        // Destruction is fail-closed. Custom instances keep the shared renderer
        // and block state alive if an independently-mutated graph makes the
        // best-effort transactional detach stale.
    }
    if (publication_prepared) state_.publish_prepared({});
}

TimelineGraphAdmission TimelineGraphPlaybackBinding::preflight(
    const playback::PlaybackProgram& program,
    std::span<const TimelineTrackGraphRoute> routes,
    const TimelineGraphBindingConfig& config, int maximum_block_size) const {
    detail::TimelineGraphPreparedCandidate candidate(state_.read());
    return build_candidate(program, routes, config,
                           sample_rate_double(program.tempo_map().sample_rate()),
                           maximum_block_size, candidate);
}

TimelineGraphAdmission TimelineGraphPlaybackBinding::prepare(
    const playback::PlaybackProgram& program, std::span<const TimelineTrackGraphRoute> routes,
    const TimelineGraphBindingConfig& config, double sample_rate, int maximum_block_size) {
    return prepare_impl(program, routes, config, sample_rate, maximum_block_size, false);
}

TimelineGraphAdmission TimelineGraphPlaybackBinding::prepare_quiesced(
    const playback::PlaybackProgram& program, std::span<const TimelineTrackGraphRoute> routes,
    const TimelineGraphBindingConfig& config, double sample_rate, int maximum_block_size) {
    return prepare_impl(program, routes, config, sample_rate, maximum_block_size, true);
}

TimelineGraphAdmission TimelineGraphPlaybackBinding::prepare_impl(
    const playback::PlaybackProgram& program, std::span<const TimelineTrackGraphRoute> routes,
    const TimelineGraphBindingConfig& config, double sample_rate, int maximum_block_size,
    bool quiesced) {
    detail::TimelineGraphPreparedCandidate candidate(state_.read());
    const auto admission = build_candidate(program, routes, config, sample_rate,
                                           maximum_block_size, candidate);
    if (!admission)
        return admission;

    auto& previous = candidate.previous;
    auto& next = candidate.next;
    auto& edit = candidate.edit;

    // Reserve publication capacity only after the disposable candidate passes.
    state_.prepare_publish();
    const bool dimensions_changed = previous &&
        (previous->prepared_sample_rate != sample_rate ||
         previous->prepared_max_block_size != static_cast<std::uint32_t>(maximum_block_size));
    const auto prepared = quiesced
        ? edit->prepare_quiesced(sample_rate, maximum_block_size)
        : edit->prepare(sample_rate, maximum_block_size);
    if (prepared == SignalGraph::PreparedTopologyEdit::Result::QuiescedRollbackFailed) {
        state_.publish_prepared({});
        return reject(TimelineGraphAdmissionCode::GraphPrepareFailed,
                      static_cast<std::uint64_t>(prepared));
    }
    if (prepared != SignalGraph::PreparedTopologyEdit::Result::Prepared)
        return reject(TimelineGraphAdmissionCode::GraphPrepareFailed,
                      static_cast<std::uint64_t>(prepared));
    if (!edit->routed_execution_ready(maximum_block_size)) {
        edit.reset();
        if (!graph_.is_prepared())
            state_.publish_prepared({});
        return reject(TimelineGraphAdmissionCode::RoutedPlanRejected);
    }
    if (before_graph_commit_hook_for_test_ != nullptr)
        before_graph_commit_hook_for_test_(before_graph_commit_context_for_test_);
    SignalGraph::PreparedTopologyEdit::Result committed;
    try {
        committed = edit->commit();
    } catch (...) {
        edit.reset();
        if (!graph_.is_prepared())
            state_.publish_prepared({});
        throw;
    }
    if (committed == SignalGraph::PreparedTopologyEdit::Result::QuiescedRollbackFailed) {
        state_.publish_prepared({});
        return reject(TimelineGraphAdmissionCode::GraphPrepareFailed,
                      static_cast<std::uint64_t>(committed));
    }
    if (committed != SignalGraph::PreparedTopologyEdit::Result::Committed)
        return reject(TimelineGraphAdmissionCode::GraphPrepareFailed,
                      static_cast<std::uint64_t>(committed));

    next->graph_snapshot = edit->committed_execution_snapshot();
    if (!next->graph_snapshot) {
        // commit() advanced the graph already. This should be unreachable, but
        // keeping the prior binding would pair its program/renderers with a
        // different live graph, so fail closed if the invariant is ever broken.
        state_.publish_prepared({});
        return reject(TimelineGraphAdmissionCode::GraphPrepareFailed);
    }
    if (quiesced && dimensions_changed) {
        for (const auto& track : next->tracks) {
            track->audio_renderer->reset();
            track->mixer_renderer->reset();
            track->note_renderer->reset();
        }
        for (const auto& track : next->automation_tracks) {
            if (track->renderer) track->renderer->reset();
        }
    }
    if (before_binding_publish_hook_for_test_ != nullptr)
        before_binding_publish_hook_for_test_(before_binding_publish_context_for_test_);

    // The exact graph snapshot and exact program become visible through this one
    // publication. An in-flight block therefore remains wholly old while the
    // next block is wholly new, even if the graph's ordinary live slot advanced.
    state_.publish_prepared(std::shared_ptr<const detail::TimelineGraphBindingState>(
        std::move(next)));
    return {};
}

TimelineGraphAdmission TimelineGraphPlaybackBinding::build_candidate(
    const playback::PlaybackProgram& program,
    std::span<const TimelineTrackGraphRoute> routes,
    const TimelineGraphBindingConfig& config, double sample_rate,
    int maximum_block_size,
    detail::TimelineGraphPreparedCandidate& candidate) const {
    const double program_sample_rate = sample_rate_double(program.tempo_map().sample_rate());
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0 || sample_rate != program_sample_rate)
        return reject(TimelineGraphAdmissionCode::SampleRateMismatch);
    const auto& previous = candidate.previous;
    if (config.audio_channels == 0 ||
        config.maximum_note_events_per_track_per_block == 0 ||
        config.audio_channels > config.audio_limits.max_channels ||
        maximum_block_size <= 0 ||
        static_cast<std::uint64_t>(maximum_block_size) >
            config.audio_limits.max_block_frames ||
        (previous &&
         (config.audio_channels != previous->config.audio_channels ||
          config.audio_limits != previous->config.audio_limits ||
          config.maximum_note_events_per_track_per_block !=
              previous->config.maximum_note_events_per_track_per_block)))
        return reject(TimelineGraphAdmissionCode::InvalidConfiguration);
    if (config.maximum_note_events_per_track_per_block >
        maximum_graph_midi_events_per_block)
        return reject(TimelineGraphAdmissionCode::NoteCapacityExceeded,
                      config.maximum_note_events_per_track_per_block,
                      maximum_graph_midi_events_per_block);
    if (routes.size() != program.tracks().size())
        return reject(TimelineGraphAdmissionCode::MissingTrack, routes.size(),
                      program.tracks().size());

    std::vector<TimelineTrackGraphRoute> ordered(routes.begin(), routes.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.track_id < rhs.track_id; });
    std::vector<NodeId> claimed_device_nodes;
    std::vector<std::vector<detail::TimelineAutomationRouteMetadata>>
        route_metadata(ordered.size());
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        const auto& route = ordered[index];
        if (!route.track_id.valid())
            return reject(TimelineGraphAdmissionCode::MissingTrack, 0, 0,
                          route.track_id);
        if (index != 0 && route.track_id == ordered[index - 1].track_id)
            return reject(TimelineGraphAdmissionCode::DuplicateTrack, 2, 1,
                          route.track_id);
        const auto* program_track = program.find_track(route.track_id);
        if (program_track == nullptr)
            return reject(TimelineGraphAdmissionCode::MissingTrack, 0, 1,
                          route.track_id);
        const auto* destination = graph_.node(route.audio_destination);
        if (destination == nullptr)
            return reject(TimelineGraphAdmissionCode::MissingDestination, 0, 1,
                          route.track_id, route.audio_destination);
        const auto first =
            static_cast<std::uint64_t>(route.audio_destination_first_port);
        const auto last = first + config.audio_channels;
        if (last >
            static_cast<std::uint64_t>(std::max(0, destination->num_input_ports)))
            return reject(TimelineGraphAdmissionCode::DestinationPortRange, last,
                          std::max(0, destination->num_input_ports), route.track_id,
                          route.audio_destination);
        if (route.midi_destination != 0 && graph_.node(route.midi_destination) == nullptr)
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
            const auto* source = graph_.node(route.post_device_audio_source);
            const auto* post_destination = graph_.node(route.post_mixer_audio_destination);
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
            graph_, *program_track, route.device_routes, claimed_device_nodes,
            route_metadata[index]);
        if (!automation_admission)
            return automation_admission;
    }

    auto next = std::make_shared<detail::TimelineGraphBindingState>();
    next->config = config;
    next->program = std::make_shared<const playback::PlaybackProgram>(program);
    next->prepared_sample_rate = program_sample_rate;
    next->prepared_max_block_size = static_cast<std::uint32_t>(maximum_block_size);
    next->automation_claim_owner = automation_claim_owner_;
    next->tracks.reserve(ordered.size());
    next->automation_tracks.reserve(ordered.size());
    next->prepared_track_ids.reserve(ordered.size());
    next->post_device_routed_track_ids.reserve(ordered.size());

    auto edit = graph_.begin_prepared_topology_edit();
    std::vector<DetachedAudioEdge> desired_detached_bypasses;
    for (const auto& route : ordered) {
        if (route.post_device_audio_source == 0)
            continue;
        for (std::uint32_t channel = 0; channel < config.audio_channels;
             ++channel) {
            const DetachedAudioEdge edge{
                route.post_device_audio_source,
                static_cast<PortIndex>(
                    route.post_device_audio_source_first_port + channel),
                route.post_mixer_audio_destination,
                static_cast<PortIndex>(
                    route.post_mixer_audio_destination_first_port + channel),
            };
            if (std::find(desired_detached_bypasses.begin(),
                          desired_detached_bypasses.end(),
                          edge) == desired_detached_bypasses.end())
                desired_detached_bypasses.push_back(edge);
        }
    }
    if (previous) {
        for (const auto& edge : previous->detached_post_device_bypasses) {
            if (std::find(desired_detached_bypasses.begin(),
                          desired_detached_bypasses.end(),
                          edge) != desired_detached_bypasses.end())
                continue;
            if (edit->node(edge.source_node) == nullptr ||
                edit->node(edge.dest_node) == nullptr)
                continue;
            const bool already_present = std::any_of(
                edit->connections().begin(), edit->connections().end(),
                [&](const Connection& connection) {
                    return is_plain_audio_edge(connection, edge);
                });
            if (!already_present &&
                !edit->connect(edge.source_node, edge.source_port,
                               edge.dest_node, edge.dest_port))
                return reject(TimelineGraphAdmissionCode::GraphMutationFailed,
                              0, 0, {}, edge.source_node);
        }
    }
    for (const auto& edge : desired_detached_bypasses) {
        const bool previously_detached =
            previous &&
            std::find(previous->detached_post_device_bypasses.begin(),
                      previous->detached_post_device_bypasses.end(),
                      edge) !=
                previous->detached_post_device_bypasses.end();
        const auto direct = std::find_if(
            edit->connections().begin(), edit->connections().end(),
            [&](const Connection& connection) {
                return is_plain_audio_edge(connection, edge);
            });
        const bool detached_now = direct != edit->connections().end();
        if (detached_now &&
            !edit->disconnect(edge.source_node, edge.source_port,
                              edge.dest_node, edge.dest_port))
            return reject(TimelineGraphAdmissionCode::GraphMutationFailed, 0,
                          0, {}, edge.source_node);
        if (previously_detached || detached_now)
            next->detached_post_device_bypasses.push_back(edge);
    }
    if (previous) {
        for (const auto& track : previous->tracks) {
            if (std::none_of(ordered.begin(), ordered.end(),
                             [&](const auto& route) { return route.track_id == track->id; })) {
                if (!edit->remove_node(track->audio_node) ||
                    !edit->remove_node(track->mixer_node) || !edit->remove_node(track->midi_node) ||
                    !edit->unregister_custom_node_type(
                        custom_type_id(binding_instance_id_, track->id), 1) ||
                    !edit->unregister_custom_node_type(
                        mixer_custom_type_id(binding_instance_id_, track->id), 1)) {
                    return reject(TimelineGraphAdmissionCode::GraphMutationFailed, 0, 0, track->id);
                }
            }
        }
    }

    // Reconcile only edges whose source is binding-owned. Keeping every
    // already-equal edge intact preserves its private connection identity,
    // which lets the prepared transaction carry its PDC ring exactly.
    const auto reconcile_track_connections =
        [&](const std::shared_ptr<detail::TimelineGraphBoundTrack>& track,
            const TimelineTrackGraphRoute& route) -> TimelineGraphAdmission {
        const bool explicit_post = route.post_device_audio_source != 0;
        const auto mixer_destination =
            explicit_post ? route.post_mixer_audio_destination : route.audio_destination;
        const auto mixer_destination_first_port =
            explicit_post ? route.post_mixer_audio_destination_first_port
                          : route.audio_destination_first_port;
        for (const auto& connection : std::vector<Connection>(edit->connections())) {
            bool desired = false;
            if (connection.source_node == track->audio_node && !connection.midi &&
                !connection.feedback && !connection.automation &&
                !connection.audio_rate_modulation && !connection.sidechain &&
                connection.source_port < config.audio_channels) {
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
                desired = explicit_post &&
                          connection.source_node == route.post_device_audio_source &&
                          connection.source_port ==
                              static_cast<PortIndex>(route.post_device_audio_source_first_port +
                                                     connection.dest_port);
            } else if (connection.source_node == track->mixer_node && !connection.midi &&
                       !connection.feedback && !connection.automation &&
                       !connection.audio_rate_modulation && !connection.sidechain &&
                       connection.source_port < config.audio_channels) {
                desired =
                    connection.dest_node == mixer_destination &&
                    connection.dest_port == static_cast<PortIndex>(mixer_destination_first_port +
                                                                   connection.source_port);
            } else if (connection.source_node == track->midi_node && connection.midi) {
                desired = route.midi_destination != 0 &&
                          connection.dest_node == route.midi_destination;
            } else if (connection.source_node != track->audio_node &&
                       connection.dest_node != track->mixer_node &&
                       connection.source_node != track->mixer_node &&
                       connection.source_node != track->midi_node) {
                continue;
            }
            if (!desired &&
                !edit->disconnect(connection.source_node, connection.source_port,
                                  connection.dest_node, connection.dest_port)) {
                return reject(TimelineGraphAdmissionCode::GraphMutationFailed, 0, 0,
                              route.track_id, connection.source_node);
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
            if (!exists &&
                !edit->connect(track->audio_node, source_port, pre_dest_node, pre_dest_port))
                return reject(TimelineGraphAdmissionCode::GraphMutationFailed, channel, 0,
                              route.track_id, track->audio_node);
            if (explicit_post) {
                const auto post_source_port =
                    static_cast<PortIndex>(route.post_device_audio_source_first_port + channel);
                const bool post_input_exists = std::any_of(
                    edit->connections().begin(), edit->connections().end(),
                    [&](const Connection& connection) {
                        return connection.source_node == route.post_device_audio_source &&
                               connection.source_port == post_source_port &&
                               connection.dest_node == track->mixer_node &&
                               connection.dest_port == source_port && !connection.midi &&
                               !connection.feedback && !connection.automation &&
                               !connection.audio_rate_modulation && !connection.sidechain;
                    });
                if (!post_input_exists &&
                    !edit->connect(route.post_device_audio_source, post_source_port,
                                   track->mixer_node, source_port))
                    return reject(TimelineGraphAdmissionCode::GraphMutationFailed, channel, 0,
                                  route.track_id, route.post_device_audio_source);
            }
            const auto final_dest_port =
                static_cast<PortIndex>(mixer_destination_first_port + channel);
            const bool post_output_exists =
                std::any_of(edit->connections().begin(), edit->connections().end(),
                            [&](const Connection& connection) {
                                return connection.source_node == track->mixer_node &&
                                       connection.source_port == source_port &&
                                       connection.dest_node == mixer_destination &&
                                       connection.dest_port == final_dest_port &&
                                       !connection.midi && !connection.feedback &&
                                       !connection.automation &&
                                       !connection.audio_rate_modulation && !connection.sidechain;
                            });
            if (!post_output_exists &&
                !edit->connect(track->mixer_node, source_port, mixer_destination, final_dest_port))
                return reject(TimelineGraphAdmissionCode::GraphMutationFailed, channel, 0,
                              route.track_id, track->mixer_node);
        }
        const bool midi_exists = route.midi_destination != 0 && std::any_of(
            edit->connections().begin(), edit->connections().end(),
            [&](const Connection& connection) {
                return connection.source_node == track->midi_node && connection.midi &&
                       connection.dest_node == route.midi_destination;
            });
        if (route.midi_destination != 0 && !midi_exists &&
            !edit->connect_midi(track->midi_node, route.midi_destination))
            return reject(TimelineGraphAdmissionCode::GraphMutationFailed, 0, 0,
                          route.track_id, track->midi_node);
        return {};
    };

    for (std::size_t route_index = 0; route_index < ordered.size(); ++route_index) {
        const auto& route = ordered[route_index];
        std::shared_ptr<detail::TimelineGraphBoundTrack> track;
        if (previous) {
            const auto found = std::find_if(
                previous->tracks.begin(), previous->tracks.end(),
                [&](const auto& candidate) { return candidate->id == route.track_id; });
            if (found != previous->tracks.end()) track = *found;
        }
        if (!track) {
            const auto type_id = custom_type_id(binding_instance_id_, route.track_id);
            const auto mixer_type_id = mixer_custom_type_id(binding_instance_id_, route.track_id);
            std::weak_ptr<detail::TimelineGraphSharedBlockState> shared = shared_;
            const auto track_id = route.track_id;
            const auto audio_limits = config.audio_limits;
            auto audio_renderer =
                std::make_shared<playback::ArrangementAudioTrackRenderer>(track_id, false);
            auto mixer_renderer = std::make_shared<playback::TrackMixerTrackRenderer>(track_id);
            CustomNodeType type;
            type.type_id = type_id;
            type.version = 1;
            type.num_output_ports = static_cast<int>(config.audio_channels);
            type.default_name = "Timeline audio track " + std::to_string(track_id.value);
            std::weak_ptr<playback::ArrangementAudioTrackRenderer> weak_renderer = audio_renderer;
            type.create = [shared, weak_renderer, audio_limits]() -> void* {
                auto locked_shared = shared.lock();
                auto locked_renderer = weak_renderer.lock();
                if (!locked_shared || !locked_renderer)
                    return nullptr;
                return new AudioNodeInstance(std::move(locked_shared), std::move(locked_renderer),
                                             audio_limits);
            };
            type.destroy = [](void* value) { delete static_cast<AudioNodeInstance*>(value); };
            type.process_instance = [](void* value, audio::BufferView<float>& output,
                                       const audio::BufferView<const float>&, int) {
                static_cast<AudioNodeInstance*>(value)->process(output);
            };
            type.process_instance_transport = [](void* value, audio::BufferView<float>& output,
                                                 const audio::BufferView<const float>&, int,
                                                 const format::ProcessContext&) {
                static_cast<AudioNodeInstance*>(value)->process(output);
            };
            CustomNodeType mixer_type;
            mixer_type.type_id = mixer_type_id;
            mixer_type.version = 1;
            mixer_type.num_input_ports = static_cast<int>(config.audio_channels);
            mixer_type.num_output_ports = static_cast<int>(config.audio_channels);
            mixer_type.default_name = "Timeline track mixer " + std::to_string(track_id.value);
            std::weak_ptr<playback::TrackMixerTrackRenderer> weak_mixer = mixer_renderer;
            mixer_type.create = [shared, weak_mixer, audio_limits]() -> void* {
                auto locked_shared = shared.lock();
                auto locked_renderer = weak_mixer.lock();
                if (!locked_shared || !locked_renderer)
                    return nullptr;
                return new MixerNodeInstance(std::move(locked_shared), std::move(locked_renderer),
                                             audio_limits);
            };
            mixer_type.destroy = [](void* value) { delete static_cast<MixerNodeInstance*>(value); };
            mixer_type.process_instance = [](void* value, audio::BufferView<float>& output,
                                             const audio::BufferView<const float>& input, int) {
                static_cast<MixerNodeInstance*>(value)->process(output, input);
            };
            mixer_type.process_instance_transport = [](void* value,
                                                       audio::BufferView<float>& output,
                                                       const audio::BufferView<const float>& input,
                                                       int, const format::ProcessContext&) {
                static_cast<MixerNodeInstance*>(value)->process(output, input);
            };
            track = std::make_shared<detail::TimelineGraphBoundTrack>();
            track->id = route.track_id;
            track->audio_renderer = std::move(audio_renderer);
            track->mixer_renderer = std::move(mixer_renderer);
            track->note_renderer =
                std::make_unique<playback::ArrangementNoteRenderer>(route.track_id);
            if (!track->note_renderer->prepare(config.maximum_note_events_per_track_per_block) ||
                !edit->register_custom_node_type(std::move(type)) ||
                !edit->register_custom_node_type(std::move(mixer_type))) {
                return reject(TimelineGraphAdmissionCode::GraphMutationFailed, 0, 0,
                              route.track_id);
            }
            track->audio_node = edit->add_custom_node(type_id);
            track->mixer_node = edit->add_custom_node(mixer_type_id);
            track->midi_node = edit->add_midi_input_node("Timeline MIDI track " +
                                                         std::to_string(route.track_id.value));
            if (track->audio_node == 0 || track->mixer_node == 0 || track->midi_node == 0) {
                return reject(TimelineGraphAdmissionCode::GraphMutationFailed, 0, 0,
                              route.track_id);
            }
        }

        std::shared_ptr<detail::TimelineGraphAutomationTrack> previous_automation;
        if (previous) {
            const auto found = std::find_if(
                previous->automation_tracks.begin(),
                previous->automation_tracks.end(),
                [&](const auto& candidate) {
                    return candidate->id == route.track_id;
                });
            if (found != previous->automation_tracks.end())
                previous_automation = *found;
        }
        auto automation = detail::make_timeline_automation_track(
            *program.find_track(route.track_id), route,
            std::move(route_metadata[route_index]), config.automation_limits, false,
            previous_automation);
        if (!automation) return automation.error();

        if (const auto reconciled = reconcile_track_connections(track, route);
            !reconciled)
            return reconciled;
        next->tracks.push_back(std::move(track));
        next->automation_tracks.push_back(std::move(automation).value());
        next->prepared_track_ids.push_back(route.track_id);
        if (route.post_device_audio_source != 0)
            next->post_device_routed_track_ids.push_back(route.track_id);
    }

    for (const auto& connection : edit->connections()) {
        if ((connection.automation || connection.audio_rate_modulation)
            && std::binary_search(claimed_device_nodes.begin(),
                                  claimed_device_nodes.end(),
                                  connection.dest_node)) {
            return reject(TimelineGraphAdmissionCode::DeviceNodeAutomationConflict,
                          1, 0, {}, connection.dest_node);
        }
    }
    if (!edit->set_exact_parameter_event_nodes(automation_claim_owner_,
                                               claimed_device_nodes,
                                               ExactParameterNodeClaimPasskey{})) {
        return reject(TimelineGraphAdmissionCode::DuplicateDeviceNodeOwnership);
    }

    edit->set_canonical_executor_routing_enabled(true);
    const auto candidate_admission =
        admit_candidate(graph_, edit->nodes(), edit->connections());
    if (!candidate_admission)
        return candidate_admission;
    candidate.next = std::move(next);
    candidate.edit = std::move(edit);
    return {};
}

TimelineGraphAdmission
TimelineGraphPlaybackBinding::adopt_program(const playback::PlaybackProgram& program) {
    auto previous = state_.read();
    if (!previous)
        return reject(TimelineGraphAdmissionCode::GraphPrepareFailed);
    if (previous->delivery_poisoned.load(std::memory_order_acquire)) {
        return reject(TimelineGraphAdmissionCode::CleanupRecoveryRequired);
    }
    if (sample_rate_double(program.tempo_map().sample_rate()) != previous->prepared_sample_rate)
        return reject(TimelineGraphAdmissionCode::SampleRateMismatch);
    if (program.tracks().size() != previous->prepared_track_ids.size())
        return reject(TimelineGraphAdmissionCode::MissingTrack, program.tracks().size(),
                      previous->prepared_track_ids.size());
    for (const auto id : previous->prepared_track_ids) {
        const auto* track = program.find_track(id);
        if (track == nullptr)
            return reject(TimelineGraphAdmissionCode::MissingTrack, 0, 1, id);
        if (!track->ordered_device_placement_ids().empty() && !track->mixer().transparent() &&
            !std::binary_search(previous->post_device_routed_track_ids.begin(),
                                previous->post_device_routed_track_ids.end(), id))
            return reject(TimelineGraphAdmissionCode::MissingPostDeviceRoute, 0, 1, id);
    }

    auto next = std::make_shared<detail::TimelineGraphBindingState>();
    next->tracks = previous->tracks;
    next->automation_tracks.reserve(previous->automation_tracks.size());
    std::vector<NodeId> claimed_device_nodes;
    for (const auto id : previous->prepared_track_ids) {
        const auto prior = std::find_if(
            previous->automation_tracks.begin(),
            previous->automation_tracks.end(),
            [&](const auto& candidate) { return candidate->id == id; });
        if (prior == previous->automation_tracks.end())
            return reject(TimelineGraphAdmissionCode::GraphPrepareFailed, 0, 1, id);
        TimelineTrackGraphRoute route;
        route.track_id = id;
        route.device_routes = (*prior)->delivery->mappings();
        const auto* program_track = program.find_track(id);
        const auto automation_admission =
            detail::validate_timeline_automation_routes(
                *program_track, (*prior)->route_metadata,
                claimed_device_nodes);
        if (!automation_admission) return automation_admission;
        const auto limits = (*prior)->renderer
            ? (*prior)->renderer->limits()
            : previous->config.automation_limits;
        auto automation = detail::make_timeline_automation_track(
            *program_track, route, (*prior)->route_metadata, limits, true, *prior);
        if (!automation) return automation.error();
        next->automation_tracks.push_back(std::move(automation).value());
    }
    next->config = previous->config;
    next->prepared_track_ids = previous->prepared_track_ids;
    next->post_device_routed_track_ids = previous->post_device_routed_track_ids;
    next->detached_post_device_bypasses =
        previous->detached_post_device_bypasses;
    next->program = std::make_shared<const playback::PlaybackProgram>(program);
    next->graph_snapshot = previous->graph_snapshot;
    next->prepared_sample_rate = previous->prepared_sample_rate;
    next->prepared_max_block_size = previous->prepared_max_block_size;
    next->automation_claim_owner = previous->automation_claim_owner;
    state_.prepare_publish();
    if (before_binding_publish_hook_for_test_ != nullptr)
        before_binding_publish_hook_for_test_(before_binding_publish_context_for_test_);
    state_.publish_prepared(std::shared_ptr<const detail::TimelineGraphBindingState>(
        std::move(next)));
    return {};
}

TimelineGraphAdmission TimelineGraphPlaybackBinding::adopt_latest_program() {
    auto latest = store_.read();
    if (!latest)
        return reject(TimelineGraphAdmissionCode::GraphPrepareFailed);
    return adopt_program(*latest);
}

TimelineGraphProcessResult
TimelineGraphPlaybackBinding::process(audio::BufferView<float>& output,
                                      const audio::BufferView<const float>& input,
                                      const playback::TransportSnapshot& transport) noexcept {
    runtime::ScopedNoAlloc no_alloc;
    TimelineGraphProcessResult result;
    auto state = state_.read();
    if (!state) {
        result.code = TimelineGraphProcessCode::MissingProgram;
        output.clear();
        return result;
    }
    // Bound every later fail-closed clear before it can touch caller memory.
    // Capacity rejection itself preserves the buffer exactly.
    if (output.empty() || output.num_channels() != state->config.audio_channels ||
        output.num_channels() > state->config.audio_limits.max_channels ||
        output.num_samples() > state->config.audio_limits.max_block_frames ||
        output.num_samples() > state->prepared_max_block_size) {
        result.code = TimelineGraphProcessCode::CapacityExceeded;
        return result;
    }
    if (input.num_channels() != output.num_channels() ||
        input.num_samples() != output.num_samples()) {
        result.code = TimelineGraphProcessCode::InputShapeMismatch;
        return result;
    }
    if (state->delivery_poisoned.load(std::memory_order_acquire)) {
        result.code = TimelineGraphProcessCode::CleanupFailed;
        output.clear();
        return result;
    }
    if (!state->program) {
        result.code = TimelineGraphProcessCode::MissingProgram;
        output.clear();
        return result;
    }
    playback::PlaybackProgramBlock block(state->program.get());
    if (sample_rate_double(block.program()->tempo_map().sample_rate()) !=
        state->prepared_sample_rate) {
        result.code = TimelineGraphProcessCode::InvalidTransport;
        output.clear();
        return result;
    }
    if (sample_rate_double(transport.sample_rate) != state->prepared_sample_rate ||
        transport.tempo_map != &block.program()->tempo_map() ||
        transport.frame_count != output.num_samples() || transport.range_count == 0) {
        result.code = TimelineGraphProcessCode::InvalidTransport;
        output.clear();
        return result;
    }
    if (block.program()->tracks().size() != state->prepared_track_ids.size()) {
        result.code = TimelineGraphProcessCode::TopologyChanged;
        output.clear();
        return result;
    }
    for (const auto id : state->prepared_track_ids) {
        if (block.program()->find_track(id) == nullptr) {
            result.code = TimelineGraphProcessCode::TopologyChanged;
            output.clear();
            return result;
        }
    }
    const auto clear_automation = [&](std::size_t delivered_count) noexcept {
        bool cleared = true;
        for (std::size_t index = 0; index < delivered_count; ++index) {
            cleared = state->automation_tracks[index]->delivery->clear(
                          state->graph_snapshot) &&
                      cleared;
        }
        return cleared;
    };
    // Fail-closed exit shared by every mid-block delivery failure. Publishes the
    // failure code, or CleanupFailed (poisoning the binding) when the
    // compensating clear could not run, then silences the output. The caller
    // computes `cleanup_ok` from its own clears so both always execute.
    const auto fail_closed = [&](TimelineGraphProcessCode failure_code,
                                 bool cleanup_ok) noexcept {
        if (!cleanup_ok) {
            state->delivery_poisoned.store(true, std::memory_order_release);
        }
        result.code =
            cleanup_ok ? failure_code : TimelineGraphProcessCode::CleanupFailed;
        output.clear();
        return result;
    };
    std::size_t delivered_automation_tracks = 0;
    for (const auto& track : state->automation_tracks) {
        if (!track->renderer) {
            ++delivered_automation_tracks;
            continue;
        }
        const auto rendered = track->renderer->process(transport);
        saturating_add(result.candidate_automation_events,
                       rendered.candidate_events);
        saturating_add(result.emitted_automation_events,
                       rendered.emitted_events);
        saturating_add(result.coalesced_automation_events,
                       rendered.candidate_events > rendered.emitted_events
                           ? rendered.candidate_events - rendered.emitted_events
                           : 0);
        if (rendered.code != playback::TrackAutomationRendererCode::Ok &&
            rendered.code != playback::TrackAutomationRendererCode::Coalesced) {
            return fail_closed(TimelineGraphProcessCode::AutomationRenderFailed,
                               clear_automation(delivered_automation_tracks));
        }
        const auto delivered = track->delivery->deliver(
            track->renderer->batches(), transport.frame_count,
            state->graph_snapshot);
        if (!delivered || delivered.injected_events != rendered.emitted_events) {
            return fail_closed(TimelineGraphProcessCode::AutomationDeliveryFailed,
                               clear_automation(delivered_automation_tracks));
        }
        ++delivered_automation_tracks;
    }

    midi::MidiBuffer empty_midi;
    std::size_t injected_note_tracks = 0;
    const auto clear_notes = [&]() noexcept {
        bool cleared = true;
        for (std::size_t index = 0; index < injected_note_tracks; ++index) {
            cleared = state->graph_snapshot.inject_midi(
                          state->tracks[index]->midi_node, empty_midi) &&
                      cleared;
        }
        return cleared;
    };
    for (const auto& track : state->tracks) {
        const auto note_result = track->note_renderer->process(block, transport);
        saturating_add(result.emitted_note_events, note_result.emitted_events);
        saturating_add(result.dropped_note_events, note_result.dropped_events);
        if (note_result.code != playback::NoteRenderCode::Ok) {
            const bool notes_cleared = clear_notes();
            const bool automation_cleared =
                clear_automation(delivered_automation_tracks);
            return fail_closed(TimelineGraphProcessCode::NoteRenderFailed,
                               notes_cleared && automation_cleared);
        }
        if (!state->graph_snapshot.inject_midi(track->midi_node,
                                               track->note_renderer->events())) {
            // Admission bounds every note stream to the graph mailbox's exact
            // capacity, so a failed injection here means the prepared live
            // snapshot (and therefore the admitted routed path) disappeared.
            const bool notes_cleared = clear_notes();
            const bool automation_cleared =
                clear_automation(delivered_automation_tracks);
            return fail_closed(TimelineGraphProcessCode::RoutedDispatchFailed,
                               notes_cleared && automation_cleared);
        }
        ++injected_note_tracks;
    }

    shared_->audio_code.store(TimelineGraphProcessCode::Ok, std::memory_order_relaxed);
    shared_->transport.store(&transport, std::memory_order_release);
    shared_->block.store(&block, std::memory_order_release);
    // Timeline nodes consume the exact multi-range snapshot above. The graph-wide
    // callback context describes the whole callback, including a discontinuity
    // if any constituent range jumps.
    auto context = format::project_process_context(transport, transport.ranges[0]);
    context.num_samples = transport.frame_count;
    context.transport_jump = false;
    for (std::uint8_t index = 0; index < transport.range_count; ++index)
        context.transport_jump = context.transport_jump || transport.ranges[index].discontinuity;
    // SignalGraph::process is a fork/join barrier: its parallel executor's
    // worker-pool run() waits for every participant before returning. Therefore
    // the stack-owned `block` pin and caller-owned exact snapshot remain alive
    // for every custom-node callback, including all parallel workers. Clear the
    // shared pointers only after that barrier has returned.
    const auto routed_failures_before = graph_.routed_only_execution_failures();
    state->graph_snapshot.process(output, input, static_cast<int>(transport.frame_count), context);
    const bool routed_dispatch_failed =
        graph_.routed_only_execution_failures() != routed_failures_before;
    shared_->block.store(nullptr, std::memory_order_release);
    shared_->transport.store(nullptr, std::memory_order_release);
    if (routed_dispatch_failed) {
        const bool notes_cleared = clear_notes();
        const bool automation_cleared =
            clear_automation(delivered_automation_tracks);
        return fail_closed(TimelineGraphProcessCode::RoutedDispatchFailed,
                           notes_cleared && automation_cleared);
    }
    result.code = shared_->audio_code.load(std::memory_order_relaxed);
    if (result.code != TimelineGraphProcessCode::Ok) output.clear();
    return result;
}

NodeId TimelineGraphPlaybackBinding::audio_node_for(timeline::ItemId track_id) const noexcept {
    auto state = state_.read();
    if (!state) return 0;
    const auto found = std::find_if(state->tracks.begin(), state->tracks.end(),
                                    [&](const auto& track) { return track->id == track_id; });
    return found == state->tracks.end() ? 0 : (*found)->audio_node;
}

NodeId TimelineGraphPlaybackBinding::midi_input_node_for(timeline::ItemId track_id) const noexcept {
    auto state = state_.read();
    if (!state) return 0;
    const auto found = std::find_if(state->tracks.begin(), state->tracks.end(),
                                    [&](const auto& track) { return track->id == track_id; });
    return found == state->tracks.end() ? 0 : (*found)->midi_node;
}

playback::RendererProgramKey
TimelineGraphPlaybackBinding::renderer_key_for(timeline::ItemId track_id) const noexcept {
    auto state = state_.read();
    if (!state) return {};
    const auto found = std::find_if(state->tracks.begin(), state->tracks.end(),
                                    [&](const auto& track) { return track->id == track_id; });
    return found == state->tracks.end() ? playback::RendererProgramKey{}
                                  : (*found)->audio_renderer->active_key();
}

playback::RendererCarryState
TimelineGraphPlaybackBinding::renderer_state_for(timeline::ItemId track_id) const noexcept {
    auto state = state_.read();
    if (!state) return {};
    const auto found = std::find_if(state->tracks.begin(), state->tracks.end(),
                                    [&](const auto& track) { return track->id == track_id; });
    return found == state->tracks.end() ? playback::RendererCarryState{}
                                  : (*found)->audio_renderer->state_snapshot();
}

} // namespace pulp::host
