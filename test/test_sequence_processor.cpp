#include "support/timeline_graph_binding_test_support.hpp"

#include <pulp/format/playback_context_projection.hpp>
#include <pulp/sequence/sequence_processor.hpp>

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

} // namespace

TEST_CASE("host transport projection fails closed for unsupported loop density "
          "and saturates at the sample-domain boundary") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 32) == sequence::HostTransportProjectionError::None);

    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 32;
    context.is_playing = true;
    context.is_looping = true;
    context.loop_end_beats = 0.0005;
    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) ==
            sequence::HostTransportProjectionError::LoopTooShortForBlock);

    context.is_looping = false;
    context.position_samples = std::numeric_limits<std::int64_t>::max() - 4;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.range_count == 1);

    context.position_samples = std::numeric_limits<std::int64_t>::max();
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE_FALSE(projected.reset_requested);
}

TEST_CASE("host transport projection preserves authoritative positions outside an active loop") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 32) == sequence::HostTransportProjectionError::None);

    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 32;
    context.is_playing = true;
    context.is_looping = true;
    context.loop_start_beats = 0.0;
    context.loop_end_beats = 64.0 / 24'000.0;
    context.position_samples = 128;
    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.range_count == 1);
    REQUIRE(projected.ranges[0].timeline_sample_start.value == 128);

    context.position_samples = 160;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.range_count == 1);
    REQUIRE(projected.ranges[0].timeline_sample_start.value == 160);
    REQUIRE_FALSE(projected.reset_requested);
    REQUIRE_FALSE(projected.ranges[0].discontinuity);
}

TEST_CASE("host transport projection re-anchors after an exact second loop wrap") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 96) == sequence::HostTransportProjectionError::None);

    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 96;
    context.is_playing = true;
    context.is_looping = true;
    context.loop_start_beats = 0.0;
    context.loop_end_beats = 64.0 / 24'000.0;
    context.position_samples = 32;
    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.range_count == 2);
    REQUIRE(projected.ranges[1].timeline_sample_start.value == 0);
    REQUIRE(projected.ranges[1].frame_count == 64);

    context.num_samples = 32;
    context.position_samples = 0;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE_FALSE(projected.reset_requested);
    REQUIRE(projected.ranges[0].discontinuity);
}

TEST_CASE("host transport projection anchors blocks and seeks to a valid host beat clock") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 24'000) == sequence::HostTransportProjectionError::None);

    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 24'000;
    context.is_playing = true;
    context.tempo_bpm = 60.0;
    context.position_beats = 2.0;
    context.position_samples = 96'000;
    context.transport_validity.set(format::TransportField::BeatPosition);
    context.transport_validity.set(format::TransportField::Tempo);
    context.transport_validity.set(format::TransportField::SamplePosition);

    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.ranges[0].timeline_tick_start.value == 2 * kTicksPerQuarter);
    REQUIRE(projected.ranges[0].timeline_tick_end.value == 5 * kTicksPerQuarter / 2);
    REQUIRE(projected.ranges[0].tempo_bpm == 60.0);

    context.position_beats = 2.5;
    context.position_samples = 120'000;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.ranges[0].timeline_tick_start.value == 5 * kTicksPerQuarter / 2);
    REQUIRE(projected.ranges[0].timeline_tick_end.value == 3 * kTicksPerQuarter);
    REQUIRE_FALSE(projected.reset_requested);

    context.position_beats = 1.0;
    context.position_samples = 48'000;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.ranges[0].timeline_tick_start.value == kTicksPerQuarter);
    REQUIRE(projected.reset_requested);
    REQUIRE(projected.ranges[0].discontinuity);
}

TEST_CASE("host beat projection preserves fractional tick phase across small blocks") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 1) == sequence::HostTransportProjectionError::None);

    constexpr double sample_rate = 48'000.0;
    constexpr double tempo_bpm = 123.0;
    constexpr double start_beat = 10.0;
    format::ProcessContext context;
    context.sample_rate = sample_rate;
    context.num_samples = 1;
    context.is_playing = true;
    context.tempo_bpm = tempo_bpm;
    context.transport_validity.set(format::TransportField::BeatPosition);
    context.transport_validity.set(format::TransportField::Tempo);
    context.transport_validity.set(format::TransportField::SamplePosition);

    TickPosition previous_end;
    for (std::int64_t frame = 0; frame < 64; ++frame) {
        context.position_samples = frame;
        context.position_beats =
            start_beat + static_cast<double>(frame) * tempo_bpm / (60.0 * sample_rate);
        TransportSnapshot projected;
        REQUIRE(projector.project(context, projected) ==
                sequence::HostTransportProjectionError::None);
        if (frame != 0)
            REQUIRE(projected.ranges[0].timeline_tick_start == previous_end);
        previous_end = projected.ranges[0].timeline_tick_end;
    }
    REQUIRE(previous_end.value > static_cast<std::int64_t>(start_beat * kTicksPerQuarter));
}

