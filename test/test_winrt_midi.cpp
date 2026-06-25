// WinRT MIDI 2.0 (UMP) backend tests — W3.
//
// The WinRT backend TU (winrt_midi_device.cpp) is opt-in via
// PULP_HAS_WINRT_MIDI and #error-guarded off non-Windows hosts, so it is
// not compiled in the default CI matrix. These tests therefore exercise
// the cross-platform building blocks the backend is built on — the public
// UMP→MIDI1 conversion, the shared sysex7 reassembler, and the F0..F7
// framing contract the backend applies before delivering SysEx — plus the
// always-present port-change callback contract on the active backend.
//
// The framing helper below mirrors the exact logic in
// WinrtMidiInput::on_sysex_complete: the shared reassembler emits an
// unframed payload, and the backend wraps it to F0..F7 so its SysEx
// delivery matches the mmeapi / ALSA / CoreMIDI backends. Locking that
// here keeps the Windows-only path from silently drifting.

#include <catch2/catch_test_macros.hpp>

#include <pulp/midi/device.hpp>
#include <pulp/midi/ump.hpp>
#include <pulp/midi/ump_conversion.hpp>
#include <pulp/midi/ump_sysex7_reassembler.hpp>

#include <array>
#include <cstdint>
#include <vector>

using namespace pulp::midi;

namespace {

// Mirror of WinrtMidiInput::on_sysex_complete framing: take the unframed
// sysex7 payload the reassembler emits and wrap it to a full F0..F7 stream.
std::vector<uint8_t> frame_sysex(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> framed;
    framed.reserve(payload.size() + 2);
    framed.push_back(0xF0);
    framed.insert(framed.end(), payload.begin(), payload.end());
    framed.push_back(0xF7);
    return framed;
}

} // namespace

TEST_CASE("MidiSystem: port-change callback contract on the active backend",
          "[midi][hotplug][winrt][issue-245]") {
    // Every backend must accept and clear a port-change callback without
    // crashing, whether or not it actually wires real hotplug. The WinRT
    // backend wires a MidiEndpointDeviceWatcher; the mmeapi fallback and
    // the other platform backends provide a safe no-op default.
    auto sys = create_midi_system();
    REQUIRE(sys != nullptr);

    bool fired = false;
    sys->set_port_change_callback([&fired] { fired = true; });
    // Unregister — must not fire or crash on teardown.
    sys->set_port_change_callback(nullptr);
    SUCCEED("set_port_change_callback registers + unregisters cleanly");
    (void)fired;
}

TEST_CASE("WinRT MIDI: MIDI 2.0 channel-voice UMP decodes to MIDI 1.0 events",
          "[midi][ump][winrt][issue-245]") {
    // The backend's input path runs received channel-voice UMP through
    // ump_to_midi1_event() before invoking the short-message callback.
    // Verify the note-on/off + CC + pitch-bend paths the backend relies on.

    SECTION("MIDI 2.0 note-on, 16-bit velocity scales to 7-bit") {
        const UmpPacket p = UmpPacket::note_on_2(/*group=*/0, /*ch=*/3,
                                                 /*note=*/60, /*vel16=*/0xFFFF);
        MidiEvent ev{};
        REQUIRE(ump_to_midi1_event(p, ev));
        REQUIRE(ev.data()[0] == (0x90 | 3));
        REQUIRE(ev.data()[1] == 60);
        REQUIRE(ev.data()[2] == 127);
    }

    SECTION("MIDI 2.0 note-on with tiny non-zero velocity clamps to >=1") {
        const UmpPacket p = UmpPacket::note_on_2(0, 0, 64, /*vel16=*/1);
        MidiEvent ev{};
        REQUIRE(ump_to_midi1_event(p, ev));
        REQUIRE(ev.data()[0] == 0x90);
        REQUIRE(ev.data()[2] >= 1);  // never collapses to a note-off
    }

    SECTION("MIDI 1.0 channel-voice UMP passes through verbatim") {
        const UmpPacket p = UmpPacket::midi1_note_on(0, 5, 72, 100);
        MidiEvent ev{};
        REQUIRE(ump_to_midi1_event(p, ev));
        REQUIRE(ev.data()[0] == (0x90 | 5));
        REQUIRE(ev.data()[1] == 72);
        REQUIRE(ev.data()[2] == 100);
    }

    SECTION("a SysEx (Data) UMP has no MIDI 1.0 short-message equivalent") {
        UmpPacket p;
        p.word_count = 2;
        p.words[0] = (0x3u << 28);  // Type 0x3 = DataSysEx
        MidiEvent ev{};
        REQUIRE_FALSE(ump_to_midi1_event(p, ev));
    }
}

