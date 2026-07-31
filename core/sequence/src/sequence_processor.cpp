#include <pulp/sequence/sequence_processor.hpp>

#include "midi_latency_queue.hpp"

#include <pulp/midi/block_ops.hpp>
#include <pulp/midi/ump_buffer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

namespace pulp::sequence {
namespace {

constexpr graph::NodeId kFirstTrackNode = 1;

bool same_sample_rate(double sample_rate, timebase::RationalRate expected) noexcept {
    return std::isfinite(sample_rate) && sample_rate > 0.0 &&
           std::abs(static_cast<long double>(sample_rate) - expected.as_long_double()) <= 1.0e-9L;
}

bool acceptable(playback::AudioRenderStatus status) noexcept {
    return status == playback::AudioRenderStatus::Rendered ||
           status == playback::AudioRenderStatus::Silent ||
           status == playback::AudioRenderStatus::RealtimeStretchGap;
}

playback::AudioRenderStatus audio_status(playback::RealtimeStretchRenderCode code) noexcept {
    using RenderCode = playback::RealtimeStretchRenderCode;
    using Status = playback::AudioRenderStatus;
    switch (code) {
    case RenderCode::NotRequired:
    case RenderCode::Rendered:
        return Status::Rendered;
    case RenderCode::GapIdentityChanged:
        return Status::RealtimeStretchGap;
    case RenderCode::StateRequired:
        return Status::RealtimeStretchStateRequired;
    case RenderCode::StalePublication:
        return Status::RealtimeStretchStalePublication;
    case RenderCode::UnsupportedScrubbing:
        return Status::RealtimeStretchUnsupportedScrubbing;
    case RenderCode::ImpossibleRatio:
        return Status::RealtimeStretchImpossibleRatio;
    case RenderCode::Backpressure:
        return Status::RealtimeStretchBackpressure;
    case RenderCode::Underflow:
        return Status::RealtimeStretchUnderflow;
    }
    return Status::RealtimeStretchStateRequired;
}

} // namespace

struct SequenceProcessor::PreparedPublication {
    std::shared_ptr<const playback::PlaybackProgram> program;
    std::unique_ptr<playback::RealtimeStretchProgramRuntime> realtime_stretch;
};

struct SequenceProcessor::TrackRuntime {
    TrackRuntime(timeline::ItemId id, SequenceProcessor& processor)
        : audio(id), notes(id), owner(processor) {}

