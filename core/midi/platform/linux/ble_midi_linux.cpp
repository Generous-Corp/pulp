// BLE MIDI backend — Linux using BlueZ over D-Bus (system bus).
//
// Implements pulp::midi::BleMidiCentral on Linux. BlueZ (org.bluez) is a
// system service reached over the already-dlopened libdbus client in
// core/platform (pulp::platform::DBus on connect_system()). No new library is
// linked: BlueZ is a running daemon, libdbus is loaded at runtime.
//
// Flow:
//   • is_available(): connect_system() succeeds AND an org.bluez.Adapter1
//     exists (discovered via GetManagedObjects on /).
//   • start_scan(): Adapter1.SetDiscoveryFilter (UUIDs=[kService],
//     Transport="le") + Adapter1.StartDiscovery, then subscribe to
//     ObjectManager.InterfacesAdded (new Device1 objects) and
//     Properties.PropertiesChanged (RSSI/Name updates). Each Device1 that
//     advertises the BLE-MIDI service becomes a BleMidiPeripheral surfaced on
//     the scan callback. The initial GetManagedObjects snapshot seeds already-
//     known devices.
//   • connect(): Device1.Connect on the device path, then locate the
//     GattCharacteristic1 whose UUID is kCharacteristic under the device,
//     GattCharacteristic1.StartNotify, and subscribe to PropertiesChanged on
//     THAT characteristic path. The notify bytes arrive as the "Value" property
//     (a 'ay' byte array) and are fed to a per-connection BleMidiPacketDecoder
//     whose decoded messages are published to the BleMidiPortRegistry, which the
//     ALSA MidiSystem merges into its port list.
//   • Output: encode_ble_midi_packet → GattCharacteristic1.WriteValue.
//
// Peripheral id == the BlueZ device object path (e.g.
// "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"). The path is the stable handle BlueZ
// uses for every subsequent call (Connect, characteristic lookup), so it is the
// natural id to hand back to connect(); the device Address property is exposed
// only as part of the human-readable name fallback.
//
// Threading: D-Bus signals are only delivered while dispatch() runs, so a
// dedicated worker thread pumps DBus::dispatch() while scanning or connected.
// Scan / state / decoder callbacks may therefore fire on the worker thread, per
// the BleMidiCentral contract; shared state is guarded by mu_.

#include <pulp/midi/ble_midi.hpp>
#include <pulp/midi/ble_midi_registry.hpp>
#include <pulp/platform/dbus.hpp>
#include <pulp/runtime/log.hpp>

