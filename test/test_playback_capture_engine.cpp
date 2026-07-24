#include "harness/scoped_rt_process_probe.hpp"
#include "timebase_test_helpers.hpp"

#include <pulp/playback/capture_engine.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;

namespace {

CompiledTempoMap constant_map() {
    const std::array points{TempoPoint{{0}, 120.0}};
    return require_compiled_tempo_map(points, RationalRate{48'000, 1});
}

CaptureEngineConfig capture_config(std::uint32_t block_size = 32, std::uint32_t slots = 4) {
    CaptureEngineConfig config;
    config.sample_rate = {48'000, 1};
    config.maximum_block_size = block_size;
    config.maximum_take_frames = 128;
    config.take_slots_per_track = slots;
    config.midi_events_per_take = 16;
    config.tracks.push_back({
        .track_id = {10},
        .take_lane_id = {20},
        .input_channel = 0,
        .output_channel = 0,
        .channel_count = 1,
        .armed = true,
        .monitor = true,
        .capture_midi = true,
    });
    return config;
}

void prepare_transport(MasterTransport& transport, const CompiledTempoMap& map,
                       std::uint32_t block_size, LoopRegion loop = {}) {
    MasterTransportConfig config;
    config.max_buffer_size = block_size;
    config.loop = loop;
    config.initially_playing = true;
    REQUIRE(transport.prepare(map, config) == TransportError::None);
}

TransportSnapshot next_block(MasterTransport& transport, std::uint32_t frames) {
    TransportSnapshot snapshot;
    REQUIRE(transport.begin_block(frames, snapshot) == TransportError::None);
    return snapshot;
}

void fill_absolute(audio::Buffer<float>& buffer, std::int64_t start) {
    for (std::size_t frame = 0; frame < buffer.num_samples(); ++frame)
        buffer.channel(0)[frame] = static_cast<float>(start + static_cast<std::int64_t>(frame));
}

audio::BufferView<const float> read_view(const audio::Buffer<float>& buffer) {
    return buffer.view();
}

std::vector<CaptureEvent> drain(CaptureEngine& engine) {
    std::vector<CaptureEvent> result;
    CaptureEvent event;
    while (engine.pop_event(event))
        result.push_back(event);
    return result;
}

std::vector<float> render_metronome_snapshot(const CompiledTempoMap& map,
                                             TransportSnapshot transport, CaptureSession session) {
    auto config = capture_config(transport.frame_count);
    config.maximum_take_frames = transport.frame_count;
    config.tracks[0].monitor = false;
    CaptureEngine engine;
    REQUIRE(engine.prepare(std::move(config)));

    session.metronome_enabled = true;
    session.metronome_level = 0.5f;
    CaptureCommand start;
    start.type = CaptureCommandType::Start;
    start.sequence = 10;
    start.session = session;
    REQUIRE(engine.enqueue_command(start));

    transport.tempo_map = &map;
    transport.sample_rate = map.sample_rate();
    transport.is_playing = true;
    audio::Buffer<float> input(1, transport.frame_count);
    audio::Buffer<float> output(1, transport.frame_count);
    midi::MidiBuffer midi;
    auto output_view = output.view();
    REQUIRE(engine.process(read_view(input), output_view, midi, transport) ==
            CaptureProcessResult::Ok);
    const auto channel = output.channel(0);
    return {channel.begin(), channel.end()};
}

} // namespace

TEST_CASE("capture engine applies punch boundaries sample-exactly without RT allocation") {
    const auto map = constant_map();
    MasterTransport transport;
    prepare_transport(transport, map, 32);
    CaptureEngine engine;
    REQUIRE(engine.prepare(capture_config()));

    CaptureCommand start;
    start.type = CaptureCommandType::Start;
    start.sequence = 7;
    start.session.punch_in = {10};
    start.session.has_punch_out = true;
    start.session.punch_out = {50};
    REQUIRE(engine.enqueue_command(start));

    audio::Buffer<float> input(1, 32);
    audio::Buffer<float> output(1, 32);
    midi::MidiBuffer midi;
    midi.reserve(4);
    midi.set_realtime_capacity_limit(true);

    fill_absolute(input, 0);
    midi.add([] {
        auto event = midi::MidiEvent::note_on(0, 60, 100);
        event.sample_offset = 12;
        return event;
    }());
    auto first = next_block(transport, 32);
    std::size_t allocations = 0;
    {
        test::ScopedRtProcessProbe probe;
        auto output_view = output.view();
        REQUIRE(engine.process(read_view(input), output_view, midi, first) ==
                CaptureProcessResult::Ok);
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
    REQUIRE(
        std::equal(output.channel(0).begin(), output.channel(0).end(), input.channel(0).begin()));
    REQUIRE(engine.input_peak(0) == 31.0f);

    fill_absolute(input, 32);
    output.clear();
    midi.clear();
    midi.add([] {
        auto event = midi::MidiEvent::note_off(0, 60);
        event.sample_offset = 5;
        return event;
    }());
    auto second = next_block(transport, 32);
    {
        test::ScopedRtProcessProbe probe;
        auto output_view = output.view();
        REQUIRE(engine.process(read_view(input), output_view, midi, second) ==
                CaptureProcessResult::Ok);
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);

    const auto events = drain(engine);
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].type == CaptureEventType::Started);
    REQUIRE(events[1].type == CaptureEventType::TakeCompleted);
    REQUIRE(events[1].placement_start == SamplePosition{10});
    REQUIRE(events[1].frame_count == 40);
    REQUIRE(events[1].midi_event_count == 2);

    audio::Buffer<float> captured(1, 40);
    REQUIRE(engine.copy_audio(events[1].take, captured.view()));
    for (std::size_t frame = 0; frame < 40; ++frame)
        REQUIRE(captured.channel(0)[frame] == static_cast<float>(frame + 10));

    std::array<CapturedMidiEvent, 2> captured_midi;
    std::size_t midi_count = 0;
    REQUIRE(engine.copy_midi(events[1].take, captured_midi, midi_count));
    REQUIRE(midi_count == 2);
    REQUIRE(captured_midi[0].take_frame == 2);
    REQUIRE(captured_midi[0].event.is_note_on());
    REQUIRE(captured_midi[1].take_frame == 27);
    REQUIRE(captured_midi[1].event.is_note_off());
}

TEST_CASE("capture engine accumulates one immutable take per loop pass") {
    const auto map = constant_map();
    const auto loop_end = map.samples_to_ticks({16});
    MasterTransport transport;
    prepare_transport(transport, map, 16, {true, {}, loop_end});
    CaptureEngine engine;
    REQUIRE(engine.prepare(capture_config(16, 3)));

    CaptureCommand start;
    start.type = CaptureCommandType::Start;
    start.sequence = 8;
    start.session.punch_in = {0};
    start.session.has_punch_out = true;
    start.session.punch_out = {16};
    start.session.loop_enabled = true;
    start.session.loop_start = {0};
    start.session.loop_end = {16};
    REQUIRE(engine.enqueue_command(start));

    audio::Buffer<float> input(1, 16);
    audio::Buffer<float> output(1, 16);
    midi::MidiBuffer midi;
    std::fill(input.channel(0).begin(), input.channel(0).end(), 1.0f);
    auto first = next_block(transport, 16);
    auto output_view = output.view();
    REQUIRE(engine.process(read_view(input), output_view, midi, first) == CaptureProcessResult::Ok);

    std::fill(input.channel(0).begin(), input.channel(0).end(), 2.0f);
    output.clear();
    auto second = next_block(transport, 16);
    output_view = output.view();
    REQUIRE(engine.process(read_view(input), output_view, midi, second) ==
            CaptureProcessResult::Ok);

    const auto events = drain(engine);
    REQUIRE(events.size() == 3);
    REQUIRE(events[0].type == CaptureEventType::Started);
    REQUIRE(events[1].type == CaptureEventType::TakeCompleted);
    REQUIRE(events[2].type == CaptureEventType::TakeCompleted);
    REQUIRE(events[1].take != events[2].take);
    REQUIRE(events[1].frame_count == 16);
    REQUIRE(events[2].frame_count == 16);

    audio::Buffer<float> captured(1, 16);
    REQUIRE(engine.copy_audio(events[1].take, captured.view()));
    REQUIRE(std::all_of(captured.channel(0).begin(), captured.channel(0).end(),
                        [](float sample) { return sample == 1.0f; }));
    REQUIRE(engine.copy_audio(events[2].take, captured.view()));
    REQUIRE(std::all_of(captured.channel(0).begin(), captured.channel(0).end(),
                        [](float sample) { return sample == 2.0f; }));
}

TEST_CASE("capture engine finalizes a fractional host-loop pass at its explicit wrap") {
    const auto map = constant_map();
    CaptureEngine engine;
    auto config = capture_config(101, 2);
    config.maximum_take_frames = 202;
    REQUIRE(engine.prepare(std::move(config)));

    CaptureCommand start;
    start.type = CaptureCommandType::Start;
    start.sequence = 9;
    start.session.loop_enabled = true;
    start.session.loop_start = {0};
    start.session.loop_end = {100};
    REQUIRE(engine.enqueue_command(start));

    audio::Buffer<float> input(1, 101);
    audio::Buffer<float> output(1, 101);
    midi::MidiBuffer midi;
    TransportSnapshot before_wrap;
    before_wrap.tempo_map = &map;
    before_wrap.sample_rate = map.sample_rate();
    before_wrap.frame_count = 101;
    before_wrap.is_playing = true;
    before_wrap.range_count = 1;
    before_wrap.ranges[0].frame_count = 101;
    before_wrap.ranges[0].timeline_sample_start = {0};
    before_wrap.ranges[0].timeline_tick_end = {1};
    auto output_view = output.view();
    REQUIRE(engine.process(read_view(input), output_view, midi, before_wrap) ==
            CaptureProcessResult::Ok);

    TransportSnapshot after_wrap = before_wrap;
    after_wrap.ranges[0].frame_count = 1;
    after_wrap.frame_count = 1;
    after_wrap.ranges[0].timeline_sample_start = {0};
    after_wrap.ranges[0].timeline_tick_end = {1};
    after_wrap.ranges[0].discontinuity = true;
    output_view = output.view();
    REQUIRE(engine.process(read_view(input), output_view, midi, after_wrap) ==
            CaptureProcessResult::Ok);

    const auto events = drain(engine);
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].type == CaptureEventType::Started);
    REQUIRE(events[1].type == CaptureEventType::TakeCompleted);
    REQUIRE(events[1].frame_count == 101);
}