TEST_CASE("WinRT MIDI: sysex7 reassembly framed to F0..F7 for delivery",
          "[midi][ump][sysex][winrt][issue-245]") {
    UmpSysex7Reassembler r;

    SECTION("single complete-in-one packet (status 0x0)") {
        // 3 payload bytes: 0x01 0x02 0x03
        const uint32_t w0 = (0x3u << 28) | (0x0u << 20) | (0x3u << 16)
                          | (0x01u << 8) | 0x02u;
        const uint32_t w1 = (0x03u << 24);
        std::vector<uint8_t> payload;
        const auto status = feed_collect(r, w0, w1, payload);
        REQUIRE(status == UmpSysex7Reassembler::Status::single_packet);

        const auto framed = frame_sysex(payload);
        REQUIRE(framed.size() == 5);
        REQUIRE(framed.front() == 0xF0);
        REQUIRE(framed.back() == 0xF7);
        REQUIRE(framed[1] == 0x01);
        REQUIRE(framed[2] == 0x02);
        REQUIRE(framed[3] == 0x03);
    }

    SECTION("multi-packet start -> continue -> end reassembles in order") {
        // start: 6 bytes 0x10..0x15
        const uint32_t s0 = (0x3u << 28) | (0x1u << 20) | (0x6u << 16)
                          | (0x10u << 8) | 0x11u;
        const uint32_t s1 = (0x12u << 24) | (0x13u << 16) | (0x14u << 8) | 0x15u;
        std::vector<uint8_t> sink;
        REQUIRE(feed_collect(r, s0, s1, sink)
                == UmpSysex7Reassembler::Status::start);

        // continue: 6 bytes 0x16..0x1B
        const uint32_t c0 = (0x3u << 28) | (0x2u << 20) | (0x6u << 16)
                          | (0x16u << 8) | 0x17u;
        const uint32_t c1 = (0x18u << 24) | (0x19u << 16) | (0x1Au << 8) | 0x1Bu;
        REQUIRE(feed_collect(r, c0, c1, sink)
                == UmpSysex7Reassembler::Status::continued);

        // end: 2 bytes 0x1C 0x1D — completes, emits the full payload
        const uint32_t e0 = (0x3u << 28) | (0x3u << 20) | (0x2u << 16)
                          | (0x1Cu << 8) | 0x1Du;
        std::vector<uint8_t> payload;
        REQUIRE(feed_collect(r, e0, 0u, payload)
                == UmpSysex7Reassembler::Status::ended);

        const auto framed = frame_sysex(payload);
        REQUIRE(framed.size() == 16);  // F0 + 14 bytes + F7
        REQUIRE(framed.front() == 0xF0);
        REQUIRE(framed.back() == 0xF7);
        REQUIRE(framed[1] == 0x10);
        REQUIRE(framed[14] == 0x1D);
    }
}

TEST_CASE("WinRT MIDI: per-group sysex7 reassembly keeps interleaved streams separate",
          "[midi][ump][sysex][winrt][pr3781]") {
    // SysEx7 reassembly is per-stream state, and UMP SysEx7
    // streams can interleave across the 16 UMP groups. The backend keeps one
    // reassembler per group so a Start on group 1 cannot reset/corrupt an
    // in-flight stream on group 0 — which a single shared reassembler would.
    std::array<UmpSysex7Reassembler, 16> r{};

    auto w0 = [](uint8_t group, uint8_t status, uint8_t nbytes,
                 uint8_t b1, uint8_t b2) {
        return (0x3u << 28) | (uint32_t(group) << 24) | (uint32_t(status) << 20)
             | (uint32_t(nbytes) << 16) | (uint32_t(b1) << 8) | uint32_t(b2);
    };

    // Group 0: Start with 2 bytes (stream still open).
    std::vector<uint8_t> g0;
    REQUIRE(feed_collect(r[0], w0(0, 0x1, 2, 0xA0, 0xA1), 0u, g0)
            == UmpSysex7Reassembler::Status::start);

    // Group 1: a full single-packet stream arrives in between.
    std::vector<uint8_t> g1;
    REQUIRE(feed_collect(r[1], w0(1, 0x0, 3, 0xB0, 0xB1), (0xB2u << 24), g1)
            == UmpSysex7Reassembler::Status::single_packet);
    REQUIRE(g1 == std::vector<uint8_t>{0xB0, 0xB1, 0xB2});

    // Group 0: End with 2 bytes — completes the ORIGINAL stream intact,
    // unaffected by group 1's intervening packet.
    std::vector<uint8_t> g0done;
    REQUIRE(feed_collect(r[0], w0(0, 0x3, 2, 0xA2, 0xA3), 0u, g0done)
            == UmpSysex7Reassembler::Status::ended);
    REQUIRE(g0done == std::vector<uint8_t>{0xA0, 0xA1, 0xA2, 0xA3});
}

TEST_CASE("WinRT MIDI: MIDI 1.0 short message promotes to MIDI 2.0 UMP for send",
          "[midi][ump][winrt][issue-245]") {
    // The output path runs each MidiEvent through midi1_event_to_ump2()
    // before transmitting on the MIDI 2.0 wire. Verify a note-on promotes
    // to a 64-bit MIDI 2.0 channel-voice packet with scaled velocity.
    const MidiEvent ev = MidiEvent::note_on(/*ch=*/2, /*note=*/48, /*vel=*/127);
    const UmpPacket p = midi1_event_to_ump2(ev, /*group=*/0);

    REQUIRE(p.message_type() == UmpMessageType::Midi2ChannelVoice);
    REQUIRE(p.word_count == 2);
    REQUIRE(p.note_number() == 48);
    // 7-bit max velocity scales to 16-bit full-scale.
    REQUIRE(p.velocity_16() == 0xFFFF);
}
