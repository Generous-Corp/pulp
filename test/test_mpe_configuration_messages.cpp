#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/midi/mpe_config_messages.hpp>
#include <pulp/midi/mpe_voice_tracker.hpp>

#include <vector>

using namespace pulp::midi;
using Catch::Approx;

namespace {

template <typename List>
void feed(MpeVoiceTracker& t, const List& list) {
    for (const auto& e : list) t.process(e);
}

void rpn(MpeVoiceTracker& t, uint8_t ch, uint8_t msb, uint8_t lsb, uint8_t data_msb,
         int data_lsb = -1) {
    t.process(MidiEvent::cc(ch, 101, msb));
    t.process(MidiEvent::cc(ch, 100, lsb));
    t.process(MidiEvent::cc(ch, 6, data_msb));
    if (data_lsb >= 0) t.process(MidiEvent::cc(ch, 38, static_cast<uint8_t>(data_lsb)));
}

} // namespace

TEST_CASE("MCM on the lower manager channel sets the lower zone member count", "[midi][mpe][config]") {
    MpeVoiceTracker t{MpeConfig{}};  // no zones
    REQUIRE_FALSE(t.process(MidiEvent::note_on(1, 60, 100)));

    int changes = 0;
    t.on_config_change = [&](const MpeConfig&) { ++changes; };
    feed(t, mpe_configuration_message(0, 7));

    REQUIRE(changes == 1);
    REQUIRE(t.config().lower_zone.member_channels == 7);
    REQUIRE(t.config().lower_zone.manager_channel == 0);
    REQUIRE(t.process(MidiEvent::note_on(7, 60, 100)));
    REQUIRE(t.active_count() == 1);
    REQUIRE_FALSE(t.process(MidiEvent::note_on(8, 60, 100)));
}

TEST_CASE("MCM sequence builds two zones and shrinks the displaced zone", "[midi][mpe][config]") {
    MpeVoiceTracker t{MpeConfig{}};
    feed(t, mpe_configuration_message(0, 5));
    feed(t, mpe_configuration_message(15, 4));
    REQUIRE(t.config().lower_zone.member_channels == 5);
    REQUIRE(t.config().upper_zone.member_channels == 4);
    REQUIRE(t.config().upper_zone.manager_channel == 15);
    REQUIRE(t.config().zone_for_channel(11)->is_upper());   // 15-4..14
    REQUIRE(t.config().zone_for_channel(5)->is_lower());

    // Growing the lower zone to 12 members leaves only 2 for the upper zone.
    feed(t, mpe_configuration_message(0, 12));
    REQUIRE(t.config().lower_zone.member_channels == 12);
    REQUIRE(t.config().upper_zone.member_channels == 2);

    // A full-width upper zone removes the lower zone.
    feed(t, mpe_configuration_message(15, 14));
    REQUIRE(t.config().upper_zone.member_channels == 14);
    REQUIRE(t.config().lower_zone.member_channels == 0);

    // Zero members disables a zone.
    feed(t, mpe_configuration_message(15, 0));
    REQUIRE(t.config().upper_zone.member_channels == 0);
}

TEST_CASE("MCM works with the data entry LSB omitted or present and ignores other channels",
          "[midi][mpe][config]") {
    MpeVoiceTracker t{MpeConfig{}};
    rpn(t, 0, 0, 6, 3, 0);
    REQUIRE(t.config().lower_zone.member_channels == 3);
    rpn(t, 4, 0, 6, 9);  // a member channel cannot carry an MCM
    REQUIRE(t.config().lower_zone.member_channels == 3);
    REQUIRE(t.config().upper_zone.member_channels == 0);
    rpn(t, 0, 0, 6, 100);  // out of range clamps to 15
    REQUIRE(t.config().lower_zone.member_channels == 15);
}

TEST_CASE("MCM releases notes on channels that leave a zone", "[midi][mpe][config]") {
    MpeVoiceTracker t{MpeConfig::standard_lower(15)};
    std::vector<uint8_t> released;
    t.on_note_off = [&](const MpeNoteState& n) { released.push_back(n.channel); };
    t.process(MidiEvent::note_on(2, 60, 90));
    t.process(MidiEvent::note_on(9, 62, 90));

    feed(t, mpe_configuration_message(0, 4));

    REQUIRE(released == std::vector<uint8_t>{9});
    REQUIRE(t.active_count() == 1);
    REQUIRE(t.find(2, 60) != nullptr);
}

