#pragma once

// BLE MIDI — Bluetooth Low Energy MIDI 1.0 transport (Apple BLE-MIDI 1.0 spec,
// also adopted by the MIDI Association in 2015). This header declares the
// cross-platform discovery / connection / I/O surface that backends implement.
//
// Backends:
//   macOS / iOS:  CoreBluetooth (CBCentralManager + CBPeripheral) bridged
//                 into CoreMIDI via the well-known BLE-MIDI service UUID
//                 03B80E5A-EDE8-4B33-A751-6CE34EC4C700. On Apple the
//                 CoreMIDI driver also exposes paired BLE peripherals as
//                 regular MIDI endpoints; this surface adds explicit
//                 scan-and-pair control for unpaired peripherals.
//   Linux:        BlueZ (org.bluez.GattCharacteristic1 notifications) +
//                 ALSA virtual port for output. Scaffold only in this
//                 slice — the BlueZ D-Bus glue lands in a follow-up.
//   Windows:      WinRT BluetoothLEAdvertisementWatcher +
//                 GattCharacteristic.ValueChanged. Scaffold only.
//   Android:      BluetoothLeScanner + BluetoothGatt notification
//                 via the existing JNI bridge. Scaffold only.
//
// Spec reference (BLE-MIDI 1.0):
//   Service UUID:        03B80E5A-EDE8-4B33-A751-6CE34EC4C700
//   Characteristic UUID: 7772E5DB-3868-4112-A1A9-F2669D106BF3 (read/write/notify)
//
// Packet format (per 20-byte MTU minimum):
//   header  = 0x80 | (timestamp_high & 0x3F)            (top 6 bits of 13-bit ms)
//   tstmp   = 0x80 | (timestamp_low  & 0x7F)            (bottom 7 bits)
//   [status, data1, data2, ...]
//   Running status, multi-message-per-packet, and split-sysex follow the
//   Apple spec — backends MUST reassemble before delivering to callbacks.
//
// Acceptance for the initial slice: discover BLE MIDI peripherals,
// connect to one, receive MIDI events (notes) via the standard
// MidiInputCallback path. Pairing UI is delegated to the platform —
// the scanner surfaces peripherals and the host app drives the
// pairing dialog (iOS CABTMIDICentralViewController, macOS
// equivalent, or a custom widget).

#include <pulp/midi/device.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulp::midi {

/// One BLE peripheral that advertises the BLE-MIDI service. Identified
/// by a backend-specific string (CBPeripheral.identifier UUID on Apple,
/// MAC address on Linux/Android, device-id on Windows). The host app
/// uses the id to call connect(); the human-readable name is for UI.
struct BleMidiPeripheral {
    std::string id;
    std::string name;
    /// Last-seen RSSI in dBm. 0 if the backend doesn't report it.
    int rssi = 0;
    /// Monotonic time the peripheral was last seen in a scan response.
    /// Backends use steady_clock so the value is comparable across
    /// scan cycles.
    std::chrono::steady_clock::time_point last_seen{};
    /// True if the OS already has a bond/pairing for this peripheral.
    /// Apple exposes this via CBCentralManager.retrievePeripherals; on
    /// other platforms backends may report false until paired.
    bool is_paired = false;
};

/// Connection state for an attempted BLE pairing. Reported on the
/// callback registered with BleMidiCentral::set_state_callback().
enum class BleMidiConnectionState {
    Idle,
    Scanning,
    Connecting,
    Connected,
    Disconnected,
    Failed,
};

/// Backend-reported error code. Backends translate platform errors
/// (CBError, BlueZ D-Bus errors, WinRT HRESULT, etc.) into this
/// neutral enum so cross-platform tests can assert without including
/// per-OS headers.
enum class BleMidiError {
    None,
    PermissionDenied,    /// User declined Bluetooth permission.
    BluetoothOff,        /// Adapter is off / unavailable.
    Unsupported,         /// Platform has no BLE MIDI backend wired.
    PeripheralNotFound,  /// Scan never produced the requested id.
    ConnectFailed,       /// CB / GATT connect attempt failed.
    ServiceNotFound,     /// Peripheral does not expose BLE-MIDI service.
    Timeout,             /// Connection or service discovery timed out.
};

/// Callback fired each time the scanner observes a peripheral that
/// advertises the BLE-MIDI service UUID. May be invoked from a
/// backend thread; consumers post to the UI thread themselves.
using BleMidiScanCallback = std::function<void(const BleMidiPeripheral&)>;

/// Callback for connection-state changes against a specific peripheral
/// id. Drives the pairing UI (Connecting → Connected | Failed).
using BleMidiStateCallback =
    std::function<void(const std::string& peripheral_id,
                       BleMidiConnectionState state,
                       BleMidiError error)>;

