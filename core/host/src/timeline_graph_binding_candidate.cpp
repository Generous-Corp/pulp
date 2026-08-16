#include <pulp/host/timeline_graph_binding.hpp>

#include <pulp/format/playback_context_projection.hpp>

#include "timeline_graph_binding_internal.hpp"
#include "timeline_graph_stretch_policy.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace pulp::host {
using namespace detail::timeline_graph_binding;

NodeId TimelineGraphPlaybackBinding::device_node_for(timeline::ItemId placement_id) const noexcept {
    auto state = state_.read();
    if (!state)
        return 0;
    const auto found =
        std::find_if(state->owned_devices.begin(), state->owned_devices.end(),
                     [&](const auto& device) { return device.declaration.id == placement_id; });
    return found == state->owned_devices.end() ? 0 : found->plugin_node;
}

namespace detail::timeline_graph_binding {

TimelineGraphAdmission validate_owned_timeline_devices(
    const playback::PlaybackProgram& program,
    std::span<const TimelineGraphBoundDevice> devices) noexcept {
    for (const auto& device : devices) {
        TimelineGraphAdmission project_error;
        const auto* track = timeline_project_track_for(program, device.track_id, project_error);
        if (!track)
            return project_error;
        const auto found = std::find_if(track->device_chain().begin(), track->device_chain().end(),
                                        [&](const auto& placement) {
                                            return placement.id == device.declaration.id;
                                        });
        if (found == track->device_chain().end() || *found != device.declaration)
            return reject(TimelineGraphAdmissionCode::UnsupportedDeviceChain, 0, 1,
                          device.declaration.id, device.plugin_node);
    }
    return {};
}

} // namespace detail::timeline_graph_binding

TimelineGraphAdmission TimelineGraphPlaybackBinding::preflight(
    const playback::PlaybackProgram& program, std::span<const TimelineTrackGraphRoute> routes,
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
    const auto admission =
        build_candidate(program, routes, config, sample_rate, maximum_block_size, candidate);
    if (!admission)
        return admission;

    auto& previous = candidate.previous;
    auto& next = candidate.next;
    auto& edit = candidate.edit;

    // Reserve publication capacity only after the disposable candidate passes.
    state_.prepare_publish();
    const bool dimensions_changed =
        previous &&
        (previous->prepared_sample_rate != sample_rate ||
         previous->prepared_max_block_size != static_cast<std::uint32_t>(maximum_block_size));
    const auto prepared = quiesced ? edit->prepare_quiesced(sample_rate, maximum_block_size)
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
            if (track->renderer)
                track->renderer->reset();
        }
    }
    if (before_binding_publish_hook_for_test_ != nullptr)
        before_binding_publish_hook_for_test_(before_binding_publish_context_for_test_);

    // The exact graph snapshot and exact program become visible through this one
    // publication. An in-flight block therefore remains wholly old while the
    // next block is wholly new, even if the graph's ordinary live slot advanced.
    state_.publish_prepared(
        std::shared_ptr<const detail::TimelineGraphBindingState>(std::move(next)));
    return {};
}

