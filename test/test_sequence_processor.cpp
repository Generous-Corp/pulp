#include "support/timeline_graph_binding_test_support.hpp"

#include "../core/sequence/src/midi_latency_queue.hpp"

#include <pulp/format/playback_context_projection.hpp>
#include <pulp/sequence/sequence_processor.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <optional>
#include <string>

namespace {

class UmpOutputAttachment {
  public:
    UmpOutputAttachment(midi::MidiBuffer& output, std::size_t capacity) : output_(output) {
        ump_.reserve(capacity);
        ump_.set_realtime_capacity_limit(true);
        output_.attach_ump(&ump_);
    }
    ~UmpOutputAttachment() {
        output_.attach_ump(nullptr);
    }
    UmpOutputAttachment(const UmpOutputAttachment&) = delete;
    UmpOutputAttachment& operator=(const UmpOutputAttachment&) = delete;

    const midi::UmpBuffer& ump() const noexcept {
        return ump_;
    }

  private:
    midi::MidiBuffer& output_;
    midi::UmpBuffer ump_;
};

format::ProcessContext host_context(const TransportSnapshot& snapshot) {
    auto context = format::project_process_context(snapshot, snapshot.ranges[0]);
    context.num_samples = static_cast<int>(snapshot.frame_count);
    context.position_samples = snapshot.ranges[0].timeline_sample_start.value;
    context.transport_jump = snapshot.ranges[0].discontinuity;
    return context;
}

TransportSnapshot loop_block(const PlaybackProgram& program, std::uint64_t block_index,
                             std::int64_t start, std::uint32_t frames) {
    auto result = snapshot(program, frames, start);
    result.block_index = block_index;
    result.loop = {
        true,
        program.tempo_map().samples_to_ticks({0}),
        program.tempo_map().samples_to_ticks({64}),
    };
    const auto until_wrap = static_cast<std::uint32_t>(64 - start);
    if (until_wrap < frames) {
        result.range_count = 2;
        result.ranges[0].frame_count = until_wrap;
        result.ranges[0].timeline_tick_end = result.loop.end;
        result.ranges[1].sample_offset = until_wrap;
        result.ranges[1].frame_count = frames - until_wrap;
        result.ranges[1].timeline_sample_start = {0};
        result.ranges[1].timeline_tick_start = result.loop.start;
        result.ranges[1].timeline_tick_end =
            program.tempo_map().samples_to_ticks({static_cast<std::int64_t>(frames - until_wrap)});
        result.ranges[1].discontinuity = true;
    }
    return result;
}

void require_same_midi(const midi::MidiBuffer& expected, const midi::MidiBuffer& actual) {
    REQUIRE(actual.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        REQUIRE(actual[index].sample_offset == expected[index].sample_offset);
        REQUIRE(actual[index].message == expected[index].message);
    }
}

std::shared_ptr<const Project> host_tempo_note_project() {
    NoteEvent event;
    event.id = {101};
    event.start = {2 * kTicksPerQuarter};
    event.duration = {kTicksPerQuarter / 4};
    event.velocity = 0xffff;
    event.pitch = 60;
    auto content = take(NoteContent::create({event}));
    auto clip = take(Clip::create({100}, {0}, {4 * kTicksPerQuarter}, std::move(content)));
    auto track = take(Track::create({10}, "host-tempo notes", {std::move(clip)}));
    auto sequence =
        take(Sequence::create({2}, "root", TickDuration{4 * kTicksPerQuarter}, {std::move(track)}));
    return std::make_shared<const Project>(take(
        Project::create(ProjectInput{{1}, "host tempo", 1'000, {2}, {}, {std::move(sequence)}})));
}

std::shared_ptr<const Project> host_tempo_same_sample_boundary_project() {
    NoteEvent ending;
    ending.id = {101};
    ending.start = {kTicksPerQuarter};
    ending.duration = {kTicksPerQuarter};
    ending.velocity = 0xffff;
    ending.pitch = 60;
    NoteEvent starting;
    starting.id = {102};
    starting.start = {2 * kTicksPerQuarter - 1};
    starting.duration = {kTicksPerQuarter / 4};
    starting.velocity = 0xffff;
    starting.pitch = 61;
    auto content = take(NoteContent::create({ending, starting}));
    auto clip = take(Clip::create({100}, {0}, {4 * kTicksPerQuarter}, std::move(content)));
    auto track = take(Track::create({10}, "same-sample boundary", {std::move(clip)}));
    auto sequence =
        take(Sequence::create({2}, "root", TickDuration{4 * kTicksPerQuarter}, {std::move(track)}));
    return std::make_shared<const Project>(take(
        Project::create(ProjectInput{{1}, "boundary", 1'000, {2}, {}, {std::move(sequence)}})));
}

std::shared_ptr<const Project> dense_multitrack_note_project(std::size_t notes_per_track) {
    std::vector<Track> tracks;
    tracks.reserve(2);
    std::uint64_t next_id = 100;
    for (std::size_t track_index = 0; track_index < 2; ++track_index) {
        std::vector<NoteEvent> events;
        events.reserve(notes_per_track);
        for (std::size_t note_index = 0; note_index < notes_per_track; ++note_index) {
            NoteEvent event;
            event.id = {next_id++};
            event.start = {};
            event.duration = {kTicksPerQuarter};
            event.velocity = 0xffff;
            event.pitch = static_cast<std::uint8_t>(note_index % 128);
            event.channel = static_cast<std::uint8_t>(note_index / 128);
            events.push_back(event);
        }
        auto content = take(NoteContent::create(std::move(events)));
        auto clip =
            take(Clip::create({next_id++}, {0}, {2 * kTicksPerQuarter}, std::move(content)));
        tracks.push_back(take(Track::create({next_id++}, "dense", {std::move(clip)})));
    }
    auto sequence =
        take(Sequence::create({2}, "root", TickDuration{2 * kTicksPerQuarter}, std::move(tracks)));
    return std::make_shared<const Project>(
        take(Project::create(ProjectInput{{1}, "dense", next_id, {2}, {}, {std::move(sequence)}})));
}

std::shared_ptr<const Project> single_note_project(std::uint8_t pitch) {
    NoteEvent event;
    event.id = {101};
    event.start = {};
    event.duration = {kTicksPerQuarter};
    event.velocity = 0xffff;
    event.pitch = pitch;
    auto content = take(NoteContent::create({event}));
    auto clip = take(Clip::create({100}, {0}, {2 * kTicksPerQuarter}, std::move(content)));
    auto track = take(Track::create({10}, "adopted note", {std::move(clip)}));
    auto sequence =
        take(Sequence::create({2}, "root", TickDuration{2 * kTicksPerQuarter}, {std::move(track)}));
    return std::make_shared<const Project>(take(
        Project::create(ProjectInput{{1}, "adoption", 1'000, {2}, {}, {std::move(sequence)}})));
}

std::shared_ptr<const Project> stretch_latency_alignment_project(std::uint64_t source_frames) {
    ClipPlaybackProperties playback;
    auto impulse = take(
        Clip::create_absolute({100}, {0}, 512, {48'000, 1}, MediaRef{{3}, {0}, 512}, playback));
    auto stretch =
        take(Clip::create({101}, {0}, {kTicksPerQuarter}, MediaRef{{4}, {0}, source_frames},
                          playback, TimeConform::Stretch));

    NoteEvent note;
    note.id = {103};
    note.start = {};
    note.duration = {kTicksPerQuarter};
    note.velocity = 0xffff;
    note.pitch = 60;
    auto notes = take(NoteContent::create({note}));
    auto note_clip = take(Clip::create({102}, {0}, {2 * kTicksPerQuarter}, std::move(notes)));
    auto impulse_track =
        take(Track::create({10}, "latency alignment impulse", {std::move(impulse)}));
    auto stretch_track = take(Track::create({11}, "latency declaration", {std::move(stretch)}));
    auto note_track = take(Track::create({12}, "latency alignment note", {std::move(note_clip)}));
    auto sequence =
        take(Sequence::create({2}, "root", TickDuration{10 * kTicksPerQuarter},
                              std::vector<Track>{std::move(impulse_track), std::move(stretch_track),
                                                 std::move(note_track)}));
    const auto impulse_hash = ContentHash::from_hex(std::string(64, 'a'));
    const auto stretch_hash = ContentHash::from_hex(std::string(64, 'b'));
    REQUIRE(impulse_hash);
    REQUIRE(stretch_hash);
    MediaAsset impulse_asset{
        .id = {3},
        .name = "impulse.wav",
        .frame_count = source_frames,
        .sample_rate = {48'000, 1},
        .content_hash = *impulse_hash,
    };
    MediaAsset stretch_asset{
        .id = {4},
        .name = "silent-stretch.wav",
        .frame_count = source_frames,
        .sample_rate = {48'000, 1},
        .content_hash = *stretch_hash,
    };
    return std::make_shared<const Project>(
        take(Project::create(ProjectInput{{1},
                                          "stretch latency",
                                          1'000,
                                          {2},
                                          {impulse_asset, stretch_asset},
                                          {std::move(sequence)}})));
}

} // namespace

TEST_CASE("sequence MIDI latency queue delays paired MIDI and UMP without leakage",
          "[sequence][midi-latency]") {
    sequence::detail::MidiLatencyQueue queue;
    REQUIRE(queue.prepare(4, 5, 64));

    midi::MidiBuffer source;
    source.reserve(4);
    source.set_realtime_capacity_limit(true);
    auto note = midi::MidiEvent::note_on(0, 60, 100);
    note.sample_offset = 2;
    REQUIRE(source.add(note));
    midi::UmpBuffer source_ump;
    source_ump.reserve(4);
    source_ump.set_realtime_capacity_limit(true);
    REQUIRE(source_ump.add(midi::UmpPacket::note_on_2(0, 0, 60, 0xffff), 2));
    source.attach_ump(&source_ump);

    midi::MidiBuffer destination;
    destination.reserve(4);
    destination.set_realtime_capacity_limit(true);
    midi::UmpBuffer destination_ump;
    destination_ump.reserve(4);
    destination_ump.set_realtime_capacity_limit(true);
    destination.attach_ump(&destination_ump);
    REQUIRE(queue.process(source, destination, 4));
    REQUIRE(destination.empty());
    REQUIRE(destination_ump.empty());

    source.clear();
    source_ump.clear();
    REQUIRE(queue.process(source, destination, 4));
    REQUIRE(destination.size() == 1);
    REQUIRE(destination.begin()->sample_offset == 3);
    REQUIRE(destination_ump.size() == 1);
    REQUIRE(destination_ump[0].sample_offset == 3);

    // Clear discards both representations together. A queued UMP packet also
    // fails closed before mutation when the destination lacks its sidecar.
    destination.clear();
    destination_ump.clear();
    REQUIRE(source.add(note));
    REQUIRE(source_ump.add(midi::UmpPacket::note_on_2(0, 0, 60, 0xffff), 2));
    REQUIRE(queue.process(source, destination, 4));
    queue.clear_pending();
    source.clear();
    source_ump.clear();
    REQUIRE(queue.process(source, destination, 8));
    REQUIRE(destination.empty());
    REQUIRE(destination_ump.empty());

    sequence::detail::MidiLatencyQueue missing_sidecar;
    REQUIRE(missing_sidecar.prepare(4, 5, 64));
    REQUIRE(source.add(note));
    REQUIRE(source_ump.add(midi::UmpPacket::note_on_2(0, 0, 60, 0xffff), 2));
    midi::MidiBuffer midi_only;
    midi_only.reserve(4);
    midi_only.set_realtime_capacity_limit(true);
    REQUIRE_FALSE(missing_sidecar.process(source, midi_only, 4));
    REQUIRE(midi_only.empty());

    sequence::detail::MidiLatencyQueue full_ump_destination;
    REQUIRE(full_ump_destination.prepare(4, 5, 64));
    midi::MidiBuffer paired_destination;
    paired_destination.reserve(4);
    paired_destination.set_realtime_capacity_limit(true);
    midi::UmpBuffer no_ump_capacity;
    no_ump_capacity.set_realtime_capacity_limit(true);
    for (std::size_t index = 0; index < no_ump_capacity.capacity(); ++index)
        REQUIRE(no_ump_capacity.add(midi::UmpPacket::note_on_2(0, 0, 1, 1), 0));
    paired_destination.attach_ump(&no_ump_capacity);
    REQUIRE(full_ump_destination.process(source, paired_destination, 4));
    source.clear();
    source_ump.clear();
    source.attach_ump(nullptr);
    REQUIRE_FALSE(full_ump_destination.process(source, paired_destination, 4));
    REQUIRE(paired_destination.empty());
    REQUIRE(no_ump_capacity.size() == no_ump_capacity.capacity());

    midi::UmpBuffer retry_ump;
    retry_ump.reserve(4);
    retry_ump.set_realtime_capacity_limit(true);
    paired_destination.attach_ump(&retry_ump);
    REQUIRE(full_ump_destination.process(source, paired_destination, 4));
    REQUIRE(paired_destination.size() == 1);
    REQUIRE(paired_destination.begin()->sample_offset == 3);
    REQUIRE(retry_ump.size() == 1);
    REQUIRE(retry_ump[0].sample_offset == 3);
}

TEST_CASE("embedded sequence processor schedules program-beat notes on the host beat clock") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(host_tempo_note_project(), map, take(DecodedAudioAssetPool::create({})), 1);

    sequence::SequenceProcessorConfig processor_config;
    processor_config.output_channels = 1;
    sequence::SequenceProcessor embedded(programs.store, processor_config);
    state::StateStore state;
    embedded.define_parameters(state);
    embedded.prepare({
        .sample_rate = 48'000.0,
        .max_buffer_size = 64'000,
        .input_channels = 0,
        .output_channels = 1,
    });
    REQUIRE(embedded.ready());

    auto process = [&](format::ProcessContext context, std::uint32_t frames,
                       midi::MidiBuffer& output) {
        Buffer silence(1, frames);
        Buffer audio_output(1, frames);
        auto output_view = audio_output.view();
        midi::MidiBuffer input;
        context.sample_rate = 48'000.0;
        context.num_samples = static_cast<int>(frames);
        embedded.process(output_view, silence.const_view(), input, output, context);
        REQUIRE(embedded.ready());
        REQUIRE(embedded.status() == sequence::SequenceProcessorStatus::Ready);
    };

    SECTION("mismatched host tempo maps beat two into the current output block") {
        midi::MidiBuffer output;
        output.reserve(16);
        output.set_realtime_capacity_limit(true);
        UmpOutputAttachment output_ump(output, 16);
        format::ProcessContext context;
        context.is_playing = true;
        context.tempo_bpm = 60.0;
        context.position_beats = 1.75;
        context.position_samples = 84'000;
        context.transport_validity.set(format::TransportField::BeatPosition);
        context.transport_validity.set(format::TransportField::Tempo);
        context.transport_validity.set(format::TransportField::SamplePosition);
        process(context, 16'000, output);

        REQUIRE(output.size() == 1);
        REQUIRE(output[0].is_note_on());
        REQUIRE(output[0].sample_offset >= 11'998);
        REQUIRE(output[0].sample_offset <= 12'000);
    }

    SECTION("exact host range boundaries retain half-open note scheduling") {
        midi::MidiBuffer output;
        output.reserve(16);
        output.set_realtime_capacity_limit(true);
        UmpOutputAttachment output_ump(output, 16);
        format::ProcessContext before;
        before.is_playing = true;
        before.tempo_bpm = 60.0;
        before.position_beats = 2.0 - 100.0 / 48'000.0;
        before.position_samples = 95'900;
        before.transport_validity.set(format::TransportField::BeatPosition);
        before.transport_validity.set(format::TransportField::Tempo);
        before.transport_validity.set(format::TransportField::SamplePosition);
        process(before, 100, output);
        REQUIRE(output.empty());

        format::ProcessContext at;
        at.is_playing = true;
        at.tempo_bpm = 60.0;
        at.position_beats = 2.0;
        at.position_samples = 96'000;
        at.transport_validity.set(format::TransportField::BeatPosition);
        at.transport_validity.set(format::TransportField::Tempo);
        at.transport_validity.set(format::TransportField::SamplePosition);
        process(at, 100, output);
        REQUIRE(output.size() == 1);
        REQUIRE(output[0].is_note_on());
        REQUIRE(output[0].sample_offset == 0);
    }

    SECTION("loop wrap maps the same program beat after the host loop restart") {
        midi::MidiBuffer output;
        output.reserve(16);
        output.set_realtime_capacity_limit(true);
        UmpOutputAttachment output_ump(output, 16);
        format::ProcessContext context;
        context.is_playing = true;
        context.is_looping = true;
        context.tempo_bpm = 60.0;
        context.position_beats = 2.25;
        context.position_samples = 108'000;
        context.loop_start_beats = 1.5;
        context.loop_end_beats = 2.5;
        context.transport_validity.set(format::TransportField::BeatPosition);
        context.transport_validity.set(format::TransportField::Tempo);
        context.transport_validity.set(format::TransportField::SamplePosition);
        context.transport_validity.set(format::TransportField::LoopRange);
        process(context, 40'000, output);

        REQUIRE(output.size() == 1);
        REQUIRE(output[0].is_note_on());
        REQUIRE(output[0].sample_offset >= 35'998);
        REQUIRE(output[0].sample_offset <= 36'000);
    }

    SECTION("fallback-to-host mapping transition is a discontinuity and still schedules") {
        midi::MidiBuffer output;
        output.reserve(16);
        output.set_realtime_capacity_limit(true);
        UmpOutputAttachment output_ump(output, 16);
        format::ProcessContext fallback;
        fallback.is_playing = true;
        fallback.position_samples = 41'968;
        process(fallback, 32, output);
        REQUIRE(output.empty());

        format::ProcessContext mapped;
        mapped.is_playing = true;
        mapped.tempo_bpm = 60.0;
        mapped.position_beats = 1.75;
        mapped.position_samples = 42'000;
        mapped.transport_validity.set(format::TransportField::BeatPosition);
        mapped.transport_validity.set(format::TransportField::Tempo);
        mapped.transport_validity.set(format::TransportField::SamplePosition);
        process(mapped, 16'000, output);

        REQUIRE(embedded.last_observation().discontinuity);
        REQUIRE(output.size() == 1);
        REQUIRE(output[0].sample_offset >= 11'998);
        REQUIRE(output[0].sample_offset <= 12'000);
    }
}

TEST_CASE("host-mapped note scheduling preserves tick order within one compiled sample") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(host_tempo_same_sample_boundary_project(), map,
                     take(DecodedAudioAssetPool::create({})), 1);
    const auto compiled = programs.store.read();
    REQUIRE(compiled);
    const auto* compiled_track = compiled->find_track({10});
    REQUIRE(compiled_track != nullptr);
    REQUIRE(compiled_track->arrangement_note_events().size() == 4);
    const auto events = compiled_track->arrangement_note_events();
    REQUIRE(events[1].sample == events[2].sample);
    REQUIRE(events[1].tick == TickPosition{2 * kTicksPerQuarter - 1});
    REQUIRE(events[1].kind == NoteProgramEventKind::On);
    REQUIRE(events[2].tick == TickPosition{2 * kTicksPerQuarter});
    REQUIRE(events[2].kind == NoteProgramEventKind::Off);

    sequence::SequenceProcessorConfig processor_config;
    processor_config.output_channels = 1;
    sequence::SequenceProcessor embedded(programs.store, processor_config);
    state::StateStore state;
    embedded.define_parameters(state);
    embedded.prepare({
        .sample_rate = 48'000.0,
        .max_buffer_size = 48'000,
        .input_channels = 0,
        .output_channels = 1,
    });
    REQUIRE(embedded.ready());

    Buffer silence(1, 48'000);
    Buffer audio_output(1, 48'000);
    midi::MidiBuffer input;
    midi::MidiBuffer output;
    output.reserve(16);
    output.set_realtime_capacity_limit(true);
    UmpOutputAttachment output_ump(output, 16);
    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 48'000;
    context.is_playing = true;
    context.tempo_bpm = 60.0;
    context.position_beats = 1.0;
    context.position_samples = 48'000;
    context.transport_validity.set(format::TransportField::BeatPosition);
    context.transport_validity.set(format::TransportField::Tempo);
    context.transport_validity.set(format::TransportField::SamplePosition);
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 48'000) == sequence::HostTransportProjectionError::None);
    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.ranges[0].timeline_tick_end == TickPosition{2 * kTicksPerQuarter});
    std::uint32_t boundary_offset = 0;
    REQUIRE(host_mapped_output_offset_for_tick(
        projected.ranges[0], TickPosition{2 * kTicksPerQuarter - 1}, boundary_offset));
    REQUIRE(boundary_offset == 47'999);
    auto audio_output_view = audio_output.view();
    const auto silence_view = silence.const_view();
    embedded.process(audio_output_view, silence_view, input, output, context);

    REQUIRE(embedded.ready());
    REQUIRE(output.size() == 2);
    REQUIRE(output[0].is_note_on());
    REQUIRE(output[0].data()[1] == 60);
    REQUIRE(output[0].sample_offset == 0);
    REQUIRE(output[1].is_note_on());
    REQUIRE(output[1].data()[1] == 61);
    REQUIRE(output[1].sample_offset == 47'999);
}

TEST_CASE("embedded sequence processor sizes routed MIDI for dense multiple tracks") {
    constexpr std::size_t notes_per_track = 600;
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(dense_multitrack_note_project(notes_per_track), map,
                     take(DecodedAudioAssetPool::create({})), 1);

    sequence::SequenceProcessorConfig processor_config;
    processor_config.output_channels = 1;
    processor_config.maximum_note_events_per_track_per_block = notes_per_track;
    sequence::SequenceProcessor embedded(programs.store, processor_config);
    state::StateStore state;
    embedded.define_parameters(state);
    embedded.prepare({
        .sample_rate = 48'000.0,
        .max_buffer_size = 32,
        .input_channels = 0,
        .output_channels = 1,
    });
    REQUIRE(embedded.ready());

    Buffer silence(1, 32);
    Buffer audio_output(1, 32);
    midi::MidiBuffer input;
    midi::MidiBuffer output;
    output.reserve(2 * notes_per_track);
    output.set_realtime_capacity_limit(true);
    UmpOutputAttachment output_ump(output, 2 * notes_per_track);
    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 32;
    context.is_playing = true;
    context.position_samples = 0;
    auto audio_output_view = audio_output.view();
    embedded.process(audio_output_view, silence.const_view(), input, output, context);

    REQUIRE(embedded.ready());
    REQUIRE(embedded.status() == sequence::SequenceProcessorStatus::Ready);
    REQUIRE(output.size() == 2 * notes_per_track);
}

TEST_CASE("embedded sequence processor matches offline and desktop event streams "
          "across a loop wrap") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(note_project(*map), map, take(DecodedAudioAssetPool::create({})), 1);
    auto program = programs.store.read();
    REQUIRE(program);

    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    auto counter = std::make_unique<MidiCountingSlot>();
    auto* counter_ptr = counter.get();
    const auto midi_destination =
        graph.add_plugin_node(std::move(counter), 1, 1, "sequence parity recorder");
    REQUIRE(graph.prepare(48'000.0, 32));
    TimelineGraphPlaybackBinding desktop(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, midi_destination}};
    REQUIRE(desktop.prepare(*program, routes, config(1), 48'000.0, 32));

    sequence::SequenceProcessorConfig processor_config;
    processor_config.output_channels = 1;
    sequence::SequenceProcessor embedded(programs.store, processor_config);
    state::StateStore state;
    embedded.define_parameters(state);
    embedded.prepare({
        .sample_rate = 48'000.0,
        .max_buffer_size = 32,
        .input_channels = 0,
        .output_channels = 1,
    });
    REQUIRE(embedded.ready());

    ArrangementNoteRenderer offline_renderer({10});
    REQUIRE(offline_renderer.prepare(256));
    PlaybackProgramBlock offline_program(program.get());

    Buffer silence(1, 32);
    midi::MidiBuffer midi_in;
    midi::MidiBuffer embedded_midi;
    embedded_midi.reserve(256);
    embedded_midi.set_realtime_capacity_limit(true);
    UmpOutputAttachment embedded_ump(embedded_midi, 256);

    auto seeked = snapshot(*program, 32, 0);
    seeked.block_index = 2;
    seeked.reset_requested = true;
    seeked.ranges[0].discontinuity = true;
    const std::array blocks{
        loop_block(*program, 0, 48, 32),
        loop_block(*program, 1, 16, 32),
        seeked,
    };
    for (const auto& transport : blocks) {
        const auto offline_result = offline_renderer.process(offline_program, transport);
        REQUIRE(offline_result.code == NoteRenderCode::Ok);

        Buffer desktop_output(1, 32);
        auto desktop_view = desktop_output.view();
        REQUIRE(desktop.process(desktop_view, silence.const_view(), transport));

        Buffer embedded_output(1, 32, 1.0f);
        auto embedded_view = embedded_output.view();
        auto context = host_context(transport);
        std::size_t allocations = 0;
        {
            test::ScopedRtProcessProbe probe;
            embedded.process(embedded_view, silence.const_view(), midi_in, embedded_midi, context);
            allocations = probe.allocation_count();
        }
        REQUIRE(allocations == 0);
        REQUIRE(embedded.ready());

        require_same_midi(offline_renderer.events(), embedded_midi);
        REQUIRE(counter_ptr->last_event_count == offline_renderer.events().size());
        for (std::size_t index = 0; index < offline_renderer.events().size(); ++index) {
            REQUIRE(counter_ptr->last_offsets[index] ==
                    static_cast<std::uint32_t>(offline_renderer.events()[index].sample_offset));
        }
        REQUIRE(std::all_of(embedded_output.storage[0].begin(), embedded_output.storage[0].end(),
                            [](float sample) { return sample == 0.0f; }));
    }
}

TEST_CASE("embedded sequence processor never publishes a partial MIDI block") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(note_project(*map), map, take(DecodedAudioAssetPool::create({})), 1);
    auto program = programs.store.read();
    REQUIRE(program);

    sequence::SequenceProcessorConfig processor_config;
    processor_config.output_channels = 1;
    sequence::SequenceProcessor embedded(programs.store, processor_config);
    state::StateStore state;
    embedded.define_parameters(state);
    embedded.prepare({
        .sample_rate = 48'000.0,
        .max_buffer_size = 32,
        .input_channels = 0,
        .output_channels = 1,
    });
    REQUIRE(embedded.ready());

    Buffer silence(1, 32);
    Buffer output(1, 32);
    auto output_view = output.view();
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    midi_out.reserve(1);
    midi_out.set_realtime_capacity_limit(true);
    UmpOutputAttachment midi_ump(midi_out, 1);
    const auto transport = snapshot(*program, 32, 0);
    auto context = host_context(transport);

    std::size_t allocations = 0;
    {
        test::ScopedRtProcessProbe probe;
        embedded.process(output_view, silence.const_view(), midi_in, midi_out, context);
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
    REQUIRE(embedded.status() == sequence::SequenceProcessorStatus::RenderFailed);
    REQUIRE(midi_out.empty());
}

TEST_CASE("embedded sequence processor atomically adopts prepared same-topology generations") {
    const auto map = tempo_map();
    const auto assets = take(DecodedAudioAssetPool::create({}));
    ProgramHarness programs;
    programs.publish(single_note_project(60), map, assets, 1);

    sequence::SequenceProcessorConfig processor_config;
    processor_config.output_channels = 1;
    sequence::SequenceProcessor embedded(programs.store, processor_config);
    embedded.prepare({
        .sample_rate = 48'000.0,
        .max_buffer_size = 32,
        .input_channels = 0,
        .output_channels = 1,
    });
    REQUIRE(embedded.ready());

    Buffer silence(1, 32);
    Buffer audio_output(1, 32);
    auto audio_view = audio_output.view();
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    midi_out.reserve(16);
    midi_out.set_realtime_capacity_limit(true);
    UmpOutputAttachment midi_ump(midi_out, 16);

    programs.publish(single_note_project(61), map, assets, 2);
    REQUIRE(embedded.adopt_latest_program());
    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 32;
    context.is_playing = true;
    context.position_samples = 0;
    context.transport_jump = true;
    embedded.process(audio_view, silence.const_view(), midi_in, midi_out, context);
    REQUIRE(embedded.status() == sequence::SequenceProcessorStatus::Ready);
    REQUIRE(midi_out.size() == 1);
    REQUIRE(midi_out[0].is_note_on());
    REQUIRE(midi_out[0].note() == 61);

    // A later store publication is inert until its exact prepared runtime pair
    // is adopted; the callback keeps rendering its pinned generation instead
    // of permanently rejecting every subsequent block.
    programs.publish(single_note_project(62), map, assets, 3);
    context.transport_jump = true;
    embedded.process(audio_view, silence.const_view(), midi_in, midi_out, context);
    REQUIRE(embedded.status() == sequence::SequenceProcessorStatus::Ready);
    REQUIRE(std::count_if(midi_out.begin(), midi_out.end(), [](const auto& event) {
                return event.is_note_on() && event.note() == 61;
            }) == 1);
}

TEST_CASE("embedded sequence processor aligns MIDI with Stretch-declared audio latency") {
    constexpr std::uint64_t source_frames = 24'000;
    std::vector<float> impulse(source_frames, 0.0f);
    impulse[0] = 1.0f;
    std::vector<float> stretch(source_frames, 0.05f);
    const auto impulse_hash = ContentHash::from_hex(std::string(64, 'a'));
    const auto stretch_hash = ContentHash::from_hex(std::string(64, 'b'));
    REQUIRE(impulse_hash);
    REQUIRE(stretch_hash);
    const auto assets = take(DecodedAudioAssetPool::create(
        {DecodedAudioAsset{{3}, audio_data(std::move(impulse)), *impulse_hash, {}},
         DecodedAudioAsset{{4}, audio_data(std::move(stretch)), *stretch_hash, {}}}));
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(stretch_latency_alignment_project(source_frames), map, assets, 1);

    sequence::SequenceProcessorConfig processor_config;
    processor_config.output_channels = 1;
    sequence::SequenceProcessor embedded(programs.store, processor_config);
    embedded.prepare({
        .sample_rate = 48'000.0,
        .max_buffer_size = 128,
        .input_channels = 0,
        .output_channels = 1,
    });
    REQUIRE(embedded.ready());
    const auto latency = embedded.latency_samples();
    REQUIRE(latency > 0);

    // A poisoned same-generation runtime is replaceable from the still-live
    // store publication. The exact publication pointer changes, so the queued
    // pre-failure note cannot leak from the discarded runtime generation.
    {
        sequence::SequenceProcessor recovery(programs.store, processor_config);
        recovery.prepare({
            .sample_rate = 48'000.0,
            .max_buffer_size = 128,
            .input_channels = 0,
            .output_channels = 1,
        });
        REQUIRE(recovery.ready());
        Buffer recovery_input(1, 128);
        Buffer recovery_output(1, 128);
        auto recovery_view = recovery_output.view();
        midi::MidiBuffer recovery_midi_in;
        midi::MidiBuffer recovery_midi_out;
        recovery_midi_out.reserve(16);
        recovery_midi_out.set_realtime_capacity_limit(true);
        UmpOutputAttachment recovery_ump(recovery_midi_out, 16);
        const auto recovery_context = [](std::int64_t position) {
            format::ProcessContext context;
            context.sample_rate = 48'000.0;
            context.num_samples = 128;
            context.is_playing = true;
            context.position_samples = position;
            context.position_beats = static_cast<double>(position) / 24'000.0;
            context.tempo_bpm = 120.0;
            context.transport_validity.set(format::TransportField::BeatPosition);
            context.transport_validity.set(format::TransportField::Tempo);
            context.transport_validity.set(format::TransportField::SamplePosition);
            return context;
        };
        auto first = recovery_context(0);
        recovery.process(recovery_view, recovery_input.const_view(), recovery_midi_in,
                         recovery_midi_out, first);
        REQUIRE(recovery.status() == sequence::SequenceProcessorStatus::Ready);
        REQUIRE(recovery_midi_out.empty());
        recovery.force_realtime_stretch_failure_for_test();
        auto rejected = recovery_context(128);
        recovery.process(recovery_view, recovery_input.const_view(), recovery_midi_in,
                         recovery_midi_out, rejected);
        REQUIRE(recovery.status() == sequence::SequenceProcessorStatus::RenderFailed);
        REQUIRE(recovery_midi_out.empty());
        REQUIRE(recovery.adopt_latest_program());
        auto resumed = recovery_context(256);
        recovery.process(recovery_view, recovery_input.const_view(), recovery_midi_in,
                         recovery_midi_out, resumed);
        REQUIRE(recovery.status() == sequence::SequenceProcessorStatus::Ready);
        REQUIRE(recovery_midi_out.empty());
    }

    {
        sequence::SequenceProcessor missing_sidecar(programs.store, processor_config);
        missing_sidecar.prepare({
            .sample_rate = 48'000.0,
            .max_buffer_size = 128,
            .input_channels = 0,
            .output_channels = 1,
        });
        REQUIRE(missing_sidecar.ready());
        Buffer input(1, 128);
        Buffer output(1, 128);
        auto output_view = output.view();
        midi::MidiBuffer midi_in;
        midi::MidiBuffer midi_out_without_ump;
        midi_out_without_ump.reserve(16);
        midi_out_without_ump.set_realtime_capacity_limit(true);
        format::ProcessContext context;
        context.sample_rate = 48'000.0;
        context.num_samples = 128;
        context.is_playing = true;
        context.position_samples = 0;
        context.position_beats = 0.0;
        context.tempo_bpm = 120.0;
        context.transport_validity.set(format::TransportField::BeatPosition);
        context.transport_validity.set(format::TransportField::Tempo);
        context.transport_validity.set(format::TransportField::SamplePosition);
        missing_sidecar.process(output_view, input.const_view(), midi_in, midi_out_without_ump,
                                context);
        REQUIRE(missing_sidecar.status() == sequence::SequenceProcessorStatus::RenderFailed);
        REQUIRE(midi_out_without_ump.empty());
    }

    {
        sequence::SequenceProcessor stopped(programs.store, processor_config);
        stopped.prepare({
            .sample_rate = 48'000.0,
            .max_buffer_size = 128,
            .input_channels = 0,
            .output_channels = 1,
        });
        REQUIRE(stopped.ready());
        Buffer input(1, 128);
        midi::MidiBuffer midi_in;
        midi::MidiBuffer midi_out;
        midi_out.reserve(16);
        midi_out.set_realtime_capacity_limit(true);
        UmpOutputAttachment ump_out(midi_out, 16);
        const auto context_at = [](std::int64_t position, bool playing) {
            format::ProcessContext context;
            context.sample_rate = 48'000.0;
            context.num_samples = 128;
            context.is_playing = playing;
            context.position_samples = position;
            context.position_beats = static_cast<double>(position) / 24'000.0;
            context.tempo_bpm = 120.0;
            context.transport_validity.set(format::TransportField::BeatPosition);
            context.transport_validity.set(format::TransportField::Tempo);
            context.transport_validity.set(format::TransportField::SamplePosition);
            return context;
        };
        const auto warm_blocks = (static_cast<std::uint64_t>(latency) + 255u) / 128u + 2u;
        bool heard_audio = false;
        for (std::uint64_t block = 0; block < warm_blocks; ++block) {
            Buffer output(1, 128);
            auto output_view = output.view();
            auto context = context_at(static_cast<std::int64_t>(block * 128u), true);
            stopped.process(output_view, input.const_view(), midi_in, midi_out, context);
            REQUIRE(stopped.status() == sequence::SequenceProcessorStatus::Ready);
            heard_audio =
                heard_audio || std::any_of(output.storage[0].begin(), output.storage[0].end(),
                                           [](float sample) { return std::abs(sample) > 1.0e-6f; });
        }
        REQUIRE(heard_audio);

        Buffer stopped_output(1, 128);
        auto stopped_view = stopped_output.view();
        auto stop_context = context_at(static_cast<std::int64_t>(warm_blocks * 128u), false);
        stopped.process(stopped_view, input.const_view(), midi_in, midi_out, stop_context);
        REQUIRE(stopped.status() == sequence::SequenceProcessorStatus::Ready);
        REQUIRE(std::all_of(stopped_output.storage[0].begin(), stopped_output.storage[0].end(),
                            [](float sample) { return sample == 0.0f; }));
        REQUIRE(std::count_if(midi_out.begin(), midi_out.end(), [](const auto& event) {
                    return event.is_note_off() && event.sample_offset == 0;
                }) == 1);
        REQUIRE(std::none_of(midi_out.begin(), midi_out.end(),
                             [](const auto& event) { return event.is_note_on(); }));
        REQUIRE(std::count_if(ump_out.ump().begin(), ump_out.ump().end(), [](const auto& event) {
                    return (event.packet.status() & 0xf0u) == 0x80u && event.sample_offset == 0;
                }) == 1);

        // Restart must be indistinguishable from a fresh processor at the same
        // transport jump through the complete block containing the exact
        // latency boundary, not merely floor(latency / block) silent blocks.
        sequence::SequenceProcessor fresh(programs.store, processor_config);
        fresh.prepare({
            .sample_rate = 48'000.0,
            .max_buffer_size = 128,
            .input_channels = 0,
            .output_channels = 1,
        });
        REQUIRE(fresh.ready());
        midi::MidiBuffer fresh_midi_out;
        fresh_midi_out.reserve(16);
        fresh_midi_out.set_realtime_capacity_limit(true);
        UmpOutputAttachment fresh_ump(fresh_midi_out, 16);
        std::optional<std::uint64_t> restarted_audio_sample;
        std::optional<std::uint64_t> fresh_audio_sample;
        std::optional<std::uint64_t> restarted_note_sample;
        std::optional<std::uint64_t> fresh_note_sample;
        const auto restart_blocks = (static_cast<std::uint64_t>(latency) + 255u) / 128u;
        for (std::uint64_t block = 0; block < restart_blocks; ++block) {
            Buffer output(1, 128);
            Buffer fresh_output(1, 128);
            auto output_view = output.view();
            auto fresh_view = fresh_output.view();
            auto context = context_at(static_cast<std::int64_t>(block * 128u), true);
            context.transport_jump = block == 0;
            stopped.process(output_view, input.const_view(), midi_in, midi_out, context);
            fresh.process(fresh_view, input.const_view(), midi_in, fresh_midi_out, context);
            REQUIRE(stopped.status() == sequence::SequenceProcessorStatus::Ready);
            REQUIRE(fresh.status() == sequence::SequenceProcessorStatus::Ready);
            REQUIRE(output.storage == fresh_output.storage);
            REQUIRE(midi_out.size() == fresh_midi_out.size());
            REQUIRE(ump_out.ump().size() == fresh_ump.ump().size());
            for (std::size_t frame = 0; frame < 128; ++frame) {
                if (!restarted_audio_sample && std::abs(output.storage[0][frame]) > 0.5f)
                    restarted_audio_sample = block * 128u + frame;
                if (!fresh_audio_sample && std::abs(fresh_output.storage[0][frame]) > 0.5f)
                    fresh_audio_sample = block * 128u + frame;
            }
            for (const auto& event : midi_out)
                if (!restarted_note_sample && event.is_note_on())
                    restarted_note_sample = block * 128u + event.sample_offset;
            for (const auto& event : fresh_midi_out)
                if (!fresh_note_sample && event.is_note_on())
                    fresh_note_sample = block * 128u + event.sample_offset;
        }
        REQUIRE(restarted_audio_sample == fresh_audio_sample);
        REQUIRE(restarted_note_sample == fresh_note_sample);
        REQUIRE(restarted_audio_sample == std::optional<std::uint64_t>{latency});
        REQUIRE(restarted_note_sample == restarted_audio_sample);
    }

    Buffer silence(1, 128);
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    midi_out.reserve(16);
    midi_out.set_realtime_capacity_limit(true);
    UmpOutputAttachment midi_ump(midi_out, 16);
    std::optional<std::uint64_t> impulse_sample;
    std::optional<std::uint64_t> note_sample;
    std::optional<std::uint64_t> ump_note_sample;
    const auto block_count = (static_cast<std::uint64_t>(latency) + 255u) / 128u;
    for (std::uint64_t block = 0; block < block_count; ++block) {
        Buffer audio_output(1, 128);
        auto audio_view = audio_output.view();
        format::ProcessContext context;
        context.sample_rate = 48'000.0;
        context.num_samples = 128;
        context.is_playing = true;
        context.position_samples = static_cast<std::int64_t>(block * 128u);
        context.position_beats = static_cast<double>(context.position_samples) / 24'000.0;
        context.tempo_bpm = 120.0;
        context.transport_validity.set(format::TransportField::BeatPosition);
        context.transport_validity.set(format::TransportField::Tempo);
        context.transport_validity.set(format::TransportField::SamplePosition);

        std::size_t allocations = 0;
        {
            test::ScopedRtProcessProbe probe;
            embedded.process(audio_view, silence.const_view(), midi_in, midi_out, context);
            allocations = probe.allocation_count();
        }
        REQUIRE(allocations == 0);
        INFO("block " << block << " processor status " << static_cast<int>(embedded.status())
                      << " audio status "
                      << static_cast<int>(embedded.last_observation().audio_status));
        REQUIRE(embedded.status() == sequence::SequenceProcessorStatus::Ready);
        for (std::size_t frame = 0; frame < 128; ++frame) {
            if (!impulse_sample && std::abs(audio_output.storage[0][frame]) > 0.5f)
                impulse_sample = block * 128u + frame;
        }
        for (const auto& event : midi_out) {
            if (!note_sample && event.is_note_on())
                note_sample = block * 128u + static_cast<std::uint32_t>(event.sample_offset);
        }
        for (const auto& event : midi_ump.ump()) {
            if (!ump_note_sample)
                ump_note_sample = block * 128u + static_cast<std::uint32_t>(event.sample_offset);
        }
    }

    REQUIRE(impulse_sample);
    REQUIRE(note_sample);
    REQUIRE(ump_note_sample);
    REQUIRE(*impulse_sample == static_cast<std::uint64_t>(latency));
    REQUIRE(*note_sample == *impulse_sample);
    REQUIRE(*ump_note_sample == *impulse_sample);
}

TEST_CASE("embedded sequence processor treats zero-frame callbacks as recoverable no-ops") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(note_project(*map), map, take(DecodedAudioAssetPool::create({})), 1);
    auto program = programs.store.read();
    REQUIRE(program);

    sequence::SequenceProcessorConfig processor_config;
    processor_config.output_channels = 1;
    sequence::SequenceProcessor embedded(programs.store, processor_config);
    state::StateStore state;
    embedded.define_parameters(state);
    embedded.prepare({
        .sample_rate = 48'000.0,
        .max_buffer_size = 32,
        .input_channels = 0,
        .output_channels = 1,
    });
    REQUIRE(embedded.ready());

    Buffer zero_output(1, 0);
    Buffer zero_input(1, 0);
    auto zero_output_view = zero_output.view();
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    midi_out.reserve(256);
    midi_out.set_realtime_capacity_limit(true);
    UmpOutputAttachment midi_ump(midi_out, 256);
    REQUIRE(midi_out.add(midi::MidiEvent::note_on(0, 60, 100)));
    auto zero_context = host_context(snapshot(*program, 32, 0));
    zero_context.num_samples = 0;
    const auto observation = embedded.last_observation();
    embedded.process(zero_output_view, zero_input.const_view(), midi_in, midi_out, zero_context);
    REQUIRE(embedded.ready());
    REQUIRE(embedded.status() == sequence::SequenceProcessorStatus::Ready);
    REQUIRE(embedded.last_observation().valid == observation.valid);
    REQUIRE(midi_out.empty());

    Buffer output(1, 32);
    Buffer input(1, 32);
    auto output_view = output.view();
    auto context = host_context(snapshot(*program, 32, 0));
    context.num_samples = 31;
    embedded.process(output_view, input.const_view(), midi_in, midi_out, context);
    REQUIRE(embedded.ready());
    REQUIRE(embedded.status() == sequence::SequenceProcessorStatus::ExecutorFailed);

    embedded.process(zero_output_view, zero_input.const_view(), midi_in, midi_out, zero_context);
    REQUIRE(embedded.ready());
    REQUIRE(embedded.status() == sequence::SequenceProcessorStatus::ExecutorFailed);

    context.num_samples = 32;
    embedded.process(output_view, input.const_view(), midi_in, midi_out, context);
    REQUIRE(embedded.ready());
    REQUIRE(embedded.status() == sequence::SequenceProcessorStatus::Ready);
}
