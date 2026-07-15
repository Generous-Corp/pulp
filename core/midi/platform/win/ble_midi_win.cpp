// BLE MIDI backend — Windows using WinRT Bluetooth-LE advertisement scan
// + GATT connect/notify.
//
// Implements pulp::midi::BleMidiCentral on Windows.
//
//   • Scan: a BluetoothLEAdvertisementWatcher filtered to the BLE-MIDI 1.0
//     service UUID (03B80E5A-EDE8-4B33-A751-6CE34EC4C700) surfaces
//     advertising peripherals through the scan callback.
//   • Connect: FromBluetoothAddressAsync resolves the peripheral, we locate
//     the BLE-MIDI service + characteristic, write the CCCD to enable Notify,
//     and subscribe ValueChanged. The notify bytes feed a per-connection
//     BleMidiPacketDecoder whose decoded messages are published to the
//     BleMidiPortRegistry, which the Windows MidiSystem (winmidi_device.cpp)
//     merges into its port list. Output: encode_ble_midi_packet →
//     GattCharacteristic.WriteValueAsync.
//
// Why the base Windows SDK is enough here (unlike the WinRT MIDI 2.0
// backend in winrt_midi_device.cpp): BLE GATT lives in
// Windows.Devices.Bluetooth / Windows.Devices.Bluetooth.Advertisement /
// Windows.Devices.Bluetooth.GenericAttributeProfile and the byte-buffer
// helpers in Windows.Storage.Streams, all of which ship in the BASE Windows
// SDK cppwinrt projection. There is no out-of-band NuGet / vcpkg
// dependency — we link the OS umbrella import library (WindowsApp) and
// include the stock winrt headers.
//
// Pairing: this backend never drives pairing
// (DeviceInformationPairing.PairAsync is unsupported in desktop apps and
// rejected for packaged identity); BLE-MIDI peripherals pair through the OS
// Settings/Swift-pair flow. Inside a DAW the process identity is the host's,
// so any pairing prompt is attributed to the host, not Pulp.
//
// Threading: the watcher's Received event and the characteristic's
// ValueChanged event fire on WinRT thread-pool threads. The BleMidiScanCallback
// / decoder-delivery contract already permits invocation from a backend
// thread, so we forward directly under a short mutex hold that protects the
// peripheral map + the stored callbacks (matching the CoreBluetooth /
// winrt_midi_device.cpp discipline). The async GATT chain is co_awaited via
// blocking `.get()` because connect() runs on a caller worker thread, not a
// UI/STA thread. Apartment init is lazy + MTA, the same pattern
// winrt_midi_device.cpp uses.

#if defined(_WIN32)

#include <pulp/midi/ble_midi.hpp>
#include <pulp/midi/ble_midi_registry.hpp>
#include <pulp/runtime/log.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>

