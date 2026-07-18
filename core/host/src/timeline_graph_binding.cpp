#include <pulp/host/timeline_graph_binding.hpp>

#include <pulp/format/playback_context_projection.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace pulp::host {
namespace {

TimelineGraphAdmission reject(TimelineGraphAdmissionCode code, std::uint64_t actual = 0,
                              std::uint64_t limit = 0, timeline::ItemId item = {},
                              NodeId node = 0) noexcept {
    return {code, actual, limit, item, node};
}

bool owns_node(std::span<const std::unique_ptr<detail::TimelineGraphBoundTrack>> tracks,
               NodeId id) noexcept;

graph::GraphRuntimeNodeKind runtime_kind(NodeType type) noexcept {
    switch (type) {
    case NodeType::AudioInput:
        return graph::GraphRuntimeNodeKind::AudioInput;
    case NodeType::AudioOutput:
        return graph::GraphRuntimeNodeKind::AudioOutput;
    case NodeType::MidiInput:
        return graph::GraphRuntimeNodeKind::MidiInput;
    case NodeType::MidiOutput:
        return graph::GraphRuntimeNodeKind::MidiOutput;
    case NodeType::Custom:
        return graph::GraphRuntimeNodeKind::Custom;
    case NodeType::Gain:
        return graph::GraphRuntimeNodeKind::Utility;
    case NodeType::Plugin:
        return graph::GraphRuntimeNodeKind::Processor;
    }
    return graph::GraphRuntimeNodeKind::Processor;
}

bool checked_add(std::uint64_t& value, std::uint64_t add) noexcept {
    if (add > std::numeric_limits<std::uint64_t>::max() - value)
        return false;
    value += add;
    return true;
}

std::atomic<std::uint64_t> next_binding_instance_id{1};

std::string custom_type_id(std::uint64_t binding_instance_id, timeline::ItemId id) {
    return "pulp.timeline.arrangement-audio-track." + std::to_string(binding_instance_id) + "." +
           std::to_string(id.value);
}

double sample_rate_double(timebase::RationalRate rate) noexcept {
    return static_cast<double>(rate.as_long_double());
}

} // namespace

struct detail::TimelineGraphSharedBlockState {
    std::atomic<const playback::PlaybackProgramBlock*> block{nullptr};
    std::atomic<const playback::TransportSnapshot*> transport{nullptr};
    std::atomic<TimelineGraphProcessCode> audio_code{TimelineGraphProcessCode::Ok};
};

namespace {

struct AudioNodeInstance {
    AudioNodeInstance(std::shared_ptr<detail::TimelineGraphSharedBlockState> shared,
                      std::shared_ptr<playback::ArrangementAudioTrackRenderer> renderer,
                      playback::AudioRendererLimits limits) noexcept
        : shared(std::move(shared)), renderer(std::move(renderer)), limits(limits) {}

    void process(audio::BufferView<float>& output) noexcept {
        // Every track node owns its renderer/shell, so nodes dispatched in one
        // parallel level never touch shared mutable renderer state. The only
        // shared write is the atomic block-wide failure code below.
        const auto* block = shared->block.load(std::memory_order_acquire);
        const auto* transport = shared->transport.load(std::memory_order_acquire);
        if (block == nullptr || transport == nullptr) {
            output.clear();
            shared->audio_code.store(TimelineGraphProcessCode::MissingProgram,
                                     std::memory_order_relaxed);
            return;
        }
        const auto status = renderer->process(*block, *transport, output, limits);
        if (status != playback::AudioRenderStatus::Rendered &&
            status != playback::AudioRenderStatus::Silent) {
            shared->audio_code.store(TimelineGraphProcessCode::AudioRenderFailed,
                                     std::memory_order_relaxed);
        }
    }

    std::shared_ptr<detail::TimelineGraphSharedBlockState> shared;
    std::shared_ptr<playback::ArrangementAudioTrackRenderer> renderer;
    playback::AudioRendererLimits limits;
};

} // namespace