TEST_CASE("host beat-domain loop projection splits and resumes at host tempo") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 16'000) == sequence::HostTransportProjectionError::None);

    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 16'000;
    context.is_playing = true;
    context.is_looping = true;
    context.tempo_bpm = 60.0;
    context.position_beats = 1.75;
    // Deliberately use a sample/beat origin that cannot be derived by assuming
    // the current host tempo has applied since beat zero.
    context.position_samples = 100'000;
    context.loop_start_beats = 1.0;
    context.loop_end_beats = 2.0;
    context.transport_validity.set(format::TransportField::BeatPosition);
    context.transport_validity.set(format::TransportField::Tempo);
    context.transport_validity.set(format::TransportField::SamplePosition);
    context.transport_validity.set(format::TransportField::LoopRange);

    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.range_count == 2);
    REQUIRE(projected.ranges[0].frame_count == 12'000);
    REQUIRE(projected.ranges[0].timeline_tick_start.value == 7 * kTicksPerQuarter / 4);
    REQUIRE(projected.ranges[0].timeline_tick_end.value == 2 * kTicksPerQuarter);
    REQUIRE(projected.ranges[1].sample_offset == 12'000);
    REQUIRE(projected.ranges[1].frame_count == 4'000);
    REQUIRE(projected.ranges[1].timeline_sample_start.value == 64'000);
    REQUIRE(projected.ranges[1].timeline_tick_start.value == kTicksPerQuarter);
    REQUIRE(projected.ranges[1].timeline_tick_end.value ==
            kTicksPerQuarter + kTicksPerQuarter / 12);
    REQUIRE(projected.ranges[1].discontinuity);

    context.num_samples = 1'000;
    context.position_beats = 1.0 + 4'000.0 / 48'000.0;
    context.position_samples = 68'000;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE_FALSE(projected.reset_requested);
    REQUIRE(projected.ranges[0].timeline_tick_start.value ==
            kTicksPerQuarter + kTicksPerQuarter / 12);
}

TEST_CASE("host beat-domain loop projection keeps the final fractional frame cell") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 15) == sequence::HostTransportProjectionError::None);

    constexpr double sample_rate = 48'000.0;
    constexpr double loop_start_beats = 1.0;
    const double loop_end_beats = loop_start_beats + 100.4 / sample_rate;
    format::ProcessContext context;
    context.sample_rate = sample_rate;
    context.num_samples = 15;
    context.is_playing = true;
    context.is_looping = true;
    context.tempo_bpm = 60.0;
    context.position_beats = loop_end_beats - 10.4 / sample_rate;
    context.position_samples = 100'000;
    context.loop_start_beats = loop_start_beats;
    context.loop_end_beats = loop_end_beats;
    context.transport_validity.set(format::TransportField::BeatPosition);
    context.transport_validity.set(format::TransportField::Tempo);
    context.transport_validity.set(format::TransportField::SamplePosition);
    context.transport_validity.set(format::TransportField::LoopRange);

    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.range_count == 2);
    REQUIRE(projected.ranges[0].frame_count == 11);
    REQUIRE(projected.ranges[0].timeline_tick_end == projected.loop.end);
    REQUIRE(projected.ranges[1].sample_offset == 11);
    REQUIRE(projected.ranges[1].frame_count == 4);
    REQUIRE(projected.ranges[1].timeline_sample_start.value == 99'910);
    REQUIRE(projected.ranges[1].timeline_tick_start == projected.loop.start);

    context.num_samples = 8;
    context.position_beats = loop_start_beats + 4.0 / sample_rate;
    context.position_samples = 99'914;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE_FALSE(projected.reset_requested);
    REQUIRE_FALSE(projected.ranges[0].discontinuity);
    REQUIRE(projected.range_count == 1);
    REQUIRE(projected.ranges[0].timeline_sample_start.value == 99'914);
    REQUIRE(projected.ranges[0].timeline_tick_start ==
            TickPosition{static_cast<std::int64_t>(
                std::llround(context.position_beats * static_cast<double>(kTicksPerQuarter)))});
}

TEST_CASE("host beat-domain projection falls back when beat clock validity is absent or invalid") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 32) == sequence::HostTransportProjectionError::None);

    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 32;
    context.is_playing = true;
    context.position_samples = 24'000;
    context.position_beats = 2.0;
    context.tempo_bpm = 60.0;

    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.ranges[0].timeline_tick_start ==
            map->samples_to_ticks({context.position_samples}));
    REQUIRE(projected.ranges[0].tempo_bpm == 120.0);

    context.position_samples = 24'032;
    context.transport_validity.set(format::TransportField::BeatPosition);
    context.transport_validity.set(format::TransportField::Tempo);
    context.tempo_bpm = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.ranges[0].timeline_tick_start ==
            map->samples_to_ticks({context.position_samples}));
    REQUIRE(projected.ranges[0].tempo_bpm == 120.0);

    context.position_samples = 24'064;
    context.position_beats = std::numeric_limits<double>::max();
    context.tempo_bpm = 60.0;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.ranges[0].timeline_tick_start ==
            map->samples_to_ticks({context.position_samples}));
    REQUIRE(projected.ranges[0].tempo_bpm == 120.0);

    context.position_samples = 24'096;
    context.position_beats = 1.0;
    context.tempo_bpm = 60.0;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE_FALSE(projected.ranges[0].host_beat_mapping);
    REQUIRE(projected.ranges[0].timeline_tick_start ==
            map->samples_to_ticks({context.position_samples}));

    context.position_samples = 24'128;
    context.transport_validity.set(format::TransportField::SamplePosition);
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.ranges[0].host_beat_mapping);
    REQUIRE(projected.reset_requested);
    REQUIRE(projected.ranges[0].discontinuity);
    REQUIRE(projected.ranges[0].timeline_tick_start.value == kTicksPerQuarter);
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

TEST_CASE("host-mapped note scheduling scans same-sample events through the exact tick end") {
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
    REQUIRE(events[1].tick == TickPosition{2 * kTicksPerQuarter});
    REQUIRE(events[2].tick == TickPosition{2 * kTicksPerQuarter - 1});

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
