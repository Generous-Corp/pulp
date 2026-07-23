#include "harness/scoped_rt_process_probe.hpp"
#include "timebase_test_helpers.hpp"

#include <pulp/playback/capture_engine.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
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
