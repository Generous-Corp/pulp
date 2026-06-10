// BLE MIDI tests — packet decoder + cross-platform factory contract.
//
// Headless coverage for the BLE-MIDI 1.0 packet codec and the
// stub central. The CoreBluetooth backend requires a real
// Bluetooth adapter and an OS-granted permission dialog, so its
// live path is exercised by the documented manual smoke (see
// planning gap-doc entry "BLE MIDI"); these tests pin the
// cross-platform half that every backend reuses.

#include <pulp/midi/ble_midi.hpp>
#include <pulp/midi/ble_midi_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
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

// Regression: Codex PR #3017 P2. Running status is scoped to a single
// BLE-MIDI packet per Apple BLE-MIDI 1.0 §3.4. Carrying it across
// packets would fabricate events from leading data bytes in the next
// packet. The first packet establishes 0x90 running status; the second
// arrives with NO status byte (just timestamp + data) and must NOT emit
// a Note On using the prior packet's 0x90.
TEST_CASE("BleMidiPacketDecoder clears running status at packet boundaries "
          "(regression: PR #3017 review)",
          "[ble-midi][issue-3017]") {
    BleMidiPacketDecoder dec;
    std::vector<DecodedMsg> received;
    dec.set_message_callback([&](const std::vector<uint8_t>& bytes, uint32_t ts) {
        received.push_back({bytes, ts});
    });

    // Packet 1 — Note On using explicit status, sets running status.
    const uint8_t packet1[] = { 0x80, 0x80, 0x90, 60, 100 };
    REQUIRE(dec.decode(packet1, sizeof(packet1)));
    REQUIRE(received.size() == 1);
    REQUIRE(received.back().bytes == std::vector<uint8_t>{0x90, 60, 100});

    received.clear();

    // Packet 2 — header + timestamp + ORPHAN data bytes (no status). If
    // running status leaked across packets, the decoder would emit a
    // fabricated Note On {0x90, 64, 90} here. It must emit nothing.
    const uint8_t packet2[] = { 0x80, 0x80, 64, 90 };
    (void)dec.decode(packet2, sizeof(packet2));
    REQUIRE(received.empty());
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

TEST_CASE("create_ble_midi_central is_available() is callable and honest",
          "[ble-midi]") {
    // is_available() must never crash and must report a definite bool on
    // every platform. On macOS CI without a granted Bluetooth permission /
    // powered adapter the CoreBluetooth backend reports false; the Windows
    // WinRT scan backend reports true only when the BLE advertisement API
    // is usable; the stub reports false. The contract under test is simply
    // that the call is safe and that an unavailable backend keeps its
    // scan / port surface as honest no-ops.
    auto central = create_ble_midi_central();
    REQUIRE(central != nullptr);
    const bool available = central->is_available();
    if (!available) {
        // An unavailable backend must keep the scan surface inert: starting
        // a scan must not flip is_scanning() on, and there are no known
        // peripherals or port mappings to surface.
        REQUIRE_FALSE(central->start_scan([](const BleMidiPeripheral&) {}));
        REQUIRE_FALSE(central->is_scanning());
        REQUIRE(central->known_peripherals().empty());
        REQUIRE(central->midi_input_port_for("any-id").empty());
        REQUIRE(central->midi_output_port_for("any-id").empty());
    }
    // stop_scan() is always safe, scanning or not.
    central->stop_scan();
    REQUIRE_FALSE(central->is_scanning());
}

// ── BleMidiPortRegistry — the off-Apple port-merge seam ──────────────────────
//
// Cross-platform + transport-free, so it is unit-testable on every host (no
// BlueZ / Bluetooth hardware needed). These tests pin the bridge the BlueZ
// central populates on connect and the ALSA MidiSystem merges into its
// enumeration. Each test uses a unique peripheral id so the process-wide
// singleton stays isolated, and unregisters at the end.

namespace {
const char* find_port(const std::vector<MidiPortInfo>& ports,
                      const std::string& id) {
    for (const auto& p : ports)
        if (p.id == id) return p.name.c_str();
    return nullptr;
}
}  // namespace

TEST_CASE("BleMidiPortRegistry surfaces a registered input in list_inputs",
          "[ble-midi][registry]") {
    auto& reg = BleMidiPortRegistry::instance();
    const std::string id = "ble-midi-in:/test/dev_AA_registry_in";
    reg.register_input(id, "Reg Test Keyboard");

    REQUIRE(reg.is_input(id));
    REQUIRE_FALSE(reg.is_output(id));
    auto inputs = reg.list_inputs();
    const char* name = find_port(inputs, id);
    REQUIRE(name != nullptr);
    REQUIRE(std::string(name) == "Reg Test Keyboard");

    reg.unregister_input(id);
    REQUIRE_FALSE(reg.is_input(id));
    REQUIRE(find_port(reg.list_inputs(), id) == nullptr);
}

TEST_CASE("BleMidiPortRegistry delivers a decoded short message to the attached "
          "MidiInput callback", "[ble-midi][registry]") {
    auto& reg = BleMidiPortRegistry::instance();
    const std::string id = "ble-midi-in:/test/dev_BB_registry_deliver";
    reg.register_input(id, "Deliver Test");

    std::vector<MidiEvent> events;
    std::vector<std::vector<uint8_t>> sysex;
    REQUIRE(reg.attach_input(
        id,
        [&](const MidiEvent& e) { events.push_back(e); },
        [&](const std::vector<uint8_t>& bytes, double) { sysex.push_back(bytes); }));

    // A Note On Ch1 note 60 vel 100 — the bytes the central's decoder produces.
    reg.deliver_message(id, {0x90, 60, 100}, /*timestamp_sec=*/0.5);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].is_note_on());
    REQUIRE(events[0].note() == 60);
    REQUIRE(events[0].velocity() == 100);
    REQUIRE(events[0].timestamp == 0.5);
    REQUIRE(sysex.empty());

    // After detach, no further delivery reaches the callback.
    reg.detach_input(id);
    reg.deliver_message(id, {0x90, 62, 110}, 0.6);
    REQUIRE(events.size() == 1);

    reg.unregister_input(id);
}

