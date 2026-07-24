#include "support/timeline_graph_binding_test_support.hpp"

#include <pulp/format/playback_context_projection.hpp>
#include <pulp/sequence/sequence_processor.hpp>

#include <catch2/catch_test_macros.hpp>

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
    REQUIRE_FALSE(projected.ranges[0].discontinuity);
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
