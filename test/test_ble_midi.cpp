// BLE MIDI tests — packet decoder + cross-platform factory contract.
//
// Headless coverage for the BLE-MIDI 1.0 packet codec and the
// stub central. The CoreBluetooth backend requires a real
// Bluetooth adapter and an OS-granted permission dialog, so its
// live path is exercised by the documented manual smoke (see
// planning gap-doc entry "BLE MIDI"); these tests pin the
// cross-platform half that every backend reuses.

#include <pulp/midi/ble_midi.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace pulp::midi;

namespace {

struct DecodedMsg {
    std::vector<uint8_t> bytes;
    uint32_t timestamp_ms;
};

}  // namespace

TEST_CASE("BleMidiPacketDecoder decodes a single Note On", "[ble-midi]") {
    // Header 0x80 (timestamp_hi = 0), timestamp 0x80 (timestamp_lo = 0),
    // Note On Ch1, note 60, velocity 100.
    const uint8_t packet[] = { 0x80, 0x80, 0x90, 60, 100 };
    std::vector<DecodedMsg> received;
    BleMidiPacketDecoder dec;
    dec.set_message_callback([&](const std::vector<uint8_t>& bytes, uint32_t ts) {
        received.push_back({bytes, ts});
    });
    REQUIRE(dec.decode(packet, sizeof(packet)));
    REQUIRE(received.size() == 1);
    REQUIRE(received[0].bytes == std::vector<uint8_t>{0x90, 60, 100});
    REQUIRE(received[0].timestamp_ms == 0);
}

TEST_CASE("BleMidiPacketDecoder reconstructs 13-bit timestamps", "[ble-midi]") {
    // ts_hi = 0x05 (5 << 7 = 640), ts_lo = 0x42 (0x42 = 66) ⇒ 640+66=706
    const uint8_t packet[] = { 0x85, 0xC2, 0xB0, 7, 127 };
    std::vector<DecodedMsg> received;
    BleMidiPacketDecoder dec;
    dec.set_message_callback([&](const std::vector<uint8_t>& bytes, uint32_t ts) {
        received.push_back({bytes, ts});
    });
    REQUIRE(dec.decode(packet, sizeof(packet)));
    REQUIRE(received.size() == 1);
    REQUIRE(received[0].timestamp_ms == 706);
    REQUIRE(received[0].bytes[0] == 0xB0);  // CC ch1
    REQUIRE(received[0].bytes[1] == 7);     // CC#7 (volume)
    REQUIRE(received[0].bytes[2] == 127);
}

TEST_CASE("BleMidiPacketDecoder honours running status", "[ble-midi]") {
    // Header + ts + status + data1 + data2 + data1' + data2'
    // (second Note On uses the previous 0x90 via running status).
    const uint8_t packet[] = { 0x80, 0x80,
                               0x90, 60, 100,
                                     62, 110 };
    std::vector<DecodedMsg> received;
    BleMidiPacketDecoder dec;
    dec.set_message_callback([&](const std::vector<uint8_t>& bytes, uint32_t ts) {
        received.push_back({bytes, ts});
    });
    REQUIRE(dec.decode(packet, sizeof(packet)));
    REQUIRE(received.size() == 2);
    REQUIRE(received[0].bytes == std::vector<uint8_t>{0x90, 60, 100});
    REQUIRE(received[1].bytes == std::vector<uint8_t>{0x90, 62, 110});
}

TEST_CASE("BleMidiPacketDecoder rejects undersized packets", "[ble-midi]") {
    BleMidiPacketDecoder dec;
    const uint8_t bogus[] = { 0x80, 0x80 };  // header + ts only.
    REQUIRE_FALSE(dec.decode(bogus, sizeof(bogus)));
}

TEST_CASE("BleMidiPacketDecoder reset() clears running status", "[ble-midi]") {
    BleMidiPacketDecoder dec;
    dec.set_message_callback([](const std::vector<uint8_t>&, uint32_t) {});
    const uint8_t initial[] = { 0x80, 0x80, 0x90, 60, 100 };
    REQUIRE(dec.decode(initial, sizeof(initial)));
    dec.reset();
    // A standalone data-only packet without status must now be dropped.
    std::vector<DecodedMsg> received;
    dec.set_message_callback([&](const std::vector<uint8_t>& bytes, uint32_t ts) {
        received.push_back({bytes, ts});
    });
    const uint8_t orphan[] = { 0x80, 0x80, 64, 90 };
    // Decoder accepts the packet shape but emits nothing because the
    // first byte after the timestamp is not a status byte and running
    // status has been cleared.
    (void)dec.decode(orphan, sizeof(orphan));
    REQUIRE(received.empty());
}

