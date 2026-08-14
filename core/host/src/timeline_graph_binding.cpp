#include <pulp/host/timeline_graph_binding.hpp>

#include <pulp/format/playback_context_projection.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include "timeline_graph_binding_internal.hpp"
#include "timeline_graph_stretch_policy.hpp"

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
    if (!current)
        return;
    bool publication_prepared = false;
    try {
        state_.prepare_publish();
        publication_prepared = true;
        auto edit = graph_.begin_prepared_topology_edit();
        bool mutated = true;
        for (const auto& edge : current->detached_post_device_bypasses) {
            if (edit->node(edge.source_node) == nullptr || edit->node(edge.dest_node) == nullptr)
                continue;
            const bool already_present =
                std::any_of(edit->connections().begin(), edit->connections().end(),
                            [&](const Connection& connection) {
                                return is_plain_audio_edge(connection, edge);
                            });
            if (!already_present)
                mutated = edit->connect(edge.source_node, edge.source_port, edge.dest_node,
                                        edge.dest_port) &&
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
        mutated = edit->set_exact_parameter_event_nodes(automation_claim_owner_, no_claims,
                                                        ExactParameterNodeClaimPasskey{}) &&
                  mutated;
        if (mutated &&
            edit->prepare(current->prepared_sample_rate,
                          static_cast<int>(current->prepared_max_block_size)) ==
                SignalGraph::PreparedTopologyEdit::Result::Prepared &&
            edit->routed_execution_ready(static_cast<int>(current->prepared_max_block_size)) &&
            edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed) {
            state_.publish_prepared({});
            return;
        }
    } catch (...) {
        // Destruction is fail-closed. Custom instances keep the shared renderer
        // and block state alive if an independently-mutated graph makes the
        // best-effort transactional detach stale.
    }
    if (publication_prepared)
        state_.publish_prepared({});
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
    const auto aggregate_stretch = playback::admit_realtime_stretch_program(
        program, previous->prepared_sample_rate, previous->prepared_max_block_size,
        previous->config.audio_limits);
    if (!aggregate_stretch)
        return reject(TimelineGraphAdmissionCode::RealtimeStretchRejected, aggregate_stretch.actual,
                      aggregate_stretch.limit, aggregate_stretch.clip_id);
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
    next->program = std::make_shared<const playback::PlaybackProgram>(program);
    next->realtime_stretch = std::make_shared<playback::RealtimeStretchProgramRuntime>();
    const auto prepared_stretch = next->realtime_stretch->prepare(
        *next->program, previous->prepared_sample_rate, previous->prepared_max_block_size,
        previous->config.audio_channels, previous->config.audio_limits);
    if (!prepared_stretch)
        return reject(TimelineGraphAdmissionCode::RealtimeStretchRejected, prepared_stretch.actual,
                      prepared_stretch.limit, prepared_stretch.clip_id);
    for (const auto id : previous->prepared_track_ids) {
        if (previous->realtime_stretch->track_uses_realtime_stretch(id) !=
            next->realtime_stretch->track_uses_realtime_stretch(id))
            return reject(TimelineGraphAdmissionCode::GraphPrepareFailed, 0, 1, id);
    }
    if (previous->realtime_stretch->latency_samples() != next->realtime_stretch->latency_samples())
        return reject(TimelineGraphAdmissionCode::GraphPrepareFailed,
                      next->realtime_stretch->latency_samples(),
                      previous->realtime_stretch->latency_samples());
    next->tracks = previous->tracks;
    next->automation_tracks.reserve(previous->automation_tracks.size());
    std::vector<NodeId> claimed_device_nodes;
    for (const auto id : previous->prepared_track_ids) {
        const auto prior =
            std::find_if(previous->automation_tracks.begin(), previous->automation_tracks.end(),
                         [&](const auto& candidate) { return candidate->id == id; });
        if (prior == previous->automation_tracks.end())
            return reject(TimelineGraphAdmissionCode::GraphPrepareFailed, 0, 1, id);
        TimelineTrackGraphRoute route;
        route.track_id = id;
        route.device_routes = (*prior)->delivery->mappings();
        const auto* program_track = program.find_track(id);
        const auto automation_admission = detail::validate_timeline_automation_routes(
            *program_track, (*prior)->route_metadata, claimed_device_nodes);
        if (!automation_admission)
            return automation_admission;
        const auto limits =
            (*prior)->renderer ? (*prior)->renderer->limits() : previous->config.automation_limits;
        auto automation = detail::make_timeline_automation_track(
            *program_track, route, (*prior)->route_metadata, limits, true, *prior);
        if (!automation)
            return automation.error();
        next->automation_tracks.push_back(std::move(automation).value());
    }
    next->config = previous->config;
    next->prepared_track_ids = previous->prepared_track_ids;
    next->post_device_routed_track_ids = previous->post_device_routed_track_ids;
    next->detached_post_device_bypasses = previous->detached_post_device_bypasses;
    next->graph_snapshot = previous->graph_snapshot;
    next->prepared_sample_rate = previous->prepared_sample_rate;
    next->prepared_max_block_size = previous->prepared_max_block_size;
    next->automation_claim_owner = previous->automation_claim_owner;
    state_.prepare_publish();
    if (before_binding_publish_hook_for_test_ != nullptr)
        before_binding_publish_hook_for_test_(before_binding_publish_context_for_test_);
    state_.publish_prepared(
        std::shared_ptr<const detail::TimelineGraphBindingState>(std::move(next)));
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
    // Validate every live-Stretch lane before automation, notes, or any graph
    // worker can mutate callback state. One rejected track rejects the whole
    // program block without leaving sibling side effects behind.
    if (const auto failure =
            detail::preflight_timeline_stretch(*state->realtime_stretch, *block.program(),
                                               state->prepared_track_ids, transport, output)) {
        result.code = *failure;
        output.clear();
        return result;
    }
    const auto clear_automation = [&](std::size_t delivered_count) noexcept {
        bool cleared = true;
        for (std::size_t index = 0; index < delivered_count; ++index) {
            cleared =
                state->automation_tracks[index]->delivery->clear(state->graph_snapshot) && cleared;
        }
        return cleared;
    };
    // Fail-closed exit shared by every mid-block delivery failure. Publishes the
    // failure code, or CleanupFailed (poisoning the binding) when the
    // compensating clear could not run, then silences the output. The caller
    // computes `cleanup_ok` from its own clears so both always execute.
    const auto fail_closed = [&](TimelineGraphProcessCode failure_code, bool cleanup_ok) noexcept {
        if (!cleanup_ok) {
            state->delivery_poisoned.store(true, std::memory_order_release);
        }
        result.code = cleanup_ok ? failure_code : TimelineGraphProcessCode::CleanupFailed;
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
        saturating_add(result.candidate_automation_events, rendered.candidate_events);
        saturating_add(result.emitted_automation_events, rendered.emitted_events);
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
            track->renderer->batches(), transport.frame_count, state->graph_snapshot);
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
            cleared =
                state->graph_snapshot.inject_midi(state->tracks[index]->midi_node, empty_midi) &&
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
            const bool automation_cleared = clear_automation(delivered_automation_tracks);
            return fail_closed(TimelineGraphProcessCode::NoteRenderFailed,
                               notes_cleared && automation_cleared);
        }
        if (!state->graph_snapshot.inject_midi(track->midi_node, track->note_renderer->events())) {
            // Admission bounds every note stream to the graph mailbox's exact
            // capacity, so a failed injection here means the prepared live
            // snapshot (and therefore the admitted routed path) disappeared.
            const bool notes_cleared = clear_notes();
            const bool automation_cleared = clear_automation(delivered_automation_tracks);
            return fail_closed(TimelineGraphProcessCode::RoutedDispatchFailed,
                               notes_cleared && automation_cleared);
        }
        ++injected_note_tracks;
    }

    shared_->audio_code.store(TimelineGraphProcessCode::Ok, std::memory_order_relaxed);
    shared_->transport.store(&transport, std::memory_order_release);
    shared_->realtime_stretch.store(state->realtime_stretch.get(), std::memory_order_release);
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
    shared_->realtime_stretch.store(nullptr, std::memory_order_release);
    if (routed_dispatch_failed) {
        const bool notes_cleared = clear_notes();
        const bool automation_cleared = clear_automation(delivered_automation_tracks);
        return fail_closed(TimelineGraphProcessCode::RoutedDispatchFailed,
                           notes_cleared && automation_cleared);
    }
    result.code = shared_->audio_code.load(std::memory_order_relaxed);
    if (result.code != TimelineGraphProcessCode::Ok &&
        result.code != TimelineGraphProcessCode::RealtimeStretchGap)
        output.clear();
    return result;
}