struct detail::TimelineGraphBoundTrack {
    timeline::ItemId id;
    NodeId audio_node = 0;
    NodeId midi_node = 0;
    TimelineTrackGraphRoute route;
    std::shared_ptr<playback::ArrangementAudioTrackRenderer> audio_renderer;
    std::unique_ptr<playback::ArrangementNoteRenderer> note_renderer;
};

namespace {

bool owns_node(std::span<const std::unique_ptr<detail::TimelineGraphBoundTrack>> tracks,
               NodeId id) noexcept {
    return std::any_of(tracks.begin(), tracks.end(), [id](const auto& track) {
        return track->audio_node == id || track->midi_node == id;
    });
}

} // namespace

TimelineGraphPlaybackBinding::TimelineGraphPlaybackBinding(
    SignalGraph& graph, const playback::PlaybackProgramStore& store)
    : graph_(graph), store_(store),
      shared_(std::make_shared<detail::TimelineGraphSharedBlockState>()),
      binding_instance_id_(next_binding_instance_id.fetch_add(1, std::memory_order_relaxed)) {
    graph_.acquire_routed_only_execution();
}

TimelineGraphPlaybackBinding::~TimelineGraphPlaybackBinding() {
    remove_all_owned_nodes();
    graph_.release_routed_only_execution();
}

void TimelineGraphPlaybackBinding::remove_all_owned_nodes() noexcept {
    for (const auto& track : tracks_) {
        if (track->audio_node != 0)
            (void)graph_.remove_node(track->audio_node);
        if (track->midi_node != 0)
            (void)graph_.remove_node(track->midi_node);
    }
    tracks_.clear();
    prepared_track_ids_.clear();
    prepared_sample_rate_ = 0.0;
    prepared_max_block_size_ = 0;
    prepared_ = false;
}