#ifndef __linux__
#error "ble_midi_linux.cpp is Linux-only"
#endif

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulp::midi {
namespace {

constexpr const char* kBluezService = "org.bluez";
constexpr const char* kAdapterIface = "org.bluez.Adapter1";
constexpr const char* kDeviceIface = "org.bluez.Device1";
constexpr const char* kGattCharIface = "org.bluez.GattCharacteristic1";
constexpr const char* kObjectManagerIface = "org.freedesktop.DBus.ObjectManager";
constexpr const char* kPropertiesIface = "org.freedesktop.DBus.Properties";

// Case-insensitive UUID compare — BlueZ reports UUIDs lowercased, the
// well-known constants are uppercase.
bool uuid_equals(const std::string& a, const char* b) {
    std::string lb = b;
    if (a.size() != lb.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = lb[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

std::string input_port_id(const std::string& device_path) {
    return "ble-midi-in:" + device_path;
}
std::string output_port_id(const std::string& device_path) {
    return "ble-midi-out:" + device_path;
}

// Translate a BlueZ D-Bus error name into the neutral BleMidiError enum so
// cross-platform callers never see org.bluez.* strings. Reached independently
// from the BlueZ D-Bus error-name documentation.
BleMidiError bluez_error_to_ble(const std::string& dbus_error_name) {
    if (dbus_error_name.empty()) return BleMidiError::None;
    if (dbus_error_name.find("NotReady") != std::string::npos ||
        dbus_error_name.find("Blocked") != std::string::npos) {
        return BleMidiError::BluetoothOff;
    }
    if (dbus_error_name.find("NotAuthorized") != std::string::npos ||
        dbus_error_name.find("AccessDenied") != std::string::npos ||
        dbus_error_name.find("AuthenticationFailed") != std::string::npos) {
        return BleMidiError::PermissionDenied;
    }
    if (dbus_error_name.find("DoesNotExist") != std::string::npos ||
        dbus_error_name.find("UnknownObject") != std::string::npos) {
        return BleMidiError::PeripheralNotFound;
    }
    if (dbus_error_name.find("NotSupported") != std::string::npos) {
        return BleMidiError::ServiceNotFound;
    }
    if (dbus_error_name.find("Timeout") != std::string::npos ||
        dbus_error_name.find("InProgress") != std::string::npos) {
        return BleMidiError::Timeout;
    }
    return BleMidiError::ConnectFailed;
}

// ── Property readers over the D-Bus marshalling facade ───────────────────────
//
// Read one variant value out of a {sv} dict-entry reader positioned at the
// variant. These helpers leave the cursor where the facade put it.

bool read_variant_string(platform::DBus::Reader& entry, std::string& out) {
    platform::DBus::Reader var;
    if (!entry.recurse(var)) return false;       // → variant body
    return var.read_string(out);
}

bool read_variant_int32(platform::DBus::Reader& entry, int& out) {
    platform::DBus::Reader var;
    if (!entry.recurse(var)) return false;
    return var.read_int32(out);
}

// Read a variant holding 'ay' (byte array) into `out`.
bool read_variant_byte_array(platform::DBus::Reader& entry,
                             std::vector<uint8_t>& out) {
    platform::DBus::Reader var;
    if (!entry.recurse(var)) return false;       // → variant body ('ay')
    if (var.arg_type() != 'a') return false;
    platform::DBus::Reader arr;
    if (!var.recurse(arr)) return false;         // → array of 'y'
    out.clear();
    while (arr.arg_type() == 'y') {
        unsigned char b = 0;
        if (!arr.read_byte(b)) break;
        out.push_back(b);
    }
    return true;
}

// Read a variant holding 'as' (string array), e.g. Device1.UUIDs, and report
// whether it contains the BLE-MIDI service UUID.
bool variant_string_array_has_uuid(platform::DBus::Reader& entry,
                                   const char* wanted_uuid) {
    platform::DBus::Reader var;
    if (!entry.recurse(var)) return false;       // → variant body ('as')
    if (var.arg_type() != 'a') return false;
    platform::DBus::Reader arr;
    if (!var.recurse(arr)) return false;         // → array of 's'
    bool found = false;
    std::string s;
    while (arr.arg_type() == 's') {
        if (!arr.read_string(s)) break;
        if (uuid_equals(s, wanted_uuid)) found = true;
    }
    return found;
}

// One Device1 property dict (a{sv}) parsed into the fields we care about.
struct DeviceProps {
    bool has_midi_service = false;
    bool name_set = false;
    std::string name;
    bool rssi_set = false;
    int rssi = 0;
};

// Walk an a{sv} dict reader positioned at the array, extracting Device1 props.
// `dict` must be a Reader on the 'a{sv}' array.
void parse_device_props(platform::DBus::Reader& dict, DeviceProps& props) {
    while (dict.arg_type() == 'e') {             // dict-entry '{sv}'
        platform::DBus::Reader entry;
        if (!dict.recurse(entry)) break;
        std::string key;
        if (!entry.read_string(key)) { dict.next(); continue; }
        if (key == "UUIDs") {
            if (variant_string_array_has_uuid(
                    entry, BleMidiUuids::kService)) {
                props.has_midi_service = true;
            }
        } else if (key == "Alias" || key == "Name") {
            std::string v;
            if (read_variant_string(entry, v)) {
                // Prefer Alias over Name when both appear in one dict.
                if (key == "Alias" || !props.name_set) {
                    props.name = v;
                    props.name_set = true;
                }
            }
        } else if (key == "RSSI") {
            int v = 0;
            if (read_variant_int32(entry, v)) {
                props.rssi = v;
                props.rssi_set = true;
            }
        }
        dict.next();
    }
}

class BluezBleMidiCentral final : public BleMidiCentral {
public:
    BluezBleMidiCentral() = default;

    ~BluezBleMidiCentral() override {
        stop_worker();
    }

    bool is_available() const override {
        // A short-lived probe connection: connect to the system bus and look
        // for at least one org.bluez.Adapter1. Honest-false when BlueZ or an
        // adapter is absent.
        platform::DBus probe;
        if (!probe.connect_system()) return false;
        bool found = false;
        probe.get_managed_objects(
            kBluezService, "/",
            [&](const std::string&, const std::string& iface) {
                if (iface == kAdapterIface) found = true;
            });
        return found;
    }

    bool start_scan(BleMidiScanCallback cb) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            scan_cb_ = std::move(cb);
            if (scanning_.load()) return true;  // already scanning — no-op.
        }
        if (!ensure_connected()) return false;

        const std::string adapter = discover_adapter_path();
        if (adapter.empty()) {
            runtime::log_error("BLE MIDI (BlueZ): no adapter found");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            adapter_path_ = adapter;
        }

        // Discovery filter: only LE devices advertising the BLE-MIDI service.
        // SetDiscoveryFilter(a{sv}) — keys "UUIDs" ('as') and "Transport" ('s').
        bool filter_ok = dbus_->call_method(
            kBluezService, adapter, kAdapterIface, "SetDiscoveryFilter",
            [](platform::DBus::Writer& w) {
                auto arr = w.open_array("{sv}");
                platform::DBus::Writer aw = w.sub(arr);
                {
                    auto e = aw.open_dict_entry();
                    platform::DBus::Writer ew = aw.sub(e);
                    ew.append_string("UUIDs");
                    auto v = ew.open_variant("as");
                    platform::DBus::Writer vw = ew.sub(v);
                    auto sa = vw.open_array("s");
                    platform::DBus::Writer saw = vw.sub(sa);
                    saw.append_string(BleMidiUuids::kService);
                    vw.close_container(sa);
                    ew.close_container(v);
                    aw.close_container(e);
                }
                {
                    auto e = aw.open_dict_entry();
                    platform::DBus::Writer ew = aw.sub(e);
                    ew.append_string("Transport");
                    auto v = ew.open_variant("s");
                    platform::DBus::Writer vw = ew.sub(v);
                    vw.append_string("le");
                    ew.close_container(v);
                    aw.close_container(e);
                }
                w.close_container(arr);
            },
            nullptr);
        if (!filter_ok) {
            // Non-fatal: some adapters reject filters; fall through to an
            // unfiltered discovery and filter client-side by UUID.
            runtime::log_info("BLE MIDI (BlueZ): SetDiscoveryFilter rejected; "
                              "scanning unfiltered");
        }

        // Subscribe BEFORE StartDiscovery so we never miss an early add.
        subscribe_scan_signals();

        bool start_ok = dbus_->call_method(
            kBluezService, adapter, kAdapterIface, "StartDiscovery",
            [](platform::DBus::Writer&) {}, nullptr);
        if (!start_ok) {
            runtime::log_error("BLE MIDI (BlueZ): StartDiscovery failed");
            unsubscribe_scan_signals();
            return false;
        }

        scanning_.store(true);
        ensure_worker();

        // Seed from devices BlueZ already knows about.
        seed_known_devices();
        return true;
    }

    void stop_scan() override {
        if (!scanning_.exchange(false)) return;
        std::string adapter;
        {
            std::lock_guard<std::mutex> lock(mu_);
            adapter = adapter_path_;
        }
        if (dbus_ && !adapter.empty()) {
            dbus_->call_method(kBluezService, adapter, kAdapterIface,
                               "StopDiscovery",
                               [](platform::DBus::Writer&) {}, nullptr);
        }
        unsubscribe_scan_signals();
        maybe_stop_worker();
    }

    bool is_scanning() const override { return scanning_.load(); }

    std::vector<BleMidiPeripheral> known_peripherals() const override {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<BleMidiPeripheral> out;
        out.reserve(peripherals_.size());
        for (const auto& [id, entry] : peripherals_) out.push_back(entry.snapshot);
        return out;
    }

    bool connect(const std::string& id) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (peripherals_.find(id) == peripherals_.end()) {
                fire_state_locked(id, BleMidiConnectionState::Failed,
                                  BleMidiError::PeripheralNotFound);
                return false;
            }
        }
        if (!ensure_connected()) {
            fire_state(id, BleMidiConnectionState::Failed,
                       BleMidiError::BluetoothOff);
            return false;
        }
        fire_state(id, BleMidiConnectionState::Connecting, BleMidiError::None);

        // Device1.Connect — blocking; BlueZ resolves GATT on success.
        std::string err_name;
        bool ok = call_capture_error(
            kBluezService, id, kDeviceIface, "Connect",
            [](platform::DBus::Writer&) {}, err_name);
        if (!ok) {
            fire_state(id, BleMidiConnectionState::Failed,
                       bluez_error_to_ble(err_name));
            return false;
        }

        // Locate the BLE-MIDI characteristic under the device.
        const std::string char_path = find_midi_characteristic(id);
        if (char_path.empty()) {
            fire_state(id, BleMidiConnectionState::Failed,
                       BleMidiError::ServiceNotFound);
            return false;
        }

        // StartNotify so the characteristic Value arrives via PropertiesChanged.
        bool notify_ok = call_capture_error(
            kBluezService, char_path, kGattCharIface, "StartNotify",
            [](platform::DBus::Writer&) {}, err_name);
        if (!notify_ok) {
            fire_state(id, BleMidiConnectionState::Failed,
                       bluez_error_to_ble(err_name));
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            auto& entry = peripherals_[id];
            entry.char_path = char_path;
            entry.connected = true;
            entry.decoder = std::make_unique<BleMidiPacketDecoder>();
            entry.decoder->set_message_callback(
                [this, id](const std::vector<uint8_t>& bytes, uint32_t ts_ms) {
                    BleMidiPortRegistry::instance().deliver_message(
                        input_port_id(id), bytes,
                        static_cast<double>(ts_ms) / 1000.0);
                });
            char_to_device_[char_path] = id;
            entry.midi_input_port_id = input_port_id(id);
            entry.midi_output_port_id = output_port_id(id);
        }

        // Publish into the registry so the ALSA MidiSystem enumerates the port.
        const std::string name = peripheral_name(id);
        auto& reg = BleMidiPortRegistry::instance();
        reg.register_input(input_port_id(id), name);
        reg.register_output(
            output_port_id(id), name,
            [this, char_path](const std::vector<uint8_t>& bytes) {
                gatt_write(char_path, bytes);
            });

        subscribe_char_notify(char_path);
        ensure_worker();
        fire_state(id, BleMidiConnectionState::Connected, BleMidiError::None);
        return true;
    }

    void disconnect(const std::string& id) override {
        std::string char_path;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = peripherals_.find(id);
            if (it == peripherals_.end()) return;
            char_path = it->second.char_path;
            it->second.connected = false;
            it->second.decoder.reset();
            it->second.char_path.clear();
            it->second.midi_input_port_id.clear();
            it->second.midi_output_port_id.clear();
        }

        auto& reg = BleMidiPortRegistry::instance();
        reg.unregister_input(input_port_id(id));
        reg.unregister_output(output_port_id(id));

        if (dbus_ && !char_path.empty()) {
            dbus_->call_method(kBluezService, char_path, kGattCharIface,
                               "StopNotify", [](platform::DBus::Writer&) {},
                               nullptr);
        }
        if (dbus_) {
            dbus_->call_method(kBluezService, id, kDeviceIface, "Disconnect",
                               [](platform::DBus::Writer&) {}, nullptr);
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!char_path.empty()) char_to_device_.erase(char_path);
        }
        fire_state(id, BleMidiConnectionState::Disconnected, BleMidiError::None);
        maybe_stop_worker();
    }

    void set_state_callback(BleMidiStateCallback cb) override {
        std::lock_guard<std::mutex> lock(mu_);
        state_cb_ = std::move(cb);
    }

    std::string midi_input_port_for(const std::string& id) const override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = peripherals_.find(id);
        if (it == peripherals_.end()) return {};
        return it->second.midi_input_port_id;
    }
    std::string midi_output_port_for(const std::string& id) const override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = peripherals_.find(id);
        if (it == peripherals_.end()) return {};
        return it->second.midi_output_port_id;
    }

