#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/midi/mpe_buffer.hpp>
#include <vector>

using namespace pulp::midi;
using Kind = MpeExpressionEvent::Kind;
using Catch::Approx;

namespace {
MidiEvent channel_pressure(uint8_t channel, uint8_t value) {
    return {choc::midi::ShortMessage(
        static_cast<uint8_t>(0xD0 | (channel & 0x0F)), value, 0), 0, 0.0};
}

UmpPacket channel_pressure_ump(uint8_t group, uint8_t channel, uint32_t value) {
    UmpPacket p;
    p.word_count = 2;
    p.words[0] = (0x4u << 28)
        | (static_cast<uint32_t>(group & 0x0F) << 24)
        | (static_cast<uint32_t>(0xD0 | (channel & 0x0F)) << 16);
    p.words[1] = value;
    return p;
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

TEST_CASE("MpeConfig identifies lower and upper zone member boundaries",
          "[midi][mpe][issue-645]") {
    auto cfg = MpeConfig::dual(3, 2);

    REQUIRE(cfg.lower_zone.is_lower());
    REQUIRE(cfg.upper_zone.is_upper());
    REQUIRE(cfg.is_manager_channel(0));
    REQUIRE(cfg.is_manager_channel(15));

    REQUIRE(cfg.zone_for_channel(1) == &cfg.lower_zone);
    REQUIRE(cfg.zone_for_channel(3) == &cfg.lower_zone);
    REQUIRE(cfg.zone_for_channel(4) == nullptr);
    REQUIRE(cfg.zone_for_channel(13) == &cfg.upper_zone);
    REQUIRE(cfg.zone_for_channel(14) == &cfg.upper_zone);
    REQUIRE(cfg.zone_for_channel(12) == nullptr);
}

TEST_CASE("MpeVoiceTracker seeds new notes from cached member expression",
          "[midi][mpe][issue-645]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};

    REQUIRE(tracker.process(MidiEvent::pitch_bend(2, 16383)));
    REQUIRE(tracker.process(channel_pressure(2, 64)));
    REQUIRE(tracker.process(MidiEvent::cc(2, 74, 100)));
    REQUIRE(tracker.process(MidiEvent::note_on(2, 61, 90)));

    const auto* note = tracker.find(2, 61);
    REQUIRE(note != nullptr);
    REQUIRE(note->pitch_bend_semitones == Approx(48.0f).margin(0.01f));
    REQUIRE(note->pressure == Approx(64.0f / 127.0f).margin(1e-6f));
    REQUIRE(note->timbre == Approx(100.0f / 127.0f).margin(1e-6f));
}

TEST_CASE("MpeVoiceTracker manager messages update zone state without notes",
          "[midi][mpe][issue-645]") {
    MpeVoiceTracker tracker{MpeConfig::dual(4, 3)};
    tracker.set_manager_bend_range(2.0f);

    REQUIRE(tracker.process(MidiEvent::pitch_bend(0, 16383)));
    REQUIRE(tracker.process(channel_pressure(15, 127)));
    REQUIRE(tracker.process(MidiEvent::cc(15, 74, 64)));

    REQUIRE(tracker.active_count() == 0);
    REQUIRE(tracker.lower_zone_state().pitch_bend_semitones == Approx(2.0f).margin(0.01f));
    REQUIRE(tracker.upper_zone_state().pressure == Approx(1.0f).margin(1e-6f));
    REQUIRE(tracker.upper_zone_state().timbre == Approx(64.0f / 127.0f).margin(1e-6f));
}

TEST_CASE("MpeVoiceTracker UMP manager and member events feed callbacks",
          "[midi][mpe][issue-645]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    MpeBuffer buffer;
    int32_t offset = 0;
    bind_tracker_to_buffer(tracker, buffer, offset);

    offset = 5;
    REQUIRE(tracker.process(UmpPacket::pitch_bend_2(0, 0, 0xFFFFFFFFu)));
    REQUIRE(buffer.empty());
    REQUIRE(tracker.lower_zone_state().pitch_bend_semitones > 1.9f);

    offset = 10;
    REQUIRE(tracker.process(UmpPacket::note_on_2(0, 3, 67, 0xFFFF)));
    offset = 20;
    REQUIRE(tracker.process(channel_pressure_ump(0, 3, 0xFFFFFFFFu)));
    offset = 30;
    REQUIRE(tracker.process(UmpPacket::registered_per_note_cc(0, 3, 67, 74,
                                                              0xFFFFFFFFu)));

    REQUIRE(buffer.size() == 3);
    REQUIRE(buffer[0].kind == Kind::NoteOn);
    REQUIRE(buffer[0].sample_offset == 10);
    REQUIRE(buffer[0].state.velocity == 127);
    REQUIRE(buffer[1].kind == Kind::Pressure);
    REQUIRE(buffer[1].state.pressure == Approx(1.0f).margin(1e-6f));
    REQUIRE(buffer[2].kind == Kind::Timbre);
    REQUIRE(buffer[2].state.timbre == Approx(1.0f).margin(1e-6f));
}

TEST_CASE("MpeVoiceTracker reset and config changes clear active state",
          "[midi][mpe][issue-645]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.process(MidiEvent::note_on(1, 60, 100));
    tracker.process(MidiEvent::pitch_bend(1, 16383));
    REQUIRE(tracker.active_count() == 1);
    REQUIRE(tracker.member_bend_range() == Approx(48.0f));

    tracker.set_member_bend_range(-1.0f);
    tracker.set_manager_bend_range(0.0f);
    REQUIRE(tracker.member_bend_range() == Approx(48.0f));
    REQUIRE(tracker.manager_bend_range() == Approx(2.0f));

    tracker.set_config(MpeConfig::dual(2, 2));
    REQUIRE(tracker.active_count() == 0);
    REQUIRE(tracker.find(1, 60) == nullptr);
    REQUIRE(tracker.config().upper_zone.member_channels == 2);
    REQUIRE(tracker.lower_zone_state().pitch_bend_semitones == Approx(0.0f));
}

TEST_CASE("Processor::supports_mpe defaults to false", "[midi][mpe]") {
    pulp::format::PluginDescriptor desc;
    REQUIRE_FALSE(desc.supports_mpe);
}