TEST_CASE("BleMidiPortRegistry routes a SysEx payload to the sysex callback",
          "[ble-midi][registry]") {
    auto& reg = BleMidiPortRegistry::instance();
    const std::string id = "ble-midi-in:/test/dev_CC_registry_sysex";
    reg.register_input(id, "SysEx Test");

    std::vector<MidiEvent> events;
    std::vector<std::vector<uint8_t>> sysex;
    REQUIRE(reg.attach_input(
        id,
        [&](const MidiEvent& e) { events.push_back(e); },
        [&](const std::vector<uint8_t>& bytes, double) { sysex.push_back(bytes); }));

    const std::vector<uint8_t> msg = {0xF0, 0x7E, 0x00, 0x06, 0x01, 0xF7};
    reg.deliver_message(id, msg, 1.0);
    REQUIRE(events.empty());
    REQUIRE(sysex.size() == 1);
    REQUIRE(sysex[0] == msg);

    reg.unregister_input(id);
}

TEST_CASE("BleMidiPortRegistry output sink forwards sent bytes to the central",
          "[ble-midi][registry]") {
    auto& reg = BleMidiPortRegistry::instance();
    const std::string id = "ble-midi-out:/test/dev_DD_registry_out";
    std::vector<std::vector<uint8_t>> written;
    reg.register_output(id, "Out Test",
                        [&](const std::vector<uint8_t>& b) { written.push_back(b); });

    REQUIRE(reg.is_output(id));
    auto outputs = reg.list_outputs();
    const char* name = find_port(outputs, id);
    REQUIRE(name != nullptr);
    REQUIRE(std::string(name) == "Out Test");

    auto sink = reg.output_sink(id);
    REQUIRE(static_cast<bool>(sink));
    sink({0xB0, 7, 64});
    REQUIRE(written.size() == 1);
    REQUIRE(written[0] == std::vector<uint8_t>{0xB0, 7, 64});

    reg.unregister_output(id);
    REQUIRE_FALSE(reg.is_output(id));
    REQUIRE_FALSE(static_cast<bool>(reg.output_sink(id)));
}

TEST_CASE("BleMidiPortRegistry deliver_message is a no-op for unknown / unopened "
          "ports", "[ble-midi][registry]") {
    auto& reg = BleMidiPortRegistry::instance();
    // Never registered — must not crash, must not deliver.
    reg.deliver_message("ble-midi-in:/test/never-registered", {0x90, 60, 100}, 0.0);

    // Registered but no host callback attached — also a safe no-op.
    const std::string id = "ble-midi-in:/test/dev_EE_unopened";
    reg.register_input(id, "Unopened");
    reg.deliver_message(id, {0x90, 60, 100}, 0.0);  // attach_input never called
    REQUIRE(reg.is_input(id));
    reg.unregister_input(id);
}

TEST_CASE("BleMidiPortRegistry attach_input rejects an unregistered port",
          "[ble-midi][registry]") {
    auto& reg = BleMidiPortRegistry::instance();
    REQUIRE_FALSE(reg.attach_input(
        "ble-midi-in:/test/never-registered-attach",
        [](const MidiEvent&) {}, [](const std::vector<uint8_t>&, double) {}));
}