TimelineGraphAdmission TimelineGraphPlaybackBinding::preflight(
    const playback::PlaybackProgram& program, std::span<const TimelineTrackGraphRoute> routes,
    const TimelineGraphBindingConfig& config, int maximum_block_size) const {
    if (config.audio_channels == 0 || config.maximum_note_events_per_track_per_block == 0 ||
        config.audio_channels > config.audio_limits.max_channels || maximum_block_size <= 0 ||
        static_cast<std::uint64_t>(maximum_block_size) > config.audio_limits.max_block_frames ||
        (prepared_ && (config.audio_channels != config_.audio_channels ||
                       config.audio_limits != config_.audio_limits ||
                       config.maximum_note_events_per_track_per_block !=
                           config_.maximum_note_events_per_track_per_block)))
        return reject(TimelineGraphAdmissionCode::InvalidConfiguration);
    if (config.maximum_note_events_per_track_per_block > maximum_graph_midi_events_per_block)
        return reject(TimelineGraphAdmissionCode::NoteCapacityExceeded,
                      config.maximum_note_events_per_track_per_block,
                      maximum_graph_midi_events_per_block);
    if (routes.size() != program.tracks().size())
        return reject(TimelineGraphAdmissionCode::MissingTrack, routes.size(),
                      program.tracks().size());

    std::vector<TimelineTrackGraphRoute> ordered(routes.begin(), routes.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.track_id < rhs.track_id; });
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        const auto& route = ordered[index];
        if (!route.track_id.valid())
            return reject(TimelineGraphAdmissionCode::MissingTrack, 0, 0, route.track_id);
        if (index != 0 && route.track_id == ordered[index - 1].track_id)
            return reject(TimelineGraphAdmissionCode::DuplicateTrack, 2, 1, route.track_id);
        if (program.find_track(route.track_id) == nullptr)
            return reject(TimelineGraphAdmissionCode::MissingTrack, 0, 1, route.track_id);
        const auto* destination = graph_.node(route.audio_destination);
        if (destination == nullptr)
            return reject(TimelineGraphAdmissionCode::MissingDestination, 0, 1, route.track_id,
                          route.audio_destination);
        const auto first = static_cast<std::uint64_t>(route.audio_destination_first_port);
        const auto last = first + config.audio_channels;
        if (last > static_cast<std::uint64_t>(std::max(0, destination->num_input_ports)))
            return reject(TimelineGraphAdmissionCode::DestinationPortRange, last,
                          std::max(0, destination->num_input_ports), route.track_id,
                          route.audio_destination);
        if (route.midi_destination != 0 && graph_.node(route.midi_destination) == nullptr)
            return reject(TimelineGraphAdmissionCode::MissingDestination, 0, 1, route.track_id,
                          route.midi_destination);
    }

    std::vector<GraphNode> nodes;
    nodes.reserve(graph_.nodes().size() + routes.size() * 2u);
    for (const auto& node : graph_.nodes())
        if (!owns_node(tracks_, node.id))
            nodes.push_back(node);
    std::vector<Connection> connections;
    connections.reserve(graph_.connections().size() + routes.size() * (config.audio_channels + 1u));
    for (const auto& connection : graph_.connections())
        if (!owns_node(tracks_, connection.source_node) &&
            !owns_node(tracks_, connection.dest_node))
            connections.push_back(connection);

    NodeId synthetic = 1;
    for (const auto& node : nodes)
        synthetic = std::max(synthetic, node.id + 1u);
    for (const auto& route : ordered) {
        GraphNode audio_node;
        audio_node.id = synthetic++;
        audio_node.type = NodeType::Custom;
        audio_node.num_output_ports = static_cast<int>(config.audio_channels);
        audio_node.custom_type_id = custom_type_id(binding_instance_id_, route.track_id);
        audio_node.custom_type_version = 1;
        audio_node.transport_sensitive = true;
        nodes.push_back(audio_node);

        GraphNode midi_node;
        midi_node.id = synthetic++;
        midi_node.type = NodeType::MidiInput;
        midi_node.num_output_ports = 1;
        nodes.push_back(midi_node);

        for (std::uint32_t channel = 0; channel < config.audio_channels; ++channel) {
            connections.push_back(
                Connection{audio_node.id, static_cast<PortIndex>(channel), route.audio_destination,
                           static_cast<PortIndex>(route.audio_destination_first_port + channel)});
        }
        if (route.midi_destination != 0) {
            Connection midi{midi_node.id, 0, route.midi_destination, 0};
            midi.midi = true;
            connections.push_back(midi);
        }
    }

    const auto graph_limits = graph_.limits();
    if (nodes.size() > graph_limits.max_nodes)
        return reject(TimelineGraphAdmissionCode::NodeLimitExceeded, nodes.size(),
                      graph_limits.max_nodes);
    if (connections.size() > graph_limits.max_connections)
        return reject(TimelineGraphAdmissionCode::ConnectionLimitExceeded, connections.size(),
                      graph_limits.max_connections);

    const bool has_midi = std::any_of(nodes.begin(), nodes.end(),
                                      [](const auto& node) {
                                          return node.type == NodeType::MidiInput ||
                                                 node.type == NodeType::MidiOutput;
                                      }) ||
                          std::any_of(connections.begin(), connections.end(),
                                      [](const auto& connection) { return connection.midi; });
    std::uint64_t graph_ports = 0;
    std::uint64_t routed_ports = 0;
    std::vector<graph::GraphRuntimeNodeSpec> node_specs;
    node_specs.reserve(nodes.size());
    const graph::GraphRuntimeLimits routed_limits;
    for (const auto& node : nodes) {
        const auto inputs = static_cast<std::uint64_t>(std::max(0, node.num_input_ports));
        const auto outputs = static_cast<std::uint64_t>(std::max(0, node.num_output_ports));
        if (inputs > routed_limits.max_ports_per_node ||
            outputs > routed_limits.max_ports_per_node) {
            return reject(TimelineGraphAdmissionCode::PerNodePortLimitExceeded,
                          std::max(inputs, outputs), routed_limits.max_ports_per_node, {},
                          node.id);
        }
        if (!checked_add(graph_ports, inputs) || !checked_add(graph_ports, outputs) ||
            !checked_add(routed_ports, inputs) || !checked_add(routed_ports, outputs) ||
            (has_midi && !checked_add(routed_ports, 2)))
            return reject(TimelineGraphAdmissionCode::TotalPortLimitExceeded,
                          std::numeric_limits<std::uint64_t>::max(),
                          routed_limits.max_total_ports);
        node_specs.push_back({node.id, runtime_kind(node.type), static_cast<std::uint32_t>(inputs),
                              static_cast<std::uint32_t>(outputs), has_midi ? 1u : 0u,
                              has_midi ? 1u : 0u});
    }
    if (graph_ports > graph_limits.max_ports)
        return reject(TimelineGraphAdmissionCode::TotalPortLimitExceeded, graph_ports,
                      graph_limits.max_ports);
    if (nodes.size() > routed_limits.max_nodes)
        return reject(TimelineGraphAdmissionCode::NodeLimitExceeded, nodes.size(),
                      routed_limits.max_nodes);
    if (connections.size() > routed_limits.max_connections)
        return reject(TimelineGraphAdmissionCode::ConnectionLimitExceeded, connections.size(),
                      routed_limits.max_connections);
    if (routed_ports > routed_limits.max_total_ports)
        return reject(TimelineGraphAdmissionCode::TotalPortLimitExceeded, routed_ports,
                      routed_limits.max_total_ports);

    std::vector<graph::GraphRuntimeConnectionSpec> connection_specs;
    connection_specs.reserve(connections.size());
    for (const auto& connection : connections) {
        graph::GraphRuntimeConnectionSpec spec;
        spec.source_node = connection.source_node;
        spec.source_port = connection.source_port;
        spec.dest_node = connection.dest_node;
        spec.dest_port = connection.dest_port;
        spec.feedback = connection.feedback;
        spec.kind = connection.midi ? graph::GraphRuntimeConnectionKind::Event
                                    : graph::GraphRuntimeConnectionKind::Audio;
        connection_specs.push_back(spec);
    }
    const auto plan = graph::build_graph_runtime_plan(node_specs, connection_specs);
    if (!plan.ok())
        return reject(TimelineGraphAdmissionCode::RoutedPlanRejected, plan.error.index, 0, {},
                      plan.error.node_id);

    // Eligibility is checked only after every exact capacity axis above. It is
    // never used as a proxy for routed-plan size.
    if (!signal_graph_topology_executor_eligible(nodes, connections))
        return reject(TimelineGraphAdmissionCode::RoutedTopologyIneligible);
    return {};
}