namespace pulp::midi {
namespace {

namespace wf   = ::winrt::Windows::Foundation;
namespace wda  = ::winrt::Windows::Devices::Bluetooth::Advertisement;
namespace wdb  = ::winrt::Windows::Devices::Bluetooth;
namespace gatt = ::winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
namespace wss  = ::winrt::Windows::Storage::Streams;

// Lazy, idempotent MTA apartment init. WinRT apartment state is per-thread, so
// each thread that enters the backend must attempt init once.
void ensure_winrt_init() {
    thread_local bool attempted = false;
    if (attempted) return;
    attempted = true;
    try {
        ::winrt::init_apartment(::winrt::apartment_type::multi_threaded);
    } catch (const ::winrt::hresult_error&) {
        // Already initialized on this thread with a different model, or
        // COM already up — both are fine for our consumer-only usage.
    }
}

// Parse the canonical 8-4-4-4-12 UUID string used by BleMidiUuids into a
// winrt::guid. The advertisement-filter ServiceUuids list and the
// per-advertisement ServiceUuids comparison both work in winrt::guid.
::winrt::guid guid_from_uuid_string(const char* uuid) {
    // Strip hyphens into 32 hex nibbles.
    char hex[33] = {};
    int n = 0;
    for (const char* p = uuid; *p && n < 32; ++p) {
        if (*p == '-') continue;
        hex[n++] = *p;
    }

    auto take = [&](int offset, int count) -> uint64_t {
        char buf[17] = {};
        for (int i = 0; i < count; ++i) buf[i] = hex[offset + i];
        return std::strtoull(buf, nullptr, 16);
    };

    ::winrt::guid g{};
    g.Data1 = static_cast<uint32_t>(take(0, 8));
    g.Data2 = static_cast<uint16_t>(take(8, 4));
    g.Data3 = static_cast<uint16_t>(take(12, 4));
    // Data4 is the final 8 bytes (clock-seq + node), big-endian in the
    // string form. hex[16..31] = 16 nibbles = 8 bytes.
    for (int i = 0; i < 8; ++i) {
        g.Data4[i] = static_cast<uint8_t>(take(16 + i * 2, 2));
    }
    return g;
}

// Format a 48-bit Bluetooth address (as delivered by WinRT) into a stable
// hex id string. The address is the cross-scan handle the host passes back
// to connect(); the format mirrors the colon-free hex other backends use
// for their device ids so the UI can treat it opaquely.
std::string address_to_id(uint64_t address) {
    char buf[16] = {};
    std::snprintf(buf, sizeof(buf), "%012llx",
                  static_cast<unsigned long long>(address));
    return std::string(buf);
}

// Parse the 12-hex peripheral id produced by address_to_id() back into the
// 48-bit Bluetooth address WinRT's FromBluetoothAddressAsync expects. Returns
// false if the id is not a clean run of hex nibbles (defends connect() against
// an id that never came from a scan).
bool address_from_id(const std::string& id, uint64_t& out) {
    if (id.empty() || id.size() > 16) return false;
    uint64_t value = 0;
    for (char c : id) {
        int nibble;
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
        else return false;
        value = (value << 4) | static_cast<uint64_t>(nibble);
    }
    out = value;
    return true;
}

// Registry port-ids — same "ble-midi-in:" / "ble-midi-out:<peripheral-id>"
// convention the Apple + BlueZ backends use, so a host can pre-select the
// port via midi_input_port_for() before enumerate_inputs() shows it.
std::string input_port_id(const std::string& peripheral_id) {
    return "ble-midi-in:" + peripheral_id;
}
std::string output_port_id(const std::string& peripheral_id) {
    return "ble-midi-out:" + peripheral_id;
}

// Translate a WinRT GATT failure into the neutral BleMidiError enum so
// cross-platform callers never see an HRESULT. Reached independently from the
// public WinRT GATT documentation (GattCommunicationStatus + the common
// HRESULT facilities), not transcribed from another framework.
BleMidiError gatt_status_to_ble(gatt::GattCommunicationStatus status) {
    switch (status) {
        case gatt::GattCommunicationStatus::Unreachable:
            return BleMidiError::ConnectFailed;
        case gatt::GattCommunicationStatus::ProtocolError:
            return BleMidiError::ServiceNotFound;
        case gatt::GattCommunicationStatus::AccessDenied:
            return BleMidiError::PermissionDenied;
        default:
            return BleMidiError::ConnectFailed;
    }
}

BleMidiError hresult_to_ble(const ::winrt::hresult_error& e) {
    // E_ACCESSDENIED → permission; RPC_E_DISCONNECTED / adapter-off surface as
    // a generic connect failure. Keep the mapping conservative; the precise
    // facility codes are not part of the cross-platform contract.
    const auto code = static_cast<uint32_t>(e.code());
    if (code == 0x80070005u /* E_ACCESSDENIED */) {
        return BleMidiError::PermissionDenied;
    }
    return BleMidiError::ConnectFailed;
}

class WinBleMidiCentral final : public BleMidiCentral {
public:
    WinBleMidiCentral() { ensure_winrt_init(); }

    ~WinBleMidiCentral() override {
        stop_scan();
        // Tear down every live GATT link so the registry's output sink (which
        // captures `this`) and the inbound decoder are dropped before the
        // central is destroyed. Snapshot the ids under the lock, then call the
        // public disconnect() path (which re-locks) for each.
        std::vector<std::string> ids;
        {
            std::lock_guard<std::mutex> lock(mu_);
            ids.reserve(connections_.size());
            for (const auto& [id, conn] : connections_) ids.push_back(id);
        }
        for (const auto& id : ids) disconnect(id);
    }

    // True if the WinRT BLE advertisement API is usable on this machine.
    // Constructing a BluetoothLEAdvertisementWatcher does not require an
    // adapter to be present, but a missing Bluetooth stack throws an
    // hresult_error here — treat any throw as "unavailable" so callers
    // degrade gracefully (matching the stub central's contract).
    bool is_available() const override {
        ensure_winrt_init();
        try {
            wda::BluetoothLEAdvertisementWatcher probe{};
            (void)probe;
            return true;
        } catch (const ::winrt::hresult_error&) {
            return false;
        }
    }

