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

TEST_CASE("MpeBuffer groups equal sample offsets without dropping events",
          "[midi][mpe][issue-645]") {
    MpeBuffer buffer;
    MpeNoteState first;
    first.note = 60;
    MpeNoteState second;
    second.note = 64;
    MpeNoteState third;
    third.note = 67;

    buffer.add({12, Kind::NoteOn, first});
    buffer.add({12, Kind::Pressure, second});
    buffer.add({20, Kind::Timbre, third});
    buffer.sort();

    REQUIRE(buffer.size() == 3);
    REQUIRE(buffer[0].sample_offset == 12);
    REQUIRE(buffer[1].sample_offset == 12);
    REQUIRE(buffer[2].sample_offset == 20);
    const bool saw_first_equal_offset = buffer[0].state.note == 60
                                     || buffer[1].state.note == 60;
    const bool saw_second_equal_offset = buffer[0].state.note == 64
                                      || buffer[1].state.note == 64;
    REQUIRE(saw_first_equal_offset);
    REQUIRE(saw_second_equal_offset);
    REQUIRE(buffer[2].state.note == 67);
}

TEST_CASE("MpeBuffer accepts moved expression events and supports const iteration",
          "[midi][mpe][issue-645]") {
    MpeBuffer buffer;
    MpeExpressionEvent event;
    event.sample_offset = 7;
    event.kind = Kind::NoteOn;
    event.state.channel = 3;
    event.state.note = 72;
    event.state.velocity = 100;

    buffer.add(std::move(event));

    const MpeBuffer& const_buffer = buffer;
    int seen = 0;
    for (const auto& e : const_buffer) {
        REQUIRE(e.sample_offset == 7);
        REQUIRE(e.kind == Kind::NoteOn);
        REQUIRE(e.state.channel == 3);
        REQUIRE(e.state.note == 72);
        REQUIRE(e.state.velocity == 100);
        ++seen;
    }
    REQUIRE(seen == 1);
}

TEST_CASE("bind_tracker_to_buffer uses the latest referenced sample offset",
          "[midi][mpe][issue-645]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    MpeBuffer buffer;
    int32_t offset = 0;
    bind_tracker_to_buffer(tracker, buffer, offset);

    offset = 3;
    REQUIRE(tracker.process(MidiEvent::note_on(1, 60, 100)));
    offset = 99;
    REQUIRE(tracker.process(MidiEvent::note_off(1, 60)));

    REQUIRE(buffer.size() == 2);
    REQUIRE(buffer[0].sample_offset == 3);
    REQUIRE(buffer[0].kind == Kind::NoteOn);
    REQUIRE(buffer[1].sample_offset == 99);
    REQUIRE(buffer[1].kind == Kind::NoteOff);
}

TEST_CASE("Processor::supports_mpe defaults to false", "[midi][mpe]") {
    pulp::format::PluginDescriptor desc;
    REQUIRE_FALSE(desc.supports_mpe);
}
