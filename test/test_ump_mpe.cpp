#include <catch2/catch_test_macros.hpp>
#include <pulp/midi/ump.hpp>

using namespace pulp::midi;

TEST_CASE("UMP MIDI 2.0 Note On", "[midi][ump]") {
    auto p = UmpPacket::note_on_2(0, 0, 60, 0x8000); // middle C, half velocity
    REQUIRE(p.word_count == 2);
    REQUIRE(p.message_type() == UmpMessageType::Midi2ChannelVoice);
    REQUIRE(p.group() == 0);
    REQUIRE(p.channel() == 0);
    REQUIRE(p.note_number() == 60);
    REQUIRE(p.velocity_16() == 0x8000);
    REQUIRE(p.velocity_7() == 64); // 0x8000 >> 9 = 64
}

TEST_CASE("UMP MIDI 2.0 Note Off", "[midi][ump]") {
    auto p = UmpPacket::note_off_2(1, 5, 72, 0);
    REQUIRE(p.message_type() == UmpMessageType::Midi2ChannelVoice);
    REQUIRE(p.group() == 1);
    REQUIRE(p.channel() == 5);
    REQUIRE(p.note_number() == 72);
    REQUIRE(p.velocity_16() == 0);
}

TEST_CASE("UMP MIDI 2.0 CC", "[midi][ump]") {
    auto p = UmpPacket::cc_2(0, 0, 74, 0x80000000); // CC74 (brightness) half value
    REQUIRE(p.message_type() == UmpMessageType::Midi2ChannelVoice);
    REQUIRE(p.data_32() == 0x80000000);
}

TEST_CASE("UMP MIDI 2.0 Pitch Bend", "[midi][ump]") {
    auto p = UmpPacket::pitch_bend_2(0, 0, 0x80000000); // center
    REQUIRE(p.message_type() == UmpMessageType::Midi2ChannelVoice);
    REQUIRE(p.data_32() == 0x80000000);
}

TEST_CASE("UMP Per-Note Pitch Bend", "[midi][ump]") {
    auto p = UmpPacket::per_note_pitch_bend(0, 1, 60, 0xC0000000);
    REQUIRE(p.message_type() == UmpMessageType::Midi2ChannelVoice);
    REQUIRE(p.note_number() == 60);
    REQUIRE(p.channel() == 1);
    REQUIRE(p.data_32() == 0xC0000000);
}

TEST_CASE("UMP MIDI 1.0 backwards compat", "[midi][ump]") {
    auto p = UmpPacket::midi1_note_on(0, 9, 36, 127);
    REQUIRE(p.word_count == 1);
    REQUIRE(p.message_type() == UmpMessageType::Midi1ChannelVoice);
    REQUIRE(p.group() == 0);
}

TEST_CASE("UMP packet sizes", "[midi][ump]") {
    REQUIRE(UmpPacket::size_for_type(UmpMessageType::Utility) == 1);
    REQUIRE(UmpPacket::size_for_type(UmpMessageType::System) == 1);
    REQUIRE(UmpPacket::size_for_type(UmpMessageType::Midi1ChannelVoice) == 1);
    REQUIRE(UmpPacket::size_for_type(UmpMessageType::DataSysEx) == 2);
    REQUIRE(UmpPacket::size_for_type(UmpMessageType::Midi2ChannelVoice) == 2);
    REQUIRE(UmpPacket::size_for_type(UmpMessageType::Data128) == 4);
}

TEST_CASE("UMP Note On with attribute", "[midi][ump]") {
    auto p = UmpPacket::note_on_2(0, 0, 60, 0xFFFF, 3, 0x1234);
    REQUIRE(p.attribute_type() == 3);
    REQUIRE(p.attribute_data() == 0x1234);
    REQUIRE(p.velocity_16() == 0xFFFF);
}

TEST_CASE("MPE zone configuration", "[midi][mpe]") {
    SECTION("Standard lower zone") {
        auto cfg = MpeConfig::standard_lower(15);
        REQUIRE(cfg.lower_zone.is_lower());
        REQUIRE(cfg.lower_zone.member_channels == 15);
        REQUIRE(cfg.lower_zone.contains_channel(1));
        REQUIRE(cfg.lower_zone.contains_channel(15));
        REQUIRE_FALSE(cfg.lower_zone.contains_channel(0)); // manager, not member
    }

    SECTION("Dual zone") {
        auto cfg = MpeConfig::dual(7, 7);
        // Lower: manager=0, members=ch1-7
        REQUIRE(cfg.lower_zone.contains_channel(1));
        REQUIRE(cfg.lower_zone.contains_channel(7));
        REQUIRE_FALSE(cfg.lower_zone.contains_channel(8));

        // Upper: manager=15, members=ch8-14
        REQUIRE(cfg.upper_zone.contains_channel(8));
        REQUIRE(cfg.upper_zone.contains_channel(14));
        REQUIRE_FALSE(cfg.upper_zone.contains_channel(15)); // manager
    }

    SECTION("Manager channel detection") {
        auto cfg = MpeConfig::dual(7, 7);
        REQUIRE(cfg.is_manager_channel(0));
        REQUIRE(cfg.is_manager_channel(15));
        REQUIRE_FALSE(cfg.is_manager_channel(5));
    }

    SECTION("Zone lookup") {
        auto cfg = MpeConfig::dual(7, 7);
        REQUIRE(cfg.zone_for_channel(3) == &cfg.lower_zone);
        REQUIRE(cfg.zone_for_channel(10) == &cfg.upper_zone);
        REQUIRE(cfg.zone_for_channel(0) == nullptr);  // manager, not member
        REQUIRE(cfg.zone_for_channel(15) == nullptr); // manager, not member
    }
}