/// Cross-platform BLE MIDI central. Owns the scan + connect surface.
/// The actual MIDI byte stream, once connected, is delivered through
/// the regular MidiInput interface acquired via create_input(id) so
/// existing callers consume BLE MIDI events identically to USB MIDI.
class BleMidiCentral {
public:
    virtual ~BleMidiCentral() = default;

    /// True if the running platform has a BLE MIDI backend compiled
    /// in AND the user/system has granted Bluetooth permission. On
    /// platforms with only a scaffold backend, returns false.
    virtual bool is_available() const = 0;

    /// Start scanning. The callback fires once per peripheral
    /// observation (the backend deduplicates by id and refreshes the
    /// last_seen timestamp). Calling start_scan() while already
    /// scanning is a no-op. Backends MUST honour the OS-level scan
    /// duty-cycle limits; consumers should still call stop_scan() when
    /// the UI is dismissed.
    virtual bool start_scan(BleMidiScanCallback callback) = 0;
    virtual void stop_scan() = 0;
    virtual bool is_scanning() const = 0;

    /// Snapshot of peripherals currently known to the central. Includes
    /// previously paired peripherals returned by the OS even when the
    /// scanner has not yet seen an advertisement in the current scan
    /// cycle (Apple retrievePeripherals semantics).
    virtual std::vector<BleMidiPeripheral> known_peripherals() const = 0;

    /// Begin a connection. Connection progress arrives on the state
    /// callback. Once state == Connected the peripheral is registered
    /// with CoreMIDI / ALSA / WinRT and surfaces as a regular MIDI
    /// port the host can open through MidiSystem::create_input().
    virtual bool connect(const std::string& peripheral_id) = 0;
    virtual void disconnect(const std::string& peripheral_id) = 0;

    virtual void set_state_callback(BleMidiStateCallback callback) = 0;

    /// MIDI port-id that MidiSystem::enumerate_inputs() returns once
    /// the peripheral is Connected. Returns empty string if the
    /// peripheral is not connected (or the backend doesn't expose
    /// the mapping). Lets the UI pre-select the BLE port in a port-
    /// picker without polling enumerate_inputs() until it appears.
    virtual std::string midi_input_port_for(const std::string& peripheral_id) const = 0;
    virtual std::string midi_output_port_for(const std::string& peripheral_id) const = 0;
};

/// Create the BLE MIDI central for the current platform. Returns a
/// scaffold central on platforms without a real backend; is_available()
/// returns false on those builds so callers can degrade gracefully.
std::unique_ptr<BleMidiCentral> create_ble_midi_central();

// ── BLE-MIDI 1.0 packet codec ──────────────────────────────────────────────
//
// The codec is cross-platform and Skia-free — every backend reuses it.
// Reassembles 13-bit timestamps, running status, and multi-message
// packets per the BLE-MIDI 1.0 spec. Exposed for tests + for the BlueZ
// / WinRT backends which receive raw GATT bytes and must produce
// MidiEvents.

/// The well-known service / characteristic UUIDs (16-byte big-endian).
struct BleMidiUuids {
    static constexpr const char* kService =
        "03B80E5A-EDE8-4B33-A751-6CE34EC4C700";
    static constexpr const char* kCharacteristic =
        "7772E5DB-3868-4112-A1A9-F2669D106BF3";
};

/// Stateful decoder. Feed BLE GATT notification bytes via decode();
/// each decoded MIDI message arrives on the registered callback with
/// a millisecond timestamp reconstructed from the BLE packet header.
class BleMidiPacketDecoder {
public:
    using MessageCallback =
        std::function<void(const std::vector<uint8_t>& bytes,
                           uint32_t timestamp_ms)>;

    void set_message_callback(MessageCallback cb) { cb_ = std::move(cb); }

    /// Feed one notification packet. Returns true if the packet was
    /// well-formed (header + at least one timestamp byte + status).
    /// Malformed packets are dropped to satisfy spec section 3 — the
    /// decoder does not throw and recovers on the next packet.
    bool decode(const uint8_t* data, std::size_t size);

    /// Reset running status + the in-flight sysex buffer. Call after a
    /// disconnect or when the host knows the stream has been
    /// interrupted (e.g. OS sleep/wake).
    void reset();

private:
    MessageCallback cb_;
    uint8_t last_status_ = 0;
    /// In-flight sysex bytes. Spec allows sysex to span multiple BLE
    /// packets; we accumulate until the terminating 0xF7.
    std::vector<uint8_t> sysex_buffer_;
    /// High 6 bits of the last reconstructed timestamp.
    uint8_t last_header_ts_ = 0;
};

/// Encode a single MIDI message into a BLE-MIDI packet using
/// timestamp `timestamp_ms` (truncated to 13 bits per the spec).
/// Returns the encoded bytes; appends header + timestamp + message.
/// Sysex callers should chunk to MTU - 1 bytes per call.
std::vector<uint8_t> encode_ble_midi_packet(const uint8_t* msg,
                                            std::size_t size,
                                            uint32_t timestamp_ms);

}  // namespace pulp::midi