TEST_CASE("MCM resets the zone's bend ranges and expression to defaults", "[midi][mpe][config]") {
    MpeVoiceTracker t{MpeConfig::standard_lower(8)};
    rpn(t, 3, 0, 0, 12);
    rpn(t, 0, 0, 0, 24);
    t.process(MidiEvent::pitch_bend(3, 16383));
    REQUIRE(t.member_bend_range(3) == Approx(12.0f));
    REQUIRE(t.manager_bend_range(false) == Approx(24.0f));

    feed(t, mpe_configuration_message(0, 8));

    REQUIRE(t.member_bend_range(3) == Approx(48.0f));
    REQUIRE(t.manager_bend_range(false) == Approx(2.0f));
    t.process(MidiEvent::note_on(3, 60, 100));
    REQUIRE(t.find(3, 60)->pitch_bend_semitones == Approx(0.0f));
}

TEST_CASE("Pitch-bend-range RPN changes the tracker's bend scaling", "[midi][mpe][config]") {
    MpeVoiceTracker t{MpeConfig::standard_lower(15)};
    t.process(MidiEvent::note_on(1, 60, 100));
    t.process(MidiEvent::pitch_bend(1, 16383));
    REQUIRE(t.find(1, 60)->pitch_bend_semitones == Approx(48.0f).margin(0.01f));

    rpn(t, 1, 0, 0, 12);
    REQUIRE(t.member_bend_range(1) == Approx(12.0f));
    t.process(MidiEvent::pitch_bend(1, 16383));
    REQUIRE(t.find(1, 60)->pitch_bend_semitones == Approx(12.0f).margin(0.01f));
    t.process(MidiEvent::pitch_bend(1, 0));
    REQUIRE(t.find(1, 60)->pitch_bend_semitones == Approx(-12.0f));

    // Cents arrive in Data Entry LSB.
    rpn(t, 1, 0, 0, 2, 50);
    REQUIRE(t.member_bend_range(1) == Approx(2.5f));
}

TEST_CASE("Member range RPN is per channel and manager range RPN is per zone", "[midi][mpe][config]") {
    MpeVoiceTracker t{MpeConfig::dual(5, 5)};
    rpn(t, 2, 0, 0, 24);
    REQUIRE(t.member_bend_range(2) == Approx(24.0f));
    REQUIRE(t.member_bend_range(3) == Approx(48.0f));

    rpn(t, 0, 0, 0, 7);
    rpn(t, 15, 0, 0, 3);
    REQUIRE(t.manager_bend_range(false) == Approx(7.0f));
    REQUIRE(t.manager_bend_range(true) == Approx(3.0f));
    REQUIRE(t.member_bend_range(1) == Approx(48.0f));

    t.process(MidiEvent::pitch_bend(0, 16383));
    REQUIRE(t.lower_zone_state().pitch_bend_semitones == Approx(7.0f).margin(0.01f));
}

TEST_CASE("RPN selection state is per channel", "[midi][mpe][config]") {
    MpeVoiceTracker t{MpeConfig::standard_lower(15)};
    // Channel 1 selects pitch-bend range, channel 2 interleaves its own selection.
    t.process(MidiEvent::cc(1, 101, 0));
    t.process(MidiEvent::cc(2, 101, 0));
    t.process(MidiEvent::cc(1, 100, 0));
    t.process(MidiEvent::cc(2, 100, 0));
    t.process(MidiEvent::cc(2, 6, 5));
    REQUIRE(t.member_bend_range(2) == Approx(5.0f));
    REQUIRE(t.member_bend_range(1) == Approx(48.0f));
    t.process(MidiEvent::cc(1, 6, 9));
    REQUIRE(t.member_bend_range(1) == Approx(9.0f));
    // Data entry on a channel that never selected anything does nothing.
    t.process(MidiEvent::cc(3, 6, 1));
    REQUIRE(t.member_bend_range(3) == Approx(48.0f));
}