private:
    struct Entry {
        BleMidiPeripheral snapshot{};
        bool connected = false;
        std::string char_path;
        std::string midi_input_port_id;
        std::string midi_output_port_id;
        std::unique_ptr<BleMidiPacketDecoder> decoder;
    };

    // ── Connection / worker lifecycle ────────────────────────────────────────

    bool ensure_connected() {
        std::lock_guard<std::mutex> lock(dbus_mu_);
        if (!dbus_) dbus_ = std::make_unique<platform::DBus>();
        if (dbus_->connected()) return true;
        return dbus_->connect_system();
    }

    void ensure_worker() {
        if (worker_running_.exchange(true)) return;
        worker_ = std::thread([this] { worker_loop(); });
    }

    void worker_loop() {
        while (worker_running_.load(std::memory_order_relaxed)) {
            {
                std::lock_guard<std::mutex> lock(dbus_mu_);
                if (dbus_) dbus_->dispatch(100 /* ms */);
            }
            // dispatch() already blocks up to its timeout on I/O, so no extra
            // sleep is needed.
        }
    }

    // Stop the worker only when nothing needs the D-Bus pump anymore.
    void maybe_stop_worker() {
        if (scanning_.load()) return;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& [id, e] : peripherals_)
                if (e.connected) return;
        }
        stop_worker();
    }

    void stop_worker() {
        if (!worker_running_.exchange(false)) return;
        if (worker_.joinable()) worker_.join();
    }

    // ── BlueZ discovery ──────────────────────────────────────────────────────

    std::string discover_adapter_path() {
        std::string adapter;
        std::lock_guard<std::mutex> lock(dbus_mu_);
        if (!dbus_) return adapter;
        dbus_->get_managed_objects(
            kBluezService, "/",
            [&](const std::string& path, const std::string& iface) {
                if (adapter.empty() && iface == kAdapterIface) adapter = path;
            });
        return adapter;
    }

    // Seed already-known Device1 objects (paired / previously discovered) so the
    // scan callback fires immediately for them too.
    void seed_known_devices() {
        std::vector<std::string> device_paths;
        {
            std::lock_guard<std::mutex> lock(dbus_mu_);
            if (!dbus_) return;
            dbus_->get_managed_objects(
                kBluezService, "/",
                [&](const std::string& path, const std::string& iface) {
                    if (iface == kDeviceIface) device_paths.push_back(path);
                });
        }
        for (const auto& path : device_paths) refresh_device(path);
    }

    // Query a device's full Device1 property dict via Properties.GetAll and, if
    // it advertises the BLE-MIDI service, surface it on the scan callback.
    void refresh_device(const std::string& device_path) {
        DeviceProps props;
        bool got = false;
        {
            std::lock_guard<std::mutex> lock(dbus_mu_);
            if (!dbus_) return;
            got = dbus_->call_method(
                kBluezService, device_path, kPropertiesIface, "GetAll",
                [](platform::DBus::Writer& w) { w.append_string(kDeviceIface); },
                [&](platform::DBus::Reader& reply) {
                    if (reply.arg_type() != 'a') return;  // a{sv}
                    platform::DBus::Reader dict;
                    if (!reply.recurse(dict)) return;
                    parse_device_props(dict, props);
                });
        }
        if (!got || !props.has_midi_service) return;
        publish_peripheral(device_path, props);
    }

    void publish_peripheral(const std::string& device_path,
                            const DeviceProps& props) {
        BleMidiScanCallback cb_copy;
        BleMidiPeripheral snapshot;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto& entry = peripherals_[device_path];
            entry.snapshot.id = device_path;
            if (props.name_set) entry.snapshot.name = props.name;
            else if (entry.snapshot.name.empty())
                entry.snapshot.name = device_path;
            if (props.rssi_set) entry.snapshot.rssi = props.rssi;
            entry.snapshot.last_seen = std::chrono::steady_clock::now();
            snapshot = entry.snapshot;
            cb_copy = scan_cb_;
        }
        if (cb_copy) cb_copy(snapshot);
    }

    std::string peripheral_name(const std::string& id) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = peripherals_.find(id);
        if (it == peripherals_.end() || it->second.snapshot.name.empty())
            return id;
        return it->second.snapshot.name;
    }

    // Find the GattCharacteristic1 object whose UUID is the BLE-MIDI
    // characteristic and that lives under `device_path`. Returns the empty
    // string if none is exposed (service-not-found).
    std::string find_midi_characteristic(const std::string& device_path) {
        std::vector<std::string> candidate_paths;
        {
            std::lock_guard<std::mutex> lock(dbus_mu_);
            if (!dbus_) return {};
            dbus_->get_managed_objects(
                kBluezService, "/",
                [&](const std::string& path, const std::string& iface) {
                    if (iface == kGattCharIface &&
                        path.rfind(device_path + "/", 0) == 0) {
                        candidate_paths.push_back(path);
                    }
                });
        }
        for (const auto& path : candidate_paths) {
            std::string uuid;
            {
                std::lock_guard<std::mutex> lock(dbus_mu_);
                if (!dbus_) return {};
                dbus_->call_method(
                    kBluezService, path, kPropertiesIface, "Get",
                    [](platform::DBus::Writer& w) {
                        w.append_string(kGattCharIface);
                        w.append_string("UUID");
                    },
                    [&](platform::DBus::Reader& reply) {
                        // Reply is a single variant 's'.
                        platform::DBus::Reader var;
                        if (reply.arg_type() == 'v' && reply.recurse(var))
                            var.read_string(uuid);
                    });
            }
            if (uuid_equals(uuid, BleMidiUuids::kCharacteristic)) return path;
        }
        return {};
    }

    // ── Output ──────────────────────────────────────────────────────────────

    void gatt_write(const std::string& char_path,
                    const std::vector<uint8_t>& midi_bytes) {
        if (midi_bytes.empty()) return;
        // Encode to a BLE-MIDI packet (single timestamped message). The decoder
        // and encoder share the 13-bit ms timestamp clock; a 0 timestamp is
        // acceptable for outbound (BlueZ does not re-time on write).
        std::vector<uint8_t> packet = encode_ble_midi_packet(
            midi_bytes.data(), midi_bytes.size(), /*timestamp_ms=*/0);
        if (packet.empty()) return;

        // Chunk to a conservative 20-byte default ATT MTU minus the GATT write
        // header is handled by BlueZ; we cap each WriteValue at <= MTU-1 worth
        // of MIDI but the spec's minimum 20-byte MTU already accommodates a
        // single short message + 2-byte BLE-MIDI header, so a single write is
        // the common path. Larger sysex is pre-chunked by the caller.
        std::lock_guard<std::mutex> lock(dbus_mu_);
        if (!dbus_) return;
        dbus_->call_method(
            kBluezService, char_path, kGattCharIface, "WriteValue",
            [&](platform::DBus::Writer& w) {
                // WriteValue(ay value, a{sv} options) — empty options.
                auto arr = w.open_array("y");
                platform::DBus::Writer aw = w.sub(arr);
                for (uint8_t b : packet) aw.append_byte(b);
                w.close_container(arr);
                auto opts = w.open_array("{sv}");
                w.close_container(opts);
            },
            nullptr);
    }

    // ── Signal subscription ──────────────────────────────────────────────────

    void subscribe_scan_signals() {
        std::lock_guard<std::mutex> lock(dbus_mu_);
        if (!dbus_) return;
        if (interfaces_added_token_ == 0) {
            interfaces_added_token_ = dbus_->add_signal_handler(
                kObjectManagerIface, "InterfacesAdded",
                [this](const std::string& /*path*/, const std::string&,
                       const std::string&, platform::DBus::Reader& args) {
                    on_interfaces_added(args);
                });
        }
        if (props_changed_token_ == 0) {
            props_changed_token_ = dbus_->add_signal_handler(
                kPropertiesIface, "PropertiesChanged",
                [this](const std::string& path, const std::string&,
                       const std::string&, platform::DBus::Reader& args) {
                    on_properties_changed(path, args);
                });
        }
    }

    void unsubscribe_scan_signals() {
        std::lock_guard<std::mutex> lock(dbus_mu_);
        if (!dbus_) return;
        if (interfaces_added_token_ != 0) {
            dbus_->remove_signal_handler(interfaces_added_token_);
            interfaces_added_token_ = 0;
        }
        // Leave PropertiesChanged subscribed if a connection still needs notify
        // delivery; only drop it when nothing is connected.
        bool any_connected = false;
        {
            std::lock_guard<std::mutex> lock2(mu_);
            for (const auto& [id, e] : peripherals_)
                if (e.connected) any_connected = true;
        }
        if (!any_connected && props_changed_token_ != 0) {
            dbus_->remove_signal_handler(props_changed_token_);
            props_changed_token_ = 0;
        }
    }

    // PropertiesChanged on a characteristic carries the GATT notify bytes; we
    // share ONE handler for both Device1 (RSSI/Name) and GattCharacteristic1
    // (Value) and dispatch on the interface name in the signal body.
    void subscribe_char_notify(const std::string& /*char_path*/) {
        // Already covered by the PropertiesChanged subscription from
        // subscribe_scan_signals(); make sure it is installed even if scanning
        // was never started (connect() without a prior scan).
        std::lock_guard<std::mutex> lock(dbus_mu_);
        if (!dbus_) return;
        if (props_changed_token_ == 0) {
            props_changed_token_ = dbus_->add_signal_handler(
                kPropertiesIface, "PropertiesChanged",
                [this](const std::string& path, const std::string&,
                       const std::string&, platform::DBus::Reader& args) {
                    on_properties_changed(path, args);
                });
        }
    }

    // InterfacesAdded(o object_path, a{sa{sv}} interfaces_and_properties).
    void on_interfaces_added(platform::DBus::Reader& args) {
        std::string obj_path;
        if (!args.read_string(obj_path)) return;     // 'o'
        if (args.arg_type() != 'a') return;          // a{sa{sv}}
        platform::DBus::Reader ifaces;
        if (!args.recurse(ifaces)) return;
        DeviceProps props;
        bool is_device = false;
        while (ifaces.arg_type() == 'e') {           // {s, a{sv}}
            platform::DBus::Reader iface_entry;
            if (!ifaces.recurse(iface_entry)) break;
            std::string iface_name;
            if (!iface_entry.read_string(iface_name)) { ifaces.next(); continue; }
            if (iface_name == kDeviceIface) {
                is_device = true;
                if (iface_entry.arg_type() == 'a') {
                    platform::DBus::Reader dict;
                    if (iface_entry.recurse(dict)) parse_device_props(dict, props);
                }
            }
            ifaces.next();
        }
        if (is_device && props.has_midi_service) publish_peripheral(obj_path, props);
        else if (is_device) {
            // A new Device1 without UUIDs yet — query it; UUIDs often arrive in
            // a later PropertiesChanged but a GetAll resolves the common case.
            refresh_device(obj_path);
        }
    }

    // PropertiesChanged(s interface, a{sv} changed, as invalidated).
    void on_properties_changed(const std::string& path,
                               platform::DBus::Reader& args) {
        std::string iface;
        if (!args.read_string(iface)) return;        // 's'
        if (args.arg_type() != 'a') return;          // a{sv}
        platform::DBus::Reader dict;
        if (!args.recurse(dict)) return;

        if (iface == kGattCharIface) {
            handle_char_value_changed(path, dict);
            return;
        }
        if (iface == kDeviceIface) {
            DeviceProps props;
            parse_device_props(dict, props);
            // Only surface devices we already know advertise BLE-MIDI, or that
            // now reveal the service UUID via this update.
            bool known = false;
            {
                std::lock_guard<std::mutex> lock(mu_);
                known = peripherals_.find(path) != peripherals_.end();
            }
            if (props.has_midi_service || known) publish_peripheral(path, props);
        }
    }

    // Pull the "Value" 'ay' out of a GattCharacteristic1 PropertiesChanged and
    // feed it to the matching device's decoder.
    void handle_char_value_changed(const std::string& char_path,
                                   platform::DBus::Reader& dict) {
        std::vector<uint8_t> value;
        bool have_value = false;
        while (dict.arg_type() == 'e') {
            platform::DBus::Reader entry;
            if (!dict.recurse(entry)) break;
            std::string key;
            if (!entry.read_string(key)) { dict.next(); continue; }
            if (key == "Value") {
                if (read_variant_byte_array(entry, value)) have_value = true;
            }
            dict.next();
        }
        if (!have_value || value.empty()) return;

        std::string device_id;
        BleMidiPacketDecoder* decoder = nullptr;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto cit = char_to_device_.find(char_path);
            if (cit == char_to_device_.end()) return;
            device_id = cit->second;
            auto pit = peripherals_.find(device_id);
            if (pit == peripherals_.end() || !pit->second.decoder) return;
            decoder = pit->second.decoder.get();
            // decode() under the lock is safe: it only forwards into the
            // registry, which copies callbacks out under its own lock.
            decoder->decode(value.data(), value.size());
        }
    }

    // ── State callback helpers ────────────────────────────────────────────────

    void fire_state(const std::string& id, BleMidiConnectionState state,
                    BleMidiError err) {
        BleMidiStateCallback cb;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cb = state_cb_;
        }
        if (cb) cb(id, state, err);
    }
    void fire_state_locked(const std::string& id, BleMidiConnectionState state,
                           BleMidiError err) {
        // Caller already holds mu_.
        if (state_cb_) state_cb_(id, state, err);
    }

    // call_method wrapper that captures the D-Bus error NAME on failure. The
    // facade's call_method returns false on a D-Bus error reply but does not
    // surface the error name, so we run a probe call and, if it fails, classify
    // conservatively. (BlueZ error-name → BleMidiError mapping is tested
    // separately via bluez_error_to_ble.)
    bool call_capture_error(const std::string& dest, const std::string& path,
                            const std::string& iface, const std::string& member,
                            const std::function<void(platform::DBus::Writer&)>& args,
                            std::string& err_name_out) {
        err_name_out.clear();
        std::lock_guard<std::mutex> lock(dbus_mu_);
        if (!dbus_) return false;
        bool ok = dbus_->call_method(dest, path, iface, member, args, nullptr);
        if (!ok) err_name_out = "org.bluez.Error.Failed";
        return ok;
    }

    mutable std::mutex mu_;             // guards peripherals_ / callbacks
    std::mutex dbus_mu_;               // guards dbus_ + token state
    std::unique_ptr<platform::DBus> dbus_;
    std::map<std::string, Entry> peripherals_;
    std::map<std::string, std::string> char_to_device_;  // char path → device id
    std::string adapter_path_;
    BleMidiScanCallback scan_cb_;
    BleMidiStateCallback state_cb_;
    unsigned interfaces_added_token_ = 0;
    unsigned props_changed_token_ = 0;

    std::atomic<bool> scanning_{false};
    std::atomic<bool> worker_running_{false};
    std::thread worker_;
};

}  // namespace

std::unique_ptr<BleMidiCentral> create_ble_midi_central() {
    return std::make_unique<BluezBleMidiCentral>();
}

}  // namespace pulp::midi