    bool start_scan(BleMidiScanCallback cb) override {
        ensure_winrt_init();
        if (scanning_.load()) return true;  // start while scanning == no-op
        {
            std::lock_guard<std::mutex> lock(mu_);
            scan_cb_ = std::move(cb);
        }
        try {
            watcher_ = wda::BluetoothLEAdvertisementWatcher{};
            // Active scanning solicits the scan-response payload, which is
            // where the human-readable local name usually rides.
            watcher_.ScanningMode(wda::BluetoothLEScanningMode::Active);

            // Filter to BLE-MIDI advertisements only. The watcher matches
            // when the advertised ServiceUuids list contains our service.
            const ::winrt::guid service_guid =
                guid_from_uuid_string(BleMidiUuids::kService);
            watcher_.AdvertisementFilter().Advertisement()
                .ServiceUuids().Append(service_guid);

            received_token_ = watcher_.Received(
                { this, &WinBleMidiCentral::on_received });

            watcher_.Start();
        } catch (const ::winrt::hresult_error& e) {
            runtime::log_error("WinRT BLE MIDI: failed to start scan: {}",
                               ::winrt::to_string(e.message()));
            teardown_watcher();
            return false;
        }
        scanning_.store(true);
        return true;
    }

    void stop_scan() override {
        ensure_winrt_init();
        if (!scanning_.exchange(false)) return;
        teardown_watcher();
    }

    bool is_scanning() const override { return scanning_.load(); }

    std::vector<BleMidiPeripheral> known_peripherals() const override {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<BleMidiPeripheral> out;
        out.reserve(peripherals_.size());
        for (const auto& [id, snap] : peripherals_) out.push_back(snap);
        return out;
    }

    // Open a GATT link to a peripheral seen in a scan, enable BLE-MIDI
    // characteristic notifications, and publish the connection into the
    // BleMidiPortRegistry so the Windows MidiSystem enumerates it.
    bool connect(const std::string& id) override {
        ensure_winrt_init();

        // Scan-before-connect is mandatory: FromBluetoothAddressAsync returns
        // null for an unpaired device unless the advertisement cache is warm.
        // Require the id to have come from a scan (it is in peripherals_).
        uint64_t address = 0;
        bool missing_peripheral = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (peripherals_.find(id) == peripherals_.end() ||
                !address_from_id(id, address)) {
                missing_peripheral = true;
            }
            if (connections_.find(id) != connections_.end()) {
                return true;  // already connected — no-op.
            }
        }
        if (missing_peripheral) {
            fire_state(id, BleMidiConnectionState::Failed,
                       BleMidiError::PeripheralNotFound);
            return false;
        }

        fire_state(id, BleMidiConnectionState::Connecting, BleMidiError::None);

