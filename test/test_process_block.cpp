#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

using namespace pulp;

static_assert(!std::is_copy_constructible_v<format::BlockScratch>);
static_assert(!std::is_copy_assignable_v<format::BlockScratch>);
static_assert(!std::is_copy_constructible_v<format::PrepareScratch>);
static_assert(format::BusBufferSet::kMaxBuses == 16);
static_assert(std::is_trivially_copyable_v<format::AudioRateModulationView>);
static_assert(std::is_trivially_copyable_v<format::ProcessBlock>);
static_assert(offsetof(format::EventDropCounters, graph_events) <
              offsetof(format::EventDropCounters, audio_rate_modulations));
static_assert(offsetof(format::EventBlock, drops) <
              offsetof(format::EventBlock, audio_rate_modulations));

namespace {

struct ConstAudioView {
    explicit ConstAudioView(audio::Buffer<float>& buffer) {
        REQUIRE(buffer.num_channels() <= ptrs.size());
        for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
            ptrs[ch] = buffer.channel(ch).data();
        }
        view = {ptrs.data(), buffer.num_channels(), buffer.num_samples()};
    }

    std::array<const float*, 16> ptrs{};
    audio::BufferView<const float> view;
};

} // namespace

TEST_CASE("BusBufferSet stores fixed-capacity borrowed bus views", "[format][process-block]") {
    audio::Buffer<float> input(2, 64);
    audio::Buffer<float> output(2, 64);
    audio::Buffer<float> sidechain(1, 64);
    ConstAudioView input_view(input);
    ConstAudioView sidechain_view(sidechain);

    format::BusBufferSet buses;
    REQUIRE(buses.add_input("main in", input_view.view));
    REQUIRE(buses.add_output("main out", output.view()));
    REQUIRE(buses.add_input("key", sidechain_view.view, format::BusRole::Sidechain));

    REQUIRE(buses.size() == 3);
    REQUIRE_FALSE(buses.empty());
    REQUIRE(buses.validate_frame_count(64));
    REQUIRE_FALSE(buses.validate_frame_count(32));

    const auto* main_in = buses.find(format::BusDirection::Input, "main in");
    REQUIRE(main_in != nullptr);
    REQUIRE(main_in->num_channels() == 2);
    REQUIRE(main_in->num_frames() == 64);

    auto* main_out = buses.first(format::BusDirection::Output);
    REQUIRE(main_out != nullptr);
    REQUIRE(main_out->role == format::BusRole::Main);
    REQUIRE(main_out->output.num_channels() == 2);

    for (std::size_t i = buses.size(); i < buses.capacity(); ++i) {
        REQUIRE(buses.add_input("extra", input_view.view, format::BusRole::Aux));
    }
    REQUIRE_FALSE(buses.add_output("overflow", output.view()));
}

TEST_CASE("EventBlock is a non-owning view with explicit drop counters", "[format][process-block]") {
    state::ParameterEventQueue params;
    REQUIRE(params.push({42, 12, 0.5f, 8}));
    REQUIRE(params.push({7, 0, 1.0f, 0}));
    params.sort();

    midi::MidiBuffer midi_in;
    auto note = midi::MidiEvent::note_on(0, 60, 100);
    note.sample_offset = 3;
    midi_in.add(note);
    midi_in.add_sysex({0xF0, 0x7D, 0x01, 0xF7}, 16, 0.0);

    midi::MidiBuffer midi_out;
    std::array<float, 4> audio_rate_values{-1.0f, -0.5f, 0.0f, 1.0f};
    std::array<format::AudioRateModulationView, 1> audio_rate_modulations{{
        {42, std::span<const float>(audio_rate_values.data(), audio_rate_values.size())},
    }};
    format::EventBlock events;
    events.parameter_events = &params;
    events.midi_in = &midi_in;
    events.midi_out = &midi_out;
    events.audio_rate_modulations = std::span<const format::AudioRateModulationView>(
        audio_rate_modulations.data(), audio_rate_modulations.size());
    events.drops.audio_rate_modulations = 1;
    events.drops.midi_events = 2;
    events.drops.graph_events = 1;

    REQUIRE(events.parameter_event_count() == 2);
    REQUIRE(events.parameters().front().sample_offset == 0);
    REQUIRE(events.audio_rate_modulation_count() == 1);
    REQUIRE(events.audio_rate_modulations.front().param_id == 42);
    REQUIRE(events.audio_rate_modulations.front().size() == 4);
    REQUIRE(events.audio_rate_modulations.front().values[3] == 1.0f);
    REQUIRE(events.midi_input_event_count() == 1);
    REQUIRE(events.midi_output_event_count() == 0);
    REQUIRE(events.sysex_event_count() == 1);
    REQUIRE(events.drops.any());
    REQUIRE(events.drops.total() == 4);
    REQUIRE_FALSE(events.empty());

    format::EventBlock empty;
    REQUIRE(empty.empty());

    format::EventDropCounters legacy_counters{1, 2, 3, 4, 5, 6};
    REQUIRE(legacy_counters.parameter_events == 1);
    REQUIRE(legacy_counters.midi_events == 2);
    REQUIRE(legacy_counters.sysex_events == 3);
    REQUIRE(legacy_counters.ump_packets == 4);
    REQUIRE(legacy_counters.mpe_events == 5);
    REQUIRE(legacy_counters.graph_events == 6);
    REQUIRE(legacy_counters.audio_rate_modulations == 0);

}