TEST_CASE("encode_ble_midi_packet round-trips through the decoder", "[ble-midi]") {
    const uint8_t msg[] = { 0xB0, 1, 64 };  // CC Mod Wheel ch1 = 64.
    auto packet = encode_ble_midi_packet(msg, sizeof(msg), /*timestamp_ms=*/512);
    REQUIRE(packet.size() == 5);
    REQUIRE((packet[0] & 0x80) != 0);
    REQUIRE((packet[1] & 0x80) != 0);

    std::vector<DecodedMsg> received;
    BleMidiPacketDecoder dec;
    dec.set_message_callback([&](const std::vector<uint8_t>& bytes, uint32_t ts) {
        received.push_back({bytes, ts});
    });
    REQUIRE(dec.decode(packet.data(), packet.size()));
    REQUIRE(received.size() == 1);
    REQUIRE(received[0].bytes == std::vector<uint8_t>{0xB0, 1, 64});
    REQUIRE(received[0].timestamp_ms == 512);
}

TEST_CASE("encode_ble_midi_packet wraps timestamps to 13 bits", "[ble-midi]") {
    const uint8_t msg[] = { 0x90, 60, 100 };
    // 0x2001 = 8193 ⇒ wraps to 1 (top 13 bits).
    auto packet = encode_ble_midi_packet(msg, sizeof(msg), 0x2001);
    BleMidiPacketDecoder dec;
    std::vector<DecodedMsg> received;
    dec.set_message_callback([&](const std::vector<uint8_t>& bytes, uint32_t ts) {
        received.push_back({bytes, ts});
    });
    REQUIRE(dec.decode(packet.data(), packet.size()));
    REQUIRE(received.size() == 1);
    REQUIRE(received[0].timestamp_ms == 1);
}

TEST_CASE("encode_ble_midi_packet rejects null input", "[ble-midi]") {
    auto packet = encode_ble_midi_packet(nullptr, 0, 100);
    REQUIRE(packet.empty());
}

TEST_CASE("BleMidiUuids carries the BLE-MIDI 1.0 well-known UUIDs",
          "[ble-midi]") {
    // Apple BLE-MIDI 1.0 spec — these are the only valid values and
    // the test pins them so a refactor of the constants cannot
    // silently break interop with paired peripherals in the wild.
    REQUIRE(std::string(BleMidiUuids::kService) ==
            "03B80E5A-EDE8-4B33-A751-6CE34EC4C700");
    REQUIRE(std::string(BleMidiUuids::kCharacteristic) ==
            "7772E5DB-3868-4112-A1A9-F2669D106BF3");
}

TEST_CASE("create_ble_midi_central always returns a usable instance",
          "[ble-midi]") {
    auto central = create_ble_midi_central();
    REQUIRE(central != nullptr);
    // Whether the backend is available is platform-dependent; the
    // contract is that the call doesn't crash, scan/connect are
    // safe no-ops, and known_peripherals starts empty.
    REQUIRE(central->known_peripherals().empty());
    REQUIRE_FALSE(central->is_scanning());
    // Unsupported backends report state through the callback.
    bool saw_failed = false;
    BleMidiError observed_err = BleMidiError::None;
    central->set_state_callback(
        [&](const std::string&, BleMidiConnectionState state, BleMidiError err) {
            if (state == BleMidiConnectionState::Failed) {
                saw_failed = true;
                observed_err = err;
            }
        });
    // connect() against a never-seen peripheral must fail. The stub
    // central reports Unsupported (no backend); the real CoreBluetooth
    // central reports PeripheralNotFound (we never saw the id). Both
    // are valid Failed paths — assert we got Failed at all and that
    // the error explains the failure.
    REQUIRE_FALSE(central->connect("missing-id-never-seen"));
    REQUIRE(saw_failed);
    REQUIRE((observed_err == BleMidiError::Unsupported ||
             observed_err == BleMidiError::PeripheralNotFound));
}
