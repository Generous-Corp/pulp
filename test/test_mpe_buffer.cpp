#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/midi/mpe_buffer.hpp>

using namespace pulp::midi;
using Kind = MpeExpressionEvent::Kind;
using Catch::Approx;

namespace {
MidiEvent channel_pressure(uint8_t channel, uint8_t value) {
    return {choc::midi::ShortMessage(
        static_cast<uint8_t>(0xD0 | (channel & 0x0F)), value, 0), 0, 0.0};
}
} // namespace

TEST_CASE("MpeBuffer records expression events from tracker", "[midi][mpe]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    MpeBuffer buffer;
    int32_t offset = 0;
    bind_tracker_to_buffer(tracker, buffer, offset);

    offset = 10; tracker.process(MidiEvent::note_on(1, 60, 100));
    offset = 20; tracker.process(MidiEvent::pitch_bend(1, 16383));
    offset = 30; tracker.process(channel_pressure(1, 64));
    offset = 40; tracker.process(MidiEvent::cc(1, 74, 100));
    offset = 50; tracker.process(MidiEvent::note_off(1, 60));

    REQUIRE(buffer.size() == 5);
    REQUIRE(buffer[0].kind == Kind::NoteOn);
    REQUIRE(buffer[0].sample_offset == 10);
    REQUIRE(buffer[0].state.note == 60);

    REQUIRE(buffer[1].kind == Kind::PitchBend);
    REQUIRE(buffer[1].sample_offset == 20);
    REQUIRE(buffer[1].state.pitch_bend_semitones == Approx(48.0f).margin(0.01f));

    REQUIRE(buffer[2].kind == Kind::Pressure);
    REQUIRE(buffer[2].state.pressure > 0.4f);

    REQUIRE(buffer[3].kind == Kind::Timbre);
    REQUIRE(buffer[3].state.timbre == Approx(100.0f / 127.0f).margin(1e-6f));

    REQUIRE(buffer[4].kind == Kind::NoteOff);
    REQUIRE(buffer[4].sample_offset == 50);
}

TEST_CASE("MpeBuffer clear and sort", "[midi][mpe]") {
    MpeBuffer buffer;
    buffer.add({30, Kind::Pressure, {}});
    buffer.add({10, Kind::NoteOn, {}});
    buffer.add({20, Kind::PitchBend, {}});
    REQUIRE(buffer.size() == 3);

    buffer.sort();
    REQUIRE(buffer[0].sample_offset == 10);
    REQUIRE(buffer[1].sample_offset == 20);
    REQUIRE(buffer[2].sample_offset == 30);

    buffer.clear();
    REQUIRE(buffer.empty());
}

TEST_CASE("Processor::supports_mpe defaults to false", "[midi][mpe]") {
    pulp::format::PluginDescriptor desc;
    REQUIRE_FALSE(desc.supports_mpe);
}