TimelineGraphAdmission TimelineGraphPlaybackBinding::prepare(
    const playback::PlaybackProgram& program, std::span<const TimelineTrackGraphRoute> routes,
    const TimelineGraphBindingConfig& config, double sample_rate, int maximum_block_size) {
    const double program_sample_rate = sample_rate_double(program.tempo_map().sample_rate());
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0 || sample_rate != program_sample_rate)
        return reject(TimelineGraphAdmissionCode::SampleRateMismatch);
    const auto admission = preflight(program, routes, config, maximum_block_size);
    if (!admission)
        return admission;

    std::vector<TimelineTrackGraphRoute> ordered(routes.begin(), routes.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.track_id < rhs.track_id; });

    // Remove vanished tracks. Unchanged ItemIds retain their node objects and
    // therefore their SignalGraph NodeIds and renderer execution state.
    for (std::size_t index = tracks_.size(); index-- > 0;) {
        if (std::none_of(ordered.begin(), ordered.end(),
                         [&](const auto& route) { return route.track_id == tracks_[index]->id; })) {
            (void)graph_.remove_node(tracks_[index]->audio_node);
            (void)graph_.remove_node(tracks_[index]->midi_node);
            tracks_.erase(tracks_.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    for (const auto& route : ordered) {
        auto found = std::find_if(tracks_.begin(), tracks_.end(),
                                  [&](const auto& track) { return track->id == route.track_id; });
        if (found == tracks_.end()) {
            const auto type_id = custom_type_id(binding_instance_id_, route.track_id);
            std::weak_ptr<detail::TimelineGraphSharedBlockState> shared = shared_;
            const auto track_id = route.track_id;
            const auto audio_limits = config.audio_limits;
            auto audio_renderer =
                std::make_shared<playback::ArrangementAudioTrackRenderer>(track_id);
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
            if (!graph_.register_custom_node_type(std::move(type)))
                return reject(TimelineGraphAdmissionCode::GraphMutationFailed, 0, 0,
                              route.track_id);
            auto track = std::make_unique<detail::TimelineGraphBoundTrack>();
            track->id = route.track_id;
            track->audio_node = graph_.add_custom_node(type_id);
            track->midi_node = graph_.add_midi_input_node("Timeline MIDI track " +
                                                          std::to_string(route.track_id.value));
            track->audio_renderer = std::move(audio_renderer);
            track->note_renderer =
                std::make_unique<playback::ArrangementNoteRenderer>(route.track_id);
            if (track->audio_node == 0 || track->midi_node == 0 ||
                !track->note_renderer->prepare(config.maximum_note_events_per_track_per_block)) {
                if (track->audio_node != 0)
                    (void)graph_.remove_node(track->audio_node);
                if (track->midi_node != 0)
                    (void)graph_.remove_node(track->midi_node);
                return reject(TimelineGraphAdmissionCode::GraphMutationFailed, 0, 0,
                              route.track_id);
            }
            tracks_.push_back(std::move(track));
            found = std::prev(tracks_.end());
        }

        auto& track = **found;
        // Reconcile only binding-owned edges. Program publications never touch
        // graph topology or custom-node state.
        for (const auto& connection : std::vector<Connection>(graph_.connections())) {
            if (connection.source_node == track.audio_node ||
                connection.source_node == track.midi_node) {
                (void)graph_.disconnect(connection.source_node, connection.source_port,
                                        connection.dest_node, connection.dest_port);
            }
        }
        for (std::uint32_t channel = 0; channel < config.audio_channels; ++channel) {
            if (!graph_.connect(
                    track.audio_node, static_cast<PortIndex>(channel), route.audio_destination,
                    static_cast<PortIndex>(route.audio_destination_first_port + channel)))
                return reject(TimelineGraphAdmissionCode::GraphMutationFailed, channel, 0,
                              route.track_id, track.audio_node);
        }
        if (route.midi_destination != 0 &&
            !graph_.connect_midi(track.midi_node, route.midi_destination))
            return reject(TimelineGraphAdmissionCode::GraphMutationFailed, 0, 0, route.track_id,
                          track.midi_node);
        track.route = route;
    }

    std::sort(tracks_.begin(), tracks_.end(),
              [](const auto& lhs, const auto& rhs) { return lhs->id < rhs->id; });
    graph_.set_canonical_executor_routing_enabled(true);
    if (!graph_.prepare(sample_rate, maximum_block_size))
        return reject(TimelineGraphAdmissionCode::GraphPrepareFailed);
    if (!graph_.routed_execution_status(maximum_block_size).strict_routed_ready())
        return reject(TimelineGraphAdmissionCode::RoutedPlanRejected);
    config_ = config;
    prepared_track_ids_.clear();
    prepared_track_ids_.reserve(tracks_.size());
    for (const auto& track : tracks_)
        prepared_track_ids_.push_back(track->id);
    prepared_sample_rate_ = program_sample_rate;
    prepared_max_block_size_ = static_cast<std::uint32_t>(maximum_block_size);
    prepared_ = true;
    return {};
}

TimelineGraphProcessResult
TimelineGraphPlaybackBinding::process(audio::BufferView<float>& output,
                                      const audio::BufferView<const float>& input,
                                      const playback::TransportSnapshot& transport) noexcept {
    runtime::ScopedNoAlloc no_alloc;
    TimelineGraphProcessResult result;
    if (!prepared_) {
        result.code = TimelineGraphProcessCode::MissingProgram;
        return result;
    }
    // Bound every later fail-closed clear before it can touch caller memory.
    // Capacity rejection itself preserves the buffer exactly.
    if (output.empty() || output.num_channels() != config_.audio_channels ||
        output.num_channels() > config_.audio_limits.max_channels ||
        output.num_samples() > config_.audio_limits.max_block_frames ||
        output.num_samples() > prepared_max_block_size_) {
        result.code = TimelineGraphProcessCode::CapacityExceeded;
        return result;
    }
    if (input.num_channels() != output.num_channels() ||
        input.num_samples() != output.num_samples()) {
        result.code = TimelineGraphProcessCode::InputShapeMismatch;
        return result;
    }
    auto block = latch_.begin_block(store_);
    if (!block) {
        result.code = TimelineGraphProcessCode::MissingProgram;
        output.clear();
        return result;
    }
    if (sample_rate_double(block.program()->tempo_map().sample_rate()) != prepared_sample_rate_) {
        result.code = TimelineGraphProcessCode::InvalidTransport;
        return result;
    }
    if (sample_rate_double(transport.sample_rate) != prepared_sample_rate_ ||
        transport.tempo_map != &block.program()->tempo_map() ||
        transport.frame_count != output.num_samples() || transport.range_count == 0) {
        result.code = TimelineGraphProcessCode::InvalidTransport;
        output.clear();
        return result;
    }
    if (block.program()->tracks().size() != prepared_track_ids_.size()) {
        result.code = TimelineGraphProcessCode::TopologyChanged;
        return result;
    }
    for (const auto id : prepared_track_ids_) {
        if (block.program()->find_track(id) == nullptr) {
            result.code = TimelineGraphProcessCode::TopologyChanged;
            return result;
        }
    }
    for (const auto& track : tracks_) {
        const auto note_result = track->note_renderer->process(block, transport);
        result.emitted_note_events += note_result.emitted_events;
        result.dropped_note_events += note_result.dropped_events;
        if (note_result.code != playback::NoteRenderCode::Ok) {
            result.code = TimelineGraphProcessCode::NoteRenderFailed;
            output.clear();
            return result;
        }
        if (!graph_.inject_midi(track->midi_node, track->note_renderer->events())) {
            // Admission bounds every note stream to the graph mailbox's exact
            // capacity, so a failed injection here means the prepared live
            // snapshot (and therefore the admitted routed path) disappeared.
            result.code = TimelineGraphProcessCode::RoutedDispatchFailed;
            output.clear();
            return result;
        }
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
    graph_.process(output, input, static_cast<int>(transport.frame_count), context);
    const bool routed_dispatch_failed =
        graph_.routed_only_execution_failures() != routed_failures_before;
    shared_->block.store(nullptr, std::memory_order_release);
    shared_->transport.store(nullptr, std::memory_order_release);
    result.code = routed_dispatch_failed
                      ? TimelineGraphProcessCode::RoutedDispatchFailed
                      : shared_->audio_code.load(std::memory_order_relaxed);
    return result;
}

NodeId TimelineGraphPlaybackBinding::audio_node_for(timeline::ItemId track_id) const noexcept {
    const auto found = std::find_if(tracks_.begin(), tracks_.end(),
                                    [&](const auto& track) { return track->id == track_id; });
    return found == tracks_.end() ? 0 : (*found)->audio_node;
}

NodeId TimelineGraphPlaybackBinding::midi_input_node_for(timeline::ItemId track_id) const noexcept {
    const auto found = std::find_if(tracks_.begin(), tracks_.end(),
                                    [&](const auto& track) { return track->id == track_id; });
    return found == tracks_.end() ? 0 : (*found)->midi_node;
}

playback::RendererProgramKey
TimelineGraphPlaybackBinding::renderer_key_for(timeline::ItemId track_id) const noexcept {
    const auto found = std::find_if(tracks_.begin(), tracks_.end(),
                                    [&](const auto& track) { return track->id == track_id; });
    return found == tracks_.end() ? playback::RendererProgramKey{}
                                  : (*found)->audio_renderer->active_key();
}

playback::RendererCarryState
TimelineGraphPlaybackBinding::renderer_state_for(timeline::ItemId track_id) const noexcept {
    const auto found = std::find_if(tracks_.begin(), tracks_.end(),
                                    [&](const auto& track) { return track->id == track_id; });
    return found == tracks_.end() ? playback::RendererCarryState{}
                                  : (*found)->audio_renderer->state_snapshot();
}

} // namespace pulp::host
