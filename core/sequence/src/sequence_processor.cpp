#include <pulp/sequence/sequence_processor.hpp>

#include <pulp/midi/block_ops.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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
           status == playback::AudioRenderStatus::Silent;
}

// ArrangementNoteRenderer emits every physical note in MIDI 1 plus a
// full-resolution UMP mirror. Some plugin hosts expose MIDI output without a
// UMP sidecar. That is a supported downgrade, not an incomplete block: deliver
// the representable MIDI 1 stream and copy the mirror only when the adapter
// supplied a destination UMP buffer.
bool copy_arrangement_note_output(const midi::MidiBuffer& source,
                                  midi::MidiBuffer& destination) noexcept {
    bool complete = source.dropped_event_count() == 0 && source.dropped_sysex_count() == 0;
    for (const auto& event : source) {
        if (!destination.add(event))
            complete = false;
    }
    const auto* source_ump = source.ump();
    auto* destination_ump = destination.ump();
    if (source_ump != nullptr) {
        if (source_ump->dropped_event_count() != 0)
            complete = false;
        if (destination_ump != nullptr) {
            for (const auto& event : *source_ump) {
                if (!destination_ump->add(event))
                    complete = false;
            }
        }
    }
    return complete;
}

} // namespace

struct SequenceProcessor::TrackRuntime {
    TrackRuntime(timeline::ItemId id, SequenceProcessor& processor)
        : audio(id), notes(id), owner(processor) {}

    playback::ArrangementAudioTrackRenderer audio;
    playback::ArrangementNoteRenderer notes;
    SequenceProcessor& owner;
};

SequenceProcessor::SequenceProcessor(const playback::PlaybackProgramStore& store,
                                     SequenceProcessorConfig config)
    : store_(store), config_(std::move(config)) {}

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
    if (!prepare_graph(*program, static_cast<std::uint32_t>(context.max_buffer_size))) {
        if (status_ == SequenceProcessorStatus::Unprepared)
            status_ = SequenceProcessorStatus::InvalidConfiguration;
        return;
    }
    if (transport_.prepare(program->tempo_map(),
                           static_cast<std::uint32_t>(context.max_buffer_size)) !=
        HostTransportProjectionError::None) {
        release();
        status_ = SequenceProcessorStatus::InvalidConfiguration;
        return;
    }
    maximum_block_size_ = static_cast<std::uint32_t>(context.max_buffer_size);
    prepared_tempo_map_ = &program->tempo_map();
    status_ = SequenceProcessorStatus::Ready;
}

void SequenceProcessor::release() {
    active_program_ = nullptr;
    active_transport_ = nullptr;
    last_observation_ = {};
    maximum_block_size_ = 0;
    midi_output_node_index_ = 0;
    prepared_tempo_map_ = nullptr;
    tracks_.clear();
    track_ids_.clear();
    snapshot_.clear();
    pool_.clear();
    midi_scratch_.clear();
    transport_.reset();
    status_ = SequenceProcessorStatus::Unprepared;
}

bool SequenceProcessor::prepare_graph(const playback::PlaybackProgram& program,
                                      std::uint32_t maximum_block_size) {
    const auto track_count = program.tracks().size();
    if (track_count + 2 > graph::GraphRuntimeLimits{}.max_nodes)
        return false;

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
    if (!pool_.reset(snapshot_.buffer_slot_count(), maximum_block_size,
                     snapshot_.buffer_assignment().connection_delay_samples) ||
        !midi_scratch_.reset(snapshot_.node_count())) {
        return false;
    }
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

bool SequenceProcessor::process_track(format::ProcessBlock&,
                                      const format::GraphRuntimeNodeProcessContext& context,
                                      void* user_data) noexcept {
    auto* track = static_cast<TrackRuntime*>(user_data);
    if (track == nullptr || !context.routed || track->owner.active_program_ == nullptr ||
        track->owner.active_transport_ == nullptr || context.node_midi_out == nullptr) {
        return false;
    }

    playback::PlaybackProgramBlock block(track->owner.active_program_);
    const auto audio_status =
        track->audio.process(block, *track->owner.active_transport_, context.node_outputs,
                             track->owner.active_program_->audio_limits());
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
    if (status_ != SequenceProcessorStatus::Ready || frames == 0 || frames > maximum_block_size_ ||
        frames != static_cast<std::size_t>(context.num_samples)) {
        audio_output.clear();
        status_ = SequenceProcessorStatus::ExecutorFailed;
        return;
    }

    auto program = latch_.begin_block(store_);
    if (!program) {
        audio_output.clear();
        status_ = SequenceProcessorStatus::MissingProgram;
        return;
    }
    if (!topology_matches(*program.program()) ||
        &program.program()->tempo_map() != prepared_tempo_map_) {
        audio_output.clear();
        status_ = SequenceProcessorStatus::TopologyChanged;
        return;
    }

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

    format::BusBufferSet buses;
    buses.add_output("main", audio_output, format::BusRole::Main);
    format::ProcessBlock process_block;
    process_block.mode = context.process_mode;
    process_block.sample_rate = context.sample_rate;
    process_block.frame_count = static_cast<std::uint32_t>(frames);
    process_block.block_index = transport.block_index;
    process_block.transport = &context;
    process_block.buses = &buses;

    active_program_ = program.program();
    active_transport_ = &transport;
    const auto result = executor_.process_routed(process_block, snapshot_, pool_, &midi_scratch_);
    active_transport_ = nullptr;
    active_program_ = nullptr;
    if (!result.ok()) {
        audio_output.clear();
        status_ = result.error == format::GraphRuntimeExecutorErrorCode::NodeProcessorFailed
                      ? SequenceProcessorStatus::RenderFailed
                      : SequenceProcessorStatus::ExecutorFailed;
        return;
    }

    auto* rendered_midi = midi_scratch_.in(midi_output_node_index_);
    if (rendered_midi == nullptr || !copy_arrangement_note_output(*rendered_midi, midi_out)) {
        status_ = SequenceProcessorStatus::RenderFailed;
        return;
    }
    last_observation_.emitted_midi_events = static_cast<std::uint32_t>(rendered_midi->size());
    status_ = SequenceProcessorStatus::Ready;
}

} // namespace pulp::sequence