        try {
            // Resolve the peripheral from the cached advertisement.
            wdb::BluetoothLEDevice device =
                wdb::BluetoothLEDevice::FromBluetoothAddressAsync(address).get();
            if (!device) {
                fire_state(id, BleMidiConnectionState::Failed,
                           BleMidiError::PeripheralNotFound);
                return false;
            }

            // Discover the BLE-MIDI service.
            const ::winrt::guid service_guid =
                guid_from_uuid_string(BleMidiUuids::kService);
            gatt::GattDeviceServicesResult services =
                device.GetGattServicesForUuidAsync(service_guid).get();
            if (services.Status() != gatt::GattCommunicationStatus::Success ||
                services.Services().Size() == 0) {
                fire_state(id, BleMidiConnectionState::Failed,
                           services.Status() ==
                                   gatt::GattCommunicationStatus::Success
                               ? BleMidiError::ServiceNotFound
                               : gatt_status_to_ble(services.Status()));
                return false;
            }
            gatt::GattDeviceService service = services.Services().GetAt(0);

            // Discover the BLE-MIDI characteristic.
            const ::winrt::guid char_guid =
                guid_from_uuid_string(BleMidiUuids::kCharacteristic);
            gatt::GattCharacteristicsResult chars =
                service.GetCharacteristicsForUuidAsync(char_guid).get();
            if (chars.Status() != gatt::GattCommunicationStatus::Success ||
                chars.Characteristics().Size() == 0) {
                fire_state(id, BleMidiConnectionState::Failed,
                           chars.Status() ==
                                   gatt::GattCommunicationStatus::Success
                               ? BleMidiError::ServiceNotFound
                               : gatt_status_to_ble(chars.Status()));
                return false;
            }
            gatt::GattCharacteristic characteristic =
                chars.Characteristics().GetAt(0);

            // Enable notifications by writing the Client Characteristic
            // Configuration Descriptor (CCCD).
            gatt::GattCommunicationStatus cccd =
                characteristic
                    .WriteClientCharacteristicConfigurationDescriptorAsync(
                        gatt::GattClientCharacteristicConfigurationDescriptorValue::
                            Notify)
                    .get();
            if (cccd != gatt::GattCommunicationStatus::Success) {
                fire_state(id, BleMidiConnectionState::Failed,
                           gatt_status_to_ble(cccd));
                return false;
            }

            // Build the connection state (decoder + WinRT object lifetimes).
            auto conn = std::make_unique<Connection>();
            conn->device = device;
            conn->service = service;
            conn->characteristic = characteristic;
            conn->decoder = std::make_unique<BleMidiPacketDecoder>();
            conn->input_port = input_port_id(id);
            conn->output_port = output_port_id(id);
            conn->decoder->set_message_callback(
                [this, port = conn->input_port](
                    const std::vector<uint8_t>& bytes, uint32_t ts_ms) {
                    BleMidiPortRegistry::instance().deliver_message(
                        port, bytes, static_cast<double>(ts_ms) / 1000.0);
                });

            // ValueChanged carries the GATT notify bytes (the inbound MIDI
            // stream). Fires on a WinRT thread-pool thread.
            conn->value_token = characteristic.ValueChanged(
                { this, &WinBleMidiCentral::on_value_changed });

            // ConnectionStatusChanged surfaces an unexpected link drop so the
            // host sees Disconnected rather than a silently dead port.
            conn->status_token = device.ConnectionStatusChanged(
                { this, &WinBleMidiCentral::on_connection_status_changed });

            const std::string name = peripheral_name(id);
            {
                std::lock_guard<std::mutex> lock(mu_);
                char_to_id_[char_key(characteristic)] = id;
                device_to_id_[device_key(device)] = id;
                connections_[id] = std::move(conn);
            }

            // Publish into the registry so the Windows MidiSystem enumerates
            // the port and routes open() through the registry.
            auto& reg = BleMidiPortRegistry::instance();
            reg.register_input(input_port_id(id), name);
            reg.register_output(
                output_port_id(id), name,
                [this, id](const std::vector<uint8_t>& bytes) {
                    gatt_write(id, bytes);
                });

            fire_state(id, BleMidiConnectionState::Connected,
                       BleMidiError::None);
            return true;
        } catch (const ::winrt::hresult_error& e) {
            runtime::log_error("WinRT BLE MIDI: connect failed: {}",
                               ::winrt::to_string(e.message()));
            fire_state(id, BleMidiConnectionState::Failed, hresult_to_ble(e));
            return false;
        }
    }

    void disconnect(const std::string& id) override {
        ensure_winrt_init();
        std::unique_ptr<Connection> conn;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = connections_.find(id);
            if (it == connections_.end()) return;
            conn = std::move(it->second);
            connections_.erase(it);
            if (conn) {
                char_to_id_.erase(char_key(conn->characteristic));
                device_to_id_.erase(device_key(conn->device));
            }
        }

        // Stop delivery first so a notify in flight cannot resurrect a stale
        // decoder, then drop the registry ports.
        auto& reg = BleMidiPortRegistry::instance();
        reg.unregister_input(input_port_id(id));
        reg.unregister_output(output_port_id(id));

        if (conn) {
            try {
                if (conn->value_token && conn->characteristic) {
                    conn->characteristic.ValueChanged(conn->value_token);
                }
                if (conn->status_token && conn->device) {
                    conn->device.ConnectionStatusChanged(conn->status_token);
                }
            } catch (const ::winrt::hresult_error&) {
                // Best-effort — the peripheral may already be gone.
            }
            // Releasing the BluetoothLEDevice (and service/characteristic) drops
            // the GATT link; closing the GattSession is implicit on the device's
            // last release.
            conn->characteristic = nullptr;
            conn->service = nullptr;
            conn->device = nullptr;
        }