    playback::ArrangementAudioTrackRenderer audio;
    playback::ArrangementNoteRenderer notes;
    SequenceProcessor& owner;
};

SequenceProcessor::SequenceProcessor(const playback::PlaybackProgramStore& store,
                                     SequenceProcessorConfig config)
    : store_(store), config_(std::move(config)),
      midi_delay_(std::make_unique<detail::MidiLatencyQueue>()) {}

SequenceProcessor::~SequenceProcessor() = default;

format::PluginDescriptor SequenceProcessor::descriptor() const {
    format::PluginDescriptor result;
    result.name = config_.name;
    result.manufacturer = config_.manufacturer;
    result.bundle_id = config_.bundle_id;
    result.version = config_.version;
    result.category = format::PluginCategory::Instrument;
    result.input_buses.clear();
    result.output_buses = {{"Sequence Output", static_cast<int>(config_.output_channels), false}};
    result.accepts_midi = false;
    result.produces_midi = true;
    result.supports_ump = true;
    return result;
}

void SequenceProcessor::define_parameters(state::StateStore&) {}

void SequenceProcessor::prepare(const format::PrepareContext& context) {
    release();
    if (context.max_buffer_size <= 0 || context.output_channels <= 0 ||
        context.output_channels != static_cast<int>(config_.output_channels) ||
        config_.output_channels == 0 ||
        config_.output_channels > graph::GraphRuntimeLimits{}.max_ports_per_node ||
        config_.maximum_note_events_per_track_per_block == 0) {
        status_ = SequenceProcessorStatus::InvalidConfiguration;
        return;
    }
    auto program = store_.read();
    if (!program) {
        status_ = SequenceProcessorStatus::MissingProgram;
        return;
    }
    if (!same_sample_rate(context.sample_rate, program->tempo_map().sample_rate())) {
        status_ = SequenceProcessorStatus::SampleRateMismatch;
        return;
    }
    auto publication = std::make_shared<PreparedPublication>();
    publication->program = std::make_shared<const playback::PlaybackProgram>(*program);
    publication->realtime_stretch = std::make_unique<playback::RealtimeStretchProgramRuntime>();
    const auto stretch_admission = publication->realtime_stretch->prepare(
        *publication->program, context.sample_rate,
        static_cast<std::uint32_t>(context.max_buffer_size), config_.output_channels,
        publication->program->audio_limits());
    if (!stretch_admission) {
        status_ = SequenceProcessorStatus::RealtimeStretchRejected;
        return;
    }
    if (!prepare_graph(*publication, static_cast<std::uint32_t>(context.max_buffer_size))) {
        if (status_ == SequenceProcessorStatus::Unprepared)
            status_ = SequenceProcessorStatus::InvalidConfiguration;
        return;
    }
    if (transport_.prepare(publication->program->tempo_map(),
                           static_cast<std::uint32_t>(context.max_buffer_size)) !=
        HostTransportProjectionError::None) {
        release();
        status_ = SequenceProcessorStatus::InvalidConfiguration;
        return;
    }
    maximum_block_size_ = static_cast<std::uint32_t>(context.max_buffer_size);
    prepared_tempo_map_ = &publication->program->tempo_map();
    prepared_latency_samples_ = publication->realtime_stretch->latency_samples();
    publication_.publish(std::move(publication));
    prepared_ = true;
    status_ = SequenceProcessorStatus::Ready;
}

void SequenceProcessor::release() {
    active_program_ = nullptr;
    active_transport_ = nullptr;
    active_realtime_stretch_ = nullptr;
    last_observation_ = {};
    maximum_block_size_ = 0;
    prepared_ = false;
    midi_output_node_index_ = 0;
    prepared_tempo_map_ = nullptr;
    if (publication_.has_value())
        publication_.unpublish();
    tracks_.clear();
    track_ids_.clear();
    snapshot_.clear();
    pool_.clear();
    midi_scratch_.clear();
    midi_delay_->reset();
    midi_delay_publication_ = nullptr;
    midi_delay_playback_epoch_ = 0;
    prepared_latency_samples_ = 0;
    transport_.reset();
    status_ = SequenceProcessorStatus::Unprepared;
}

bool SequenceProcessor::prepare_graph(const PreparedPublication& publication,
                                      std::uint32_t maximum_block_size) {
    const auto& program = *publication.program;
    const auto& realtime_stretch = *publication.realtime_stretch;
    const auto track_count = program.tracks().size();
    if (track_count + 2 > graph::GraphRuntimeLimits{}.max_nodes)
        return false;
    if (track_count != 0 && config_.maximum_note_events_per_track_per_block >
                                std::numeric_limits<std::size_t>::max() / track_count)
        return false;
    const auto aggregate_note_capacity =
        std::max<std::size_t>(1, track_count * config_.maximum_note_events_per_track_per_block);

    std::vector<graph::GraphRuntimeNodeSpec> nodes;
    std::vector<graph::GraphRuntimeConnectionSpec> connections;
    std::vector<format::GraphRuntimeNodeBinding> bindings;
    nodes.reserve(track_count + 2);
    connections.reserve(track_count * (static_cast<std::size_t>(config_.output_channels) + 1));
    bindings.reserve(track_count + 2);
    tracks_.reserve(track_count);
    track_ids_.reserve(track_count);

    for (std::size_t index = 0; index < track_count; ++index) {
        const auto id = kFirstTrackNode + static_cast<graph::NodeId>(index);
        const auto track_id = program.tracks()[index]->id();
        auto runtime = std::make_unique<TrackRuntime>(track_id, *this);
        if (!runtime->notes.prepare(config_.maximum_note_events_per_track_per_block)) {
            return false;
        }
        nodes.push_back({
            id,
            graph::GraphRuntimeNodeKind::Custom,
            0,
            config_.output_channels,
            0,
            1,
            false,
            realtime_stretch.track_uses_realtime_stretch(track_id)
                ? realtime_stretch.latency_samples()
                : 0u,
        });
        bindings.push_back({id, &SequenceProcessor::process_track, runtime.get(), true});
        track_ids_.push_back(track_id);
        tracks_.push_back(std::move(runtime));
    }

    const auto audio_output_id = kFirstTrackNode + static_cast<graph::NodeId>(track_count);
    const auto midi_output_id = audio_output_id + 1;
    nodes.push_back({
        audio_output_id,
        graph::GraphRuntimeNodeKind::AudioOutput,
        config_.output_channels,
        0,
        0,
        0,
    });
    bindings.push_back({audio_output_id, nullptr, nullptr, false});
    nodes.push_back({
        midi_output_id,
        graph::GraphRuntimeNodeKind::MidiOutput,
        0,
        0,
        1,
        0,
    });
    bindings.push_back({midi_output_id, nullptr, nullptr, false});

    for (std::size_t index = 0; index < track_count; ++index) {
        const auto track_node = kFirstTrackNode + static_cast<graph::NodeId>(index);
        for (std::uint32_t channel = 0; channel < config_.output_channels; ++channel) {
            connections.push_back({
                track_node,
                channel,
                audio_output_id,
                channel,
                false,
                graph::GraphRuntimeConnectionKind::Audio,
                {},
            });
        }
        connections.push_back({
            track_node,
            0,
            midi_output_id,
            0,
            false,
            graph::GraphRuntimeConnectionKind::Event,
            {},
        });
    }

    auto plan = graph::build_graph_runtime_plan(nodes, connections);
    if (!plan.ok() || !snapshot_.reset(std::move(plan.plan), bindings))
        return false;
    std::vector<std::size_t> midi_capacities(snapshot_.node_count(), 1);
    for (std::uint32_t index = 0; index < snapshot_.plan().nodes.size(); ++index) {
        const auto id = snapshot_.plan().nodes[index].id;
        if (id >= kFirstTrackNode && id < audio_output_id)
            midi_capacities[index] = config_.maximum_note_events_per_track_per_block;
        else if (id == midi_output_id)
            midi_capacities[index] = aggregate_note_capacity;
    }
    if (!pool_.reset(snapshot_.buffer_slot_count(), maximum_block_size,
                     snapshot_.buffer_assignment().connection_delay_samples) ||
        !midi_scratch_.reset(midi_capacities)) {
        return false;
    }
    if (!midi_delay_->prepare(aggregate_note_capacity, realtime_stretch.latency_samples(),
                              config_.maximum_delayed_events))
        return false;
    for (std::uint32_t index = 0; index < snapshot_.plan().nodes.size(); ++index) {
        if (snapshot_.plan().nodes[index].id == midi_output_id) {
            midi_output_node_index_ = index;
            return true;
        }
    }
    return false;
}

bool SequenceProcessor::topology_matches(const playback::PlaybackProgram& program) const noexcept {
    if (program.tracks().size() != track_ids_.size())
        return false;
    for (std::size_t index = 0; index < track_ids_.size(); ++index) {
        if (program.tracks()[index]->id() != track_ids_[index])
            return false;
    }
    return true;
}

bool SequenceProcessor::adopt_latest_program() {
    if (!prepared_)
        return false;
    auto latest = store_.read();
    if (!latest || !topology_matches(*latest) || &latest->tempo_map() != prepared_tempo_map_ ||
        latest->tempo_map().sample_rate() != prepared_tempo_map_->sample_rate())
        return false;

    auto candidate = std::make_shared<PreparedPublication>();
    candidate->program = std::make_shared<const playback::PlaybackProgram>(*latest);
    candidate->realtime_stretch = std::make_unique<playback::RealtimeStretchProgramRuntime>();
    const auto admission = candidate->realtime_stretch->prepare(
        *candidate->program,
        static_cast<double>(candidate->program->tempo_map().sample_rate().as_long_double()),
        maximum_block_size_, config_.output_channels, candidate->program->audio_limits());
    if (!admission)
        return false;

    auto current = publication_.read();
    if (!current)
        return false;
    if (latest->generation() < current->program->generation())
        return false;
    if (current->realtime_stretch->latency_samples() !=
        candidate->realtime_stretch->latency_samples())
        return false;
    for (const auto id : track_ids_)
        if (current->realtime_stretch->track_uses_realtime_stretch(id) !=
            candidate->realtime_stretch->track_uses_realtime_stretch(id))
            return false;

    publication_.publish(std::move(candidate));
    return true;
}

void SequenceProcessor::force_realtime_stretch_failure_for_test() noexcept {
    auto publication = publication_.read();
    if (publication && publication->realtime_stretch)
        publication->realtime_stretch->force_post_mutation_failure_for_test();
}

bool SequenceProcessor::process_track(format::ProcessBlock&,
                                      const format::GraphRuntimeNodeProcessContext& context,
                                      void* user_data) noexcept {
    auto* track = static_cast<TrackRuntime*>(user_data);
    if (track == nullptr || !context.routed || track->owner.active_program_ == nullptr ||
        track->owner.active_transport_ == nullptr || context.node_midi_out == nullptr) {
        return false;
    }

    playback::PlaybackProgramBlock block(track->owner.active_program_);
    const auto audio_status = track->audio.process(
        block, *track->owner.active_transport_, context.node_outputs,
        track->owner.active_program_->audio_limits(), track->owner.active_realtime_stretch_);
    if (!acceptable(audio_status)) {
        track->owner.block_audio_status_.store(audio_status, std::memory_order_relaxed);
    } else {
        auto expected = playback::AudioRenderStatus::Silent;
        (void)track->owner.block_audio_status_.compare_exchange_strong(expected, audio_status,
                                                                       std::memory_order_relaxed);
    }
    if (!acceptable(audio_status))
        return false;

    const auto note_result = track->notes.process(block, *track->owner.active_transport_);
    midi::clear_midi_block(*context.node_midi_out);
    return note_result.code == playback::NoteRenderCode::Ok &&
           midi::copy_midi_block(track->notes.events(), *context.node_midi_out);
}

void SequenceProcessor::process(audio::BufferView<float>& audio_output,
                                const audio::BufferView<const float>&, midi::MidiBuffer&,
                                midi::MidiBuffer& midi_out, const format::ProcessContext& context) {
    const auto frames = audio_output.num_samples();
    midi::clear_midi_block(midi_out);
    if (!prepared_) {
        audio_output.clear();
        return;
    }
    if (frames == 0 && context.num_samples == 0) {
        audio_output.clear();
        return;
    }
    if (frames > maximum_block_size_ || frames != static_cast<std::size_t>(context.num_samples)) {
        audio_output.clear();
        status_ = SequenceProcessorStatus::ExecutorFailed;
        return;
    }

    auto publication = publication_.read();
    if (!publication) {
        audio_output.clear();
        status_ = SequenceProcessorStatus::MissingProgram;
        return;
    }
    const auto* program = publication->program.get();

    playback::TransportSnapshot transport;
    if (transport_.project(context, transport) != HostTransportProjectionError::None) {
        audio_output.clear();
        status_ = SequenceProcessorStatus::TransportRejected;
        return;
    }
    last_observation_.timeline_tick_start = transport.ranges[0].timeline_tick_start;
    last_observation_.discontinuity = transport.ranges[0].discontinuity;
    last_observation_.emitted_midi_events = 0;
    last_observation_.valid = true;
    last_observation_.audio_status = playback::AudioRenderStatus::Silent;

    // The queue is callback-owned. Publication adoption and transport seeks
    // are observed here so the control thread never races its ring indices and
    // no old-generation/old-position note can emerge after the boundary.
    if (!transport.is_playing || midi_delay_publication_ != program ||
        midi_delay_playback_epoch_ != transport.ranges[0].playback_epoch ||
        transport.ranges[0].discontinuity) {
        midi_delay_->clear_pending();
        midi_delay_publication_ = program;
        midi_delay_playback_epoch_ = transport.ranges[0].playback_epoch;
    }
    if (!transport.is_playing)
        publication->realtime_stretch->reset();

    // Reject the complete block before GraphRuntimeExecutor or any note
    // renderer can mutate callback state. A rejected Stretch lane therefore
    // cannot leave successful siblings' audio or MIDI side effects behind.
    if (transport.is_playing) {
        for (const auto& track : program->tracks()) {
            const auto preflight = publication->realtime_stretch->preflight_track(
                *program, *track, transport, audio_output);
            if (preflight != playback::RealtimeStretchRenderCode::NotRequired &&
                preflight != playback::RealtimeStretchRenderCode::Rendered &&
                preflight != playback::RealtimeStretchRenderCode::GapIdentityChanged) {
                audio_output.clear();
                last_observation_.audio_status = audio_status(preflight);
                status_ = SequenceProcessorStatus::RenderFailed;
                return;
            }
        }
    }

    format::BusBufferSet buses;
    buses.add_output("main", audio_output, format::BusRole::Main);
    format::ProcessBlock process_block;
    process_block.mode = context.process_mode;
    process_block.sample_rate = context.sample_rate;
    process_block.frame_count = static_cast<std::uint32_t>(frames);
    process_block.block_index = transport.block_index;
    process_block.transport = &context;
    process_block.buses = &buses;

    active_program_ = program;
    active_realtime_stretch_ = publication->realtime_stretch.get();
    block_audio_status_.store(playback::AudioRenderStatus::Silent, std::memory_order_relaxed);
    active_transport_ = &transport;
    const auto result = executor_.process_routed(process_block, snapshot_, pool_, &midi_scratch_);
    active_transport_ = nullptr;
    active_realtime_stretch_ = nullptr;
    active_program_ = nullptr;
    last_observation_.audio_status = block_audio_status_.load(std::memory_order_relaxed);
    if (!result.ok()) {
        audio_output.clear();
        status_ = result.error == format::GraphRuntimeExecutorErrorCode::NodeProcessorFailed
                      ? SequenceProcessorStatus::RenderFailed
                      : SequenceProcessorStatus::ExecutorFailed;
        return;
    }

    auto* rendered_midi = midi_scratch_.in(midi_output_node_index_);
    if (rendered_midi == nullptr) {
        midi::clear_midi_block(midi_out);
        status_ = SequenceProcessorStatus::RenderFailed;
        return;
    }
    if (!transport.is_playing) {
        midi_delay_->clear_pending();
        if (!midi::copy_midi_block(*rendered_midi, midi_out)) {
            midi::clear_midi_block(midi_out);
            status_ = SequenceProcessorStatus::RenderFailed;
            return;
        }
        last_observation_.emitted_midi_events = static_cast<std::uint32_t>(midi_out.size());
        status_ = SequenceProcessorStatus::Ready;
        return;
    }
    if (!midi_delay_->process(*rendered_midi, midi_out, static_cast<std::uint32_t>(frames))) {
        midi::clear_midi_block(midi_out);
        status_ = SequenceProcessorStatus::RenderFailed;
        return;
    }
    last_observation_.emitted_midi_events = static_cast<std::uint32_t>(midi_out.size());
    status_ = SequenceProcessorStatus::Ready;
}

} // namespace pulp::sequence
