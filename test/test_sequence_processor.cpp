#include "support/timeline_graph_binding_test_support.hpp"

#include <pulp/format/playback_context_projection.hpp>
#include <pulp/sequence/sequence_processor.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

namespace {

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

} // namespace

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