TimelineGraphAdmission TimelineGraphPlaybackBinding::build_candidate(
    const playback::PlaybackProgram& program, std::span<const TimelineTrackGraphRoute> routes,
    const TimelineGraphBindingConfig& config, double sample_rate, int maximum_block_size,
    detail::TimelineGraphPreparedCandidate& candidate) const {
    const double program_sample_rate = sample_rate_double(program.tempo_map().sample_rate());
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0 || sample_rate != program_sample_rate)
        return reject(TimelineGraphAdmissionCode::SampleRateMismatch);
    const auto& previous = candidate.previous;
    if (config.audio_channels == 0 || config.maximum_note_events_per_track_per_block == 0 ||
        config.audio_channels > config.audio_limits.max_channels || maximum_block_size <= 0 ||
        static_cast<std::uint64_t>(maximum_block_size) > config.audio_limits.max_block_frames ||
        (previous && (config.audio_channels != previous->config.audio_channels ||
                      config.audio_limits != previous->config.audio_limits ||
                      config.maximum_note_events_per_track_per_block !=
                          previous->config.maximum_note_events_per_track_per_block)))
        return reject(TimelineGraphAdmissionCode::InvalidConfiguration);
    if (config.maximum_note_events_per_track_per_block > maximum_graph_midi_events_per_block)
        return reject(TimelineGraphAdmissionCode::NoteCapacityExceeded,
                      config.maximum_note_events_per_track_per_block,
                      maximum_graph_midi_events_per_block);
    if (routes.size() != program.tracks().size())
        return reject(TimelineGraphAdmissionCode::MissingTrack, routes.size(),
                      program.tracks().size());
    const auto aggregate_stretch = playback::admit_realtime_stretch_program(
        program, sample_rate, static_cast<std::uint32_t>(maximum_block_size), config.audio_limits);
    if (!aggregate_stretch)
        return reject(TimelineGraphAdmissionCode::RealtimeStretchRejected, aggregate_stretch.actual,
                      aggregate_stretch.limit, aggregate_stretch.clip_id);

    std::vector<TimelineTrackGraphRoute> ordered(routes.begin(), routes.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.track_id < rhs.track_id; });
    std::vector<NodeId> claimed_device_nodes;
    std::vector<std::vector<detail::TimelineAutomationRouteMetadata>> route_metadata(
        ordered.size());
    if (const auto route_admission = validate_timeline_graph_routes(
            graph_, program, ordered, config, claimed_device_nodes, route_metadata);
        !route_admission)
        return route_admission;

    auto next = std::make_shared<detail::TimelineGraphBindingState>();
    next->config = config;
    next->program = std::make_shared<const playback::PlaybackProgram>(program);
    next->realtime_stretch = std::make_shared<playback::RealtimeStretchProgramRuntime>();
    const auto prepared_stretch = next->realtime_stretch->prepare(
        *next->program, sample_rate, static_cast<std::uint32_t>(maximum_block_size),
        config.audio_channels, config.audio_limits);
    if (!prepared_stretch)
        return reject(TimelineGraphAdmissionCode::RealtimeStretchRejected, prepared_stretch.actual,
                      prepared_stretch.limit, prepared_stretch.clip_id);
    next->prepared_sample_rate = program_sample_rate;
    next->prepared_max_block_size = static_cast<std::uint32_t>(maximum_block_size);
    next->automation_claim_owner = automation_claim_owner_;
    next->tracks.reserve(ordered.size());
    next->automation_tracks.reserve(ordered.size());
    next->prepared_track_ids.reserve(ordered.size());
    next->post_device_routed_track_ids.reserve(ordered.size());
    next->owned_devices.reserve(ordered.size());

    auto edit = graph_.begin_prepared_topology_edit();
    std::vector<std::vector<TimelineDeviceGraphRoute>> generated_device_routes(ordered.size());
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

    for (std::size_t route_index = 0; route_index < ordered.size(); ++route_index) {
        auto& route = ordered[route_index];
        std::shared_ptr<detail::TimelineGraphBoundTrack> track;
        if (previous) {
            const auto found = std::find_if(
                previous->tracks.begin(), previous->tracks.end(),
                [&](const auto& candidate) { return candidate->id == route.track_id; });
            if (found != previous->tracks.end())
                track = *found;
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
            const auto stretch_latency =
                next->realtime_stretch->track_uses_realtime_stretch(route.track_id)
                    ? next->realtime_stretch->latency_samples()
                    : 0u;
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
            type.latency_samples = [stretch_latency](double) {
                return static_cast<int>(stretch_latency);
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

        if (const auto resolved = resolve_timeline_device_route(
                program, route, track, edit, previous.get(), timeline_device_factory_,
                generated_device_routes[route_index], route_metadata[route_index],
                claimed_device_nodes, next->owned_devices);
            !resolved)
            return resolved;

        std::shared_ptr<detail::TimelineGraphAutomationTrack> previous_automation;
        if (previous) {
            const auto found = std::find_if(
                previous->automation_tracks.begin(), previous->automation_tracks.end(),
                [&](const auto& candidate) { return candidate->id == route.track_id; });
            if (found != previous->automation_tracks.end())
                previous_automation = *found;
        }
        auto automation = detail::make_timeline_automation_track(
            *program.find_track(route.track_id), route, std::move(route_metadata[route_index]),
            config.automation_limits, false, previous_automation);
        if (!automation)
            return automation.error();

        if (const auto reconciled = reconcile_track_connections(edit, track, route, config);
            !reconciled)
            return reconciled;
        next->tracks.push_back(std::move(track));
        next->automation_tracks.push_back(std::move(automation).value());
        next->prepared_track_ids.push_back(route.track_id);
        if (route.post_device_audio_source != 0)
            next->post_device_routed_track_ids.push_back(route.track_id);
    }

    if (const auto removed =
            remove_stale_timeline_devices(edit, previous.get(), next->owned_devices);
        !removed)
        return removed;
    if (const auto bypasses =
            reconcile_detached_post_device_bypasses(edit, previous.get(), ordered, *next);
        !bypasses)
        return bypasses;

    for (const auto& connection : edit->connections()) {
        if ((connection.automation || connection.audio_rate_modulation) &&
            std::binary_search(claimed_device_nodes.begin(), claimed_device_nodes.end(),
                               connection.dest_node)) {
            return reject(TimelineGraphAdmissionCode::DeviceNodeAutomationConflict, 1, 0, {},
                          connection.dest_node);
        }
    }
    if (!edit->set_exact_parameter_event_nodes(automation_claim_owner_, claimed_device_nodes,
                                               ExactParameterNodeClaimPasskey{})) {
        return reject(TimelineGraphAdmissionCode::DuplicateDeviceNodeOwnership);
    }

    edit->set_canonical_executor_routing_enabled(true);
    const auto candidate_admission = admit_candidate(graph_, edit->nodes(), edit->connections());
    if (!candidate_admission)
        return candidate_admission;
    candidate.next = std::move(next);
    candidate.edit = std::move(edit);
    return {};
}

} // namespace pulp::host