NodeId TimelineGraphPlaybackBinding::audio_node_for(timeline::ItemId track_id) const noexcept {
    auto state = state_.read();
    if (!state)
        return 0;
    const auto found = std::find_if(state->tracks.begin(), state->tracks.end(),
                                    [&](const auto& track) { return track->id == track_id; });
    return found == state->tracks.end() ? 0 : (*found)->audio_node;
}

NodeId TimelineGraphPlaybackBinding::midi_input_node_for(timeline::ItemId track_id) const noexcept {
    auto state = state_.read();
    if (!state)
        return 0;
    const auto found = std::find_if(state->tracks.begin(), state->tracks.end(),
                                    [&](const auto& track) { return track->id == track_id; });
    return found == state->tracks.end() ? 0 : (*found)->midi_node;
}

playback::RendererProgramKey
TimelineGraphPlaybackBinding::renderer_key_for(timeline::ItemId track_id) const noexcept {
    auto state = state_.read();
    if (!state)
        return {};
    const auto found = std::find_if(state->tracks.begin(), state->tracks.end(),
                                    [&](const auto& track) { return track->id == track_id; });
    return found == state->tracks.end() ? playback::RendererProgramKey{}
                                        : (*found)->audio_renderer->active_key();
}

playback::RendererCarryState
TimelineGraphPlaybackBinding::renderer_state_for(timeline::ItemId track_id) const noexcept {
    auto state = state_.read();
    if (!state)
        return {};
    const auto found = std::find_if(state->tracks.begin(), state->tracks.end(),
                                    [&](const auto& track) { return track->id == track_id; });
    return found == state->tracks.end() ? playback::RendererCarryState{}
                                        : (*found)->audio_renderer->state_snapshot();
}

} // namespace pulp::host