TEST_CASE("BlockScratch and PrepareScratch allocate from caller-owned memory only", "[format][process-block]") {
    std::array<std::byte, 128> block_memory{};
    format::BlockScratch block_scratch{{block_memory.data(), block_memory.size()}};

    auto floats = block_scratch.try_allocate<float>(8);
    REQUIRE(floats.size() == 8);
    REQUIRE(reinterpret_cast<std::uintptr_t>(floats.data()) % alignof(float) == 0);
    REQUIRE(block_scratch.used_bytes() >= sizeof(float) * 8);

    auto doubles = block_scratch.try_allocate<double>(4);
    REQUIRE(doubles.size() == 4);
    REQUIRE(reinterpret_cast<std::uintptr_t>(doubles.data()) % alignof(double) == 0);

    auto too_many = block_scratch.try_allocate<std::uint64_t>(1024);
    REQUIRE(too_many.empty());

    block_scratch.reset();
    REQUIRE(block_scratch.used_bytes() == 0);
    REQUIRE(block_scratch.remaining_bytes() == block_memory.size());

    std::array<std::byte, 32> prepare_memory{};
    format::PrepareScratch prepare_scratch{{prepare_memory.data(), prepare_memory.size()}};
    auto bytes = prepare_scratch.try_allocate_bytes(8, alignof(std::uint32_t));
    REQUIRE(bytes.size() == 8);
    REQUIRE(prepare_scratch.remaining_bytes() <= prepare_memory.size() - 8);
    REQUIRE(prepare_scratch.try_allocate_bytes(1, 3).empty());
}

TEST_CASE("ProcessBlock validates mode, timing, buses, and transport frame counts", "[format][process-block]") {
    audio::Buffer<float> input(2, 64);
    audio::Buffer<float> output(2, 64);
    ConstAudioView input_view(input);
    format::BusBufferSet buses;
    REQUIRE(buses.add_input("main in", input_view.view));
    REQUIRE(buses.add_output("main out", output.view()));

    std::array<std::byte, 256> scratch_memory{};
    format::BlockScratch scratch{{scratch_memory.data(), scratch_memory.size()}};
    format::EventBlock events;
    format::ProcessContext transport;
    transport.sample_rate = 48000.0;
    transport.num_samples = 64;
    transport.is_playing = true;

    format::ProcessBlock block;
    block.mode = format::ProcessMode::Realtime;
    block.sample_rate = 48000.0;
    block.frame_count = 64;
    block.render_speed = 1.0;
    block.transport = &transport;
    block.buses = &buses;
    block.events = &events;
    block.scratch = &scratch;

    REQUIRE(block.validate());
    REQUIRE(block.is_realtime());
    REQUIRE_FALSE(block.is_offline());
    REQUIRE(block.has_transport());
    REQUIRE(block.has_scratch());

    block.mode = format::ProcessMode::Offline;
    block.render_speed = 4.0;
    block.flags.tail_drain = true;
    REQUIRE(block.validate());
    REQUIRE(block.is_offline());
    REQUIRE(block.flags.tail_drain);

    block.mode = static_cast<format::ProcessMode>(0xFF);
    REQUIRE_FALSE(block.validate());
    block.mode = format::ProcessMode::Realtime;

    std::array<float, 2> short_dense_values{};
    std::array<format::AudioRateModulationView, 1> short_dense_lanes{{
        {1, std::span<const float>(short_dense_values.data(), short_dense_values.size())},
    }};
    format::EventBlock dense_events;
    dense_events.audio_rate_modulations = short_dense_lanes;
    block.events = &dense_events;
    REQUIRE_FALSE(block.validate());
    dense_events.audio_rate_modulations = {};
    block.events = nullptr;

    block.render_speed = 0.0;
    REQUIRE_FALSE(block.validate());
    block.render_speed = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(block.validate());
    block.render_speed = 1.0;
    block.sample_rate = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(block.validate());
    block.sample_rate = 48000.0;

    transport.num_samples = 32;
    REQUIRE(block.validate());
    transport.num_samples = 64;
    transport.sample_rate = 44100.0;
    REQUIRE(block.validate());
    transport.sample_rate = 48000.0;
    REQUIRE(block.validate());

    audio::Buffer<float> short_output(2, 32);
    buses.clear();
    REQUIRE(buses.add_output("short", short_output.view()));
    REQUIRE_FALSE(block.validate());
}