TEST_CASE("capture metronome renders during count-in before punch capture") {
    const auto map = constant_map();
    MasterTransport transport;
    prepare_transport(transport, map, 16);
    auto config = capture_config(16);
    config.tracks[0].monitor = false;
    CaptureEngine engine;
    REQUIRE(engine.prepare(std::move(config)));

    CaptureCommand start;
    start.type = CaptureCommandType::Start;
    start.sequence = 9;
    start.session.count_in_start = {0};
    start.session.punch_in = {16};
    start.session.metronome_enabled = true;
    start.session.metronome_level = 0.5f;
    REQUIRE(engine.enqueue_command(start));

    audio::Buffer<float> input(1, 16);
    audio::Buffer<float> output(1, 16);
    midi::MidiBuffer midi;
    auto block = next_block(transport, 16);
    auto output_view = output.view();
    REQUIRE(engine.process(read_view(input), output_view, midi, block) == CaptureProcessResult::Ok);
    REQUIRE(output.channel(0)[0] == 0.5f);
    REQUIRE(std::all_of(output.channel(0).begin() + 1, output.channel(0).end(),
                        [](float sample) { return sample == 0.0f; }));
    REQUIRE(drain(engine).size() == 1);
}

TEST_CASE("capture metronome projects ticks through host beat mapping") {
    const auto map = constant_map();
    auto config = capture_config(4'800);
    config.maximum_take_frames = 4'800;
    config.tracks[0].monitor = false;
    CaptureEngine engine;
    REQUIRE(engine.prepare(std::move(config)));

    CaptureCommand start;
    start.type = CaptureCommandType::Start;
    start.sequence = 10;
    start.session.metronome_enabled = true;
    start.session.metronome_interval = {kTicksPerQuarter / 24};
    start.session.metronome_level = 0.5f;
    REQUIRE(engine.enqueue_command(start));

    TransportSnapshot transport;
    transport.tempo_map = &map;
    transport.sample_rate = map.sample_rate();
    transport.frame_count = 4'800;
    transport.range_count = 1;
    transport.is_playing = true;
    transport.ranges[0].frame_count = 4'800;
    transport.ranges[0].timeline_tick_start = {0};
    transport.ranges[0].timeline_tick_end = {kTicksPerQuarter / 10};
    transport.ranges[0].host_beat_mapping = true;
    transport.ranges[0].host_tick_start = 0.0;
    transport.ranges[0].host_tick_end = static_cast<double>(kTicksPerQuarter) / 10.0;
    transport.ranges[0].has_precise_host_ticks = true;

    audio::Buffer<float> input(1, 4'800);
    audio::Buffer<float> output(1, 4'800);
    midi::MidiBuffer midi;
    auto output_view = output.view();
    REQUIRE(engine.process(read_view(input), output_view, midi, transport) ==
            CaptureProcessResult::Ok);
    REQUIRE(output.channel(0)[0] == 0.5f);
    REQUIRE(output.channel(0)[2'000] == 0.5f);
    REQUIRE(output.channel(0)[4'000] == 0.5f);
    REQUIRE(output.channel(0)[1'000] == 0.0f);
    REQUIRE(output.channel(0)[3'000] == 0.0f);
}

TEST_CASE("capture metronome excludes a rounded click before the precise host start") {
    const auto map = constant_map();
    TransportSnapshot transport;
    transport.frame_count = 100;
    transport.range_count = 1;
    auto& range = transport.ranges[0];
    range.frame_count = 100;
    range.timeline_tick_start = {0};
    range.timeline_tick_end = {10};
    range.host_beat_mapping = true;
    range.host_tick_start = 0.25;
    range.host_tick_end = 9.75;
    range.has_precise_host_ticks = true;

    CaptureSession session;
    session.metronome_interval = {10};
    const auto output = render_metronome_snapshot(map, transport, session);
    REQUIRE(std::all_of(output.begin(), output.end(), [](float sample) { return sample == 0.0f; }));
}

TEST_CASE("capture metronome includes a click before the precise host end") {
    const auto map = constant_map();
    TransportSnapshot transport;
    transport.frame_count = 100;
    transport.range_count = 1;
    auto& range = transport.ranges[0];
    range.frame_count = 100;
    range.timeline_tick_start = {0};
    range.timeline_tick_end = {10};
    range.host_beat_mapping = true;
    range.host_tick_start = 0.25;
    range.host_tick_end = 10.25;
    range.has_precise_host_ticks = true;

    CaptureSession session;
    session.metronome_interval = {10};
    const auto output = render_metronome_snapshot(map, transport, session);
    REQUIRE(std::count(output.begin(), output.end(), 0.5f) == 1);
    REQUIRE(output[97] == 0.5f);
}

TEST_CASE("capture metronome uses precise host endpoints across a loop boundary") {
    const auto map = constant_map();
    TransportSnapshot transport;
    transport.frame_count = 200;
    transport.range_count = 2;
    auto& before_wrap = transport.ranges[0];
    before_wrap.frame_count = 100;
    before_wrap.timeline_tick_start = {10};
    before_wrap.timeline_tick_end = {20};
    before_wrap.host_beat_mapping = true;
    before_wrap.host_tick_start = 10.25;
    before_wrap.host_tick_end = 19.75;
    before_wrap.has_precise_host_ticks = true;
    auto& after_wrap = transport.ranges[1];
    after_wrap.sample_offset = 100;
    after_wrap.frame_count = 100;
    after_wrap.timeline_sample_start = {0};
    after_wrap.timeline_tick_start = {0};
    after_wrap.timeline_tick_end = {10};
    after_wrap.discontinuity = true;
    after_wrap.host_beat_mapping = true;
    after_wrap.host_tick_start = 0.25;
    after_wrap.host_tick_end = 10.25;
    after_wrap.has_precise_host_ticks = true;

    CaptureSession session;
    session.loop_enabled = true;
    session.loop_start = {0};
    session.loop_end = {100};
    session.metronome_interval = {10};
    const auto output = render_metronome_snapshot(map, transport, session);
    REQUIRE(std::count(output.begin(), output.end(), 0.5f) == 1);
    REQUIRE(output[100] == 0.0f);
    REQUIRE(output[197] == 0.5f);
}

TEST_CASE("capture engine rejects aggregate preallocation beyond its explicit budget") {
    CaptureEngine engine;
    REQUIRE(engine.prepare(capture_config()));

    auto undersized_budget = capture_config();
    undersized_budget.maximum_preallocated_bytes = 1;
    REQUIRE_FALSE(engine.prepare(std::move(undersized_budget)));

    auto omitted_container_storage = capture_config(1, 1);
    omitted_container_storage.maximum_take_frames = 1;
    omitted_container_storage.midi_events_per_take = 0;
    omitted_container_storage.maximum_preallocated_bytes = 150;
    REQUIRE_FALSE(engine.prepare(std::move(omitted_container_storage)));

    audio::Buffer<float> input(1, 1);
    audio::Buffer<float> output(1, 1);
    auto output_view = output.view();
    midi::MidiBuffer midi;
    REQUIRE(engine.process(read_view(input), output_view, midi, {}) ==
            CaptureProcessResult::NotPrepared);

    auto aggregate_overflow = capture_config(32, CaptureEngine::kMaximumTakeSlotsPerTrack);
    aggregate_overflow.maximum_take_frames = std::numeric_limits<std::uint64_t>::max();
    aggregate_overflow.maximum_preallocated_bytes = std::numeric_limits<std::uint64_t>::max();
    REQUIRE_FALSE(engine.prepare(std::move(aggregate_overflow)));

    auto midi_overflow = capture_config(32, CaptureEngine::kMaximumTakeSlotsPerTrack);
    midi_overflow.midi_events_per_take = std::numeric_limits<std::uint32_t>::max();
    REQUIRE_FALSE(engine.prepare(std::move(midi_overflow)));

    auto excessive_tracks = capture_config();
    excessive_tracks.tracks.resize(CaptureEngine::kMaximumTracks + 1,
                                   excessive_tracks.tracks.front());
    REQUIRE_FALSE(engine.prepare(excessive_tracks));
}

TEST_CASE("capture engine rejects overflowing channel ranges before indexing") {
    const auto map = constant_map();
    MasterTransport transport;
    prepare_transport(transport, map, 1);
    midi::MidiBuffer midi;

    SECTION("input range") {
        auto config = capture_config(1);
        config.tracks[0].input_channel = std::numeric_limits<std::uint32_t>::max();
        config.tracks[0].armed = false;
        config.tracks[0].monitor = false;
        CaptureEngine engine;
        REQUIRE(engine.prepare(config));

        const audio::BufferView<const float> no_input(nullptr, 0, 1);
        audio::Buffer<float> output(1, 1);
        auto output_view = output.view();
        REQUIRE(engine.process(no_input, output_view, midi, next_block(transport, 1)) ==
                CaptureProcessResult::InvalidBuffers);
    }

    SECTION("output range") {
        auto config = capture_config(1);
        config.tracks[0].output_channel = std::numeric_limits<std::uint32_t>::max();
        config.tracks[0].armed = false;
        config.tracks[0].monitor = false;
        CaptureEngine engine;
        REQUIRE(engine.prepare(config));

        audio::Buffer<float> input(1, 1);
        audio::BufferView<float> no_output(nullptr, 0, 1);
        REQUIRE(engine.process(read_view(input), no_output, midi, next_block(transport, 1)) ==
                CaptureProcessResult::InvalidBuffers);
    }
}

TEST_CASE("capture engine accepts equivalent rational sample rates") {
    const auto map = constant_map();
    MasterTransport transport;
    prepare_transport(transport, map, 1);
    auto config = capture_config(1);
    config.sample_rate = {96'000, 2};
    CaptureEngine engine;
    REQUIRE(engine.prepare(config));

    audio::Buffer<float> input(1, 1);
    audio::Buffer<float> output(1, 1);
    auto output_view = output.view();
    midi::MidiBuffer midi;
    auto snapshot = next_block(transport, 1);
    REQUIRE(engine.process(read_view(input), output_view, midi, snapshot) ==
            CaptureProcessResult::Ok);

    snapshot.sample_rate = {96'000, 2};
    REQUIRE(engine.process(read_view(input), output_view, midi, snapshot) ==
            CaptureProcessResult::Ok);

    snapshot.sample_rate = {44'100, 1};
    REQUIRE(engine.process(read_view(input), output_view, midi, snapshot) ==
            CaptureProcessResult::InvalidTransport);
}