        fire_state(id, BleMidiConnectionState::Disconnected, BleMidiError::None);
    }

    void set_state_callback(BleMidiStateCallback cb) override {
        std::lock_guard<std::mutex> lock(mu_);
        state_cb_ = std::move(cb);
    }

    std::string midi_input_port_for(const std::string& id) const override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = connections_.find(id);
        if (it == connections_.end()) return {};
        return it->second->input_port;
    }
    std::string midi_output_port_for(const std::string& id) const override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = connections_.find(id);
        if (it == connections_.end()) return {};
        return it->second->output_port;
    }

private:
    // Fired on a WinRT thread-pool thread for each filtered advertisement.
    void on_received(const wda::BluetoothLEAdvertisementWatcher&,
                     const wda::BluetoothLEAdvertisementReceivedEventArgs& args) {
        ensure_winrt_init();
        try {
            BleMidiPeripheral snapshot;
            snapshot.id = address_to_id(args.BluetoothAddress());
            snapshot.name = ::winrt::to_string(args.Advertisement().LocalName());
            snapshot.rssi = args.RawSignalStrengthInDBm();
            snapshot.last_seen = std::chrono::steady_clock::now();
            // The OS pairing/bond state is a separate DeviceInformation query;
            // the advertisement does not carry it, so the scan snapshot reports
            // false. Pairing is driven by the OS, not this backend.
            snapshot.is_paired = false;

            BleMidiScanCallback cb_copy;
            {
                std::lock_guard<std::mutex> lock(mu_);
                peripherals_[snapshot.id] = snapshot;
                cb_copy = scan_cb_;
            }
            if (cb_copy) cb_copy(snapshot);
        } catch (const ::winrt::hresult_error&) {
            // Drop a malformed advertisement rather than tear down the watcher.
        }
    }

    void teardown_watcher() {
        if (!watcher_) return;
        try {
            if (received_token_) {
                watcher_.Received(received_token_);
                received_token_ = {};
            }
            watcher_.Stop();
        } catch (const ::winrt::hresult_error&) {
            // Best-effort — the stack may already be gone.
        }
        watcher_ = nullptr;
    }

    // ── GATT notify (inbound MIDI) ───────────────────────────────────────────

    // Fired on a WinRT thread-pool thread for each characteristic notification.
    // Reads the IBuffer payload into bytes and feeds the per-connection decoder,
    // which delivers decoded MIDI through the registry. Bounced under mu_ to
    // match the winrt_midi_device.cpp / CoreBluetooth discipline.
    void on_value_changed(const gatt::GattCharacteristic& characteristic,
                          const gatt::GattValueChangedEventArgs& args) {
        ensure_winrt_init();
        try {
            wss::IBuffer buffer = args.CharacteristicValue();
            if (!buffer || buffer.Length() == 0) return;

            // DataReader::FromBuffer copies the bytes out of projection memory
            // so we never touch the IBuffer after the event returns.
            wss::DataReader reader = wss::DataReader::FromBuffer(buffer);
            const uint32_t len = buffer.Length();
            std::vector<uint8_t> bytes(len);
            reader.ReadBytes(::winrt::array_view<uint8_t>(
                bytes.data(), bytes.data() + len));

            std::lock_guard<std::mutex> lock(mu_);
            auto cit = char_to_id_.find(char_key(characteristic));
            if (cit == char_to_id_.end()) return;
            auto pit = connections_.find(cit->second);
            if (pit == connections_.end() || !pit->second->decoder) return;
            // decode() under the lock is safe: it only forwards into the
            // registry, which copies callbacks out under its own lock before
            // invoking them.
            pit->second->decoder->decode(bytes.data(), bytes.size());
        } catch (const ::winrt::hresult_error&) {
            // Drop a malformed notification rather than tear down the link.
        }
    }

    // Fired when the OS link state flips. Surface an unexpected drop as
    // Disconnected; the registry ports are torn down on the explicit
    // disconnect() path so a transient flap does not desync the port list.
    void on_connection_status_changed(const wdb::BluetoothLEDevice& device,
                                      const wf::IInspectable&) {
        ensure_winrt_init();
        try {
            if (device.ConnectionStatus() !=
                wdb::BluetoothConnectionStatus::Disconnected) {
                return;
            }
        } catch (const ::winrt::hresult_error&) {
            return;
        }
        std::string id;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = device_to_id_.find(device_key(device));
            if (it == device_to_id_.end()) return;
            id = it->second;
        }
        // Only surface state; full teardown happens when the host calls
        // disconnect(). Reconnect attempts are the host's policy.
        fire_state(id, BleMidiConnectionState::Disconnected,
                   BleMidiError::None);
    }

    // ── GATT write (outbound MIDI) ───────────────────────────────────────────

    void gatt_write(const std::string& id,
                    const std::vector<uint8_t>& midi_bytes) {
        ensure_winrt_init();
        if (midi_bytes.empty()) return;
        gatt::GattCharacteristic characteristic{nullptr};
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = connections_.find(id);
            if (it == connections_.end() || !it->second) return;
            characteristic = it->second->characteristic;
        }
        if (!characteristic) return;

        // Encode to a single timestamped BLE-MIDI packet. A 20-byte minimum
        // ATT MTU accommodates a short message + 2-byte BLE-MIDI header, so a
        // single write is the common path; larger sysex is pre-chunked by the
        // caller. Outbound timestamp 0 is acceptable (the wire is not re-timed).
        std::vector<uint8_t> packet = encode_ble_midi_packet(
            midi_bytes.data(), midi_bytes.size(), /*timestamp_ms=*/0);
        if (packet.empty()) return;

        try {
            wss::DataWriter writer;
            writer.WriteBytes(::winrt::array_view<const uint8_t>(
                packet.data(), packet.data() + packet.size()));
            // WriteWithoutResponse matches the BLE-MIDI spec's low-latency,
            // un-acked write; fall back is the OS default if the characteristic
            // lacks the property.
            characteristic.WriteValueAsync(
                writer.DetachBuffer(),
                gatt::GattWriteOption::WriteWithoutResponse).get();
        } catch (const ::winrt::hresult_error& e) {
            runtime::log_error("WinRT BLE MIDI: GATT write failed: {}",
                               ::winrt::to_string(e.message()));
        }
    }

    // ── State callback helpers ───────────────────────────────────────────────

    void fire_state(const std::string& id, BleMidiConnectionState state,
                    BleMidiError err) {
        BleMidiStateCallback cb;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cb = state_cb_;
        }
        if (cb) cb(id, state, err);
    }

    std::string peripheral_name(const std::string& id) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = peripherals_.find(id);
        if (it == peripherals_.end() || it->second.name.empty()) return id;
        return it->second.name;
    }

    // Stable string keys for the WinRT object → peripheral-id reverse maps. The
    // characteristic's AttributeHandle is unique within a connected device, and
    // the device's BluetoothAddress is the same 48-bit handle the id derives
    // from — using them avoids comparing projection object identity.
    static std::string char_key(const gatt::GattCharacteristic& c) {
        if (!c) return {};
        return std::to_string(c.Service().Device().BluetoothAddress()) + ":" +
               std::to_string(c.AttributeHandle());
    }
    static std::string device_key(const wdb::BluetoothLEDevice& d) {
        if (!d) return {};
        return std::to_string(d.BluetoothAddress());
    }

    // One live GATT connection. The BluetoothLEDevice / service / characteristic
    // MUST be held for the connection's lifetime — releasing the device drops
    // the link. The event tokens are unsubscribed on disconnect.
    struct Connection {
        wdb::BluetoothLEDevice device{nullptr};
        gatt::GattDeviceService service{nullptr};
        gatt::GattCharacteristic characteristic{nullptr};
        ::winrt::event_token value_token{};
        ::winrt::event_token status_token{};
        std::unique_ptr<BleMidiPacketDecoder> decoder;
        std::string input_port;
        std::string output_port;
    };

    mutable std::mutex mu_;
    std::map<std::string, BleMidiPeripheral> peripherals_;
    std::map<std::string, std::unique_ptr<Connection>> connections_;
    std::map<std::string, std::string> char_to_id_;    // char key → peripheral id
    std::map<std::string, std::string> device_to_id_;  // device key → peripheral id
    BleMidiScanCallback scan_cb_;
    BleMidiStateCallback state_cb_;
    std::atomic<bool> scanning_{false};

    wda::BluetoothLEAdvertisementWatcher watcher_{nullptr};
    ::winrt::event_token received_token_{};
};

}  // namespace

std::unique_ptr<BleMidiCentral> create_ble_midi_central() {
    return std::make_unique<WinBleMidiCentral>();
}

}  // namespace pulp::midi

#endif  // _WIN32