TEST_CASE("NRPN selection never reads as RPN", "[midi][mpe][config]") {
    MpeVoiceTracker t{MpeConfig::standard_lower(15)};
    // NRPN 0/0 followed by data entry must not change the pitch-bend range.
    t.process(MidiEvent::cc(1, 99, 0));
    t.process(MidiEvent::cc(1, 98, 0));
    t.process(MidiEvent::cc(1, 6, 3));
    t.process(MidiEvent::cc(1, 38, 0));
    REQUIRE(t.member_bend_range(1) == Approx(48.0f));

    // RPN selected, then switched to NRPN before the data entry.
    t.process(MidiEvent::cc(1, 101, 0));
    t.process(MidiEvent::cc(1, 100, 0));
    t.process(MidiEvent::cc(1, 99, 1));
    t.process(MidiEvent::cc(1, 98, 1));
    t.process(MidiEvent::cc(1, 6, 4));
    REQUIRE(t.member_bend_range(1) == Approx(48.0f));

    // And the reverse: an NRPN selection followed by an RPN works normally.
    t.process(MidiEvent::cc(1, 101, 0));
    t.process(MidiEvent::cc(1, 100, 0));
    t.process(MidiEvent::cc(1, 6, 6));
    REQUIRE(t.member_bend_range(1) == Approx(6.0f));
}

TEST_CASE("RPN null deselects and an incomplete selection is ignored", "[midi][mpe][config]") {
    MpeVoiceTracker t{MpeConfig::standard_lower(15)};
    rpn(t, 1, 0, 0, 6);
    REQUIRE(t.member_bend_range(1) == Approx(6.0f));
    t.process(MidiEvent::cc(1, 101, 127));
    t.process(MidiEvent::cc(1, 100, 127));
    t.process(MidiEvent::cc(1, 6, 20));
    REQUIRE(t.member_bend_range(1) == Approx(6.0f));

    // MSB alone (LSB from an older selection must not carry over).
    t.process(MidiEvent::cc(1, 101, 0));
    t.process(MidiEvent::cc(1, 6, 21));
    REQUIRE(t.member_bend_range(1) == Approx(6.0f));
}

TEST_CASE("Configuration following can be switched off", "[midi][mpe][config]") {
    MpeVoiceTracker t{MpeConfig::standard_lower(4)};
    t.set_follow_configuration_messages(false);
    feed(t, mpe_configuration_message(0, 9));
    rpn(t, 1, 0, 0, 3);
    REQUIRE(t.config().lower_zone.member_channels == 4);
    REQUIRE(t.member_bend_range(1) == Approx(48.0f));
}

TEST_CASE("Builders produce the documented byte sequences", "[midi][mpe][config]") {
    const auto mcm = mpe_configuration_message(15, 6);
    REQUIRE(mcm.size() == 3);
    REQUIRE(mcm.events[0].message.data()[0] == 0xBF);
    REQUIRE(mcm.events[0].cc_number() == 101);
    REQUIRE(mcm.events[1].cc_number() == 100);
    REQUIRE(mcm.events[1].cc_value() == 6);
    REQUIRE(mcm.events[2].cc_number() == 6);
    REQUIRE(mcm.events[2].cc_value() == 6);

    const auto pbr = mpe_pitch_bend_range_message(3, 24.5f);
    REQUIRE(pbr.size() == 4);
    REQUIRE(pbr.events[2].cc_value() == 24);
    REQUIRE(pbr.events[3].cc_number() == 38);
    REQUIRE(pbr.events[3].cc_value() == 50);

    const auto layout = mpe_zone_layout_messages(MpeConfig::dual(3, 2));
    REQUIRE(layout.size() == 6);
    REQUIRE(layout.events[2].cc_value() == 3);
    REQUIRE(layout.events[5].cc_value() == 2);
    // A lower-only config disables the upper zone explicitly.
    const auto lower_only = mpe_zone_layout_messages(MpeConfig::standard_lower(15));
    REQUIRE(lower_only.events[2].cc_value() == 15);
    REQUIRE(lower_only.events[5].cc_value() == 0);
}

TEST_CASE("A layout built from a config round-trips through a tracker", "[midi][mpe][config]") {
    const MpeConfig want = MpeConfig::dual(6, 3);
    MpeVoiceTracker t{MpeConfig{}};
    feed(t, mpe_zone_layout_messages(want));
    feed(t, mpe_zone_pitch_bend_range_messages(want.lower_zone, 2.0f, 24.0f));
    REQUIRE(t.config().lower_zone.member_channels == 6);
    REQUIRE(t.config().upper_zone.member_channels == 3);
    REQUIRE(t.member_bend_range(4) == Approx(24.0f));
    REQUIRE(t.member_bend_range(13) == Approx(48.0f));

    struct Recorder : MidiOutput {
        std::vector<MidiEvent> sent;
        bool open(const std::string&) override { return true; }
        void close() override {}
        bool is_open() const override { return true; }
        void send(const MidiEvent& e) override { sent.push_back(e); }
    } out;
    send_mpe_messages(out, mpe_configuration_message(0, 2));
    REQUIRE(out.sent.size() == 3);
}
