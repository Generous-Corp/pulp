// BLE MIDI backend — Windows using WinRT Bluetooth-LE advertisement scan.
//
// Implements pulp::midi::BleMidiCentral on Windows. This slice is the
// *scan probe* half: a BluetoothLEAdvertisementWatcher filtered to the
// BLE-MIDI 1.0 service UUID (03B80E5A-EDE8-4B33-A751-6CE34EC4C700)
// surfaces advertising peripherals through the scan callback. GATT
// connect + characteristic notification (the inbound MIDI byte stream)
// lands in a follow-up slice — see the "PR4: connect/notify" markers.
//
// Why the base Windows SDK is enough here (unlike the WinRT MIDI 2.0
// backend in winrt_midi_device.cpp): BLE GATT lives in
// Windows.Devices.Bluetooth / Windows.Devices.Bluetooth.Advertisement,
// which ship in the BASE Windows SDK cppwinrt projection. There is no
// out-of-band NuGet / vcpkg dependency — we link the OS umbrella import
// library (WindowsApp) and include the stock winrt headers.
//
// Threading: the watcher's Received event fires on a WinRT thread-pool
// thread. The BleMidiScanCallback contract already permits invocation
// from a backend thread, so we forward directly under a short mutex hold
// that protects the dedup map + the stored callback (matching the
// CoreBluetooth backend's discipline). Apartment init is lazy + MTA, the
// same pattern winrt_midi_device.cpp uses.

#if defined(_WIN32)

#include <pulp/midi/ble_midi.hpp>
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

namespace pulp::midi {
namespace {

namespace wf  = ::winrt::Windows::Foundation;
namespace wda = ::winrt::Windows::Devices::Bluetooth::Advertisement;

// Lazy, idempotent MTA apartment init. WinRT reference-counts init/uninit
// per thread; calling init_apartment more than once on the same thread is
// benign. We init lazily so merely linking the backend does not force
// apartment creation on hosts that never scan for BLE peripherals.
void ensure_winrt_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        try {
            ::winrt::init_apartment(::winrt::apartment_type::multi_threaded);
        } catch (const ::winrt::hresult_error&) {
            // Already initialised on this thread with a different model, or
            // COM already up — both are fine for our consumer-only usage.
        }
    });
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

class WinBleMidiCentral final : public BleMidiCentral {
public:
    WinBleMidiCentral() { ensure_winrt_init(); }

    ~WinBleMidiCentral() override { stop_scan(); }

    // True if the WinRT BLE advertisement API is usable on this machine.
    // Constructing a BluetoothLEAdvertisementWatcher does not require an
    // adapter to be present, but a missing Bluetooth stack throws an
    // hresult_error here — treat any throw as "unavailable" so callers
    // degrade gracefully (matching the stub central's contract).
    bool is_available() const override {
        try {
            wda::BluetoothLEAdvertisementWatcher probe{};
            (void)probe;
            return true;
        } catch (const ::winrt::hresult_error&) {
            return false;
        }
    }

    bool start_scan(BleMidiScanCallback cb) override {
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

    // PR4: connect/notify. The scan probe does not yet open a GATT link.
    // Report Failed/Unsupported so the contract stays honest and the host
    // does not wait forever for a Connected that will not arrive.
    bool connect(const std::string& id) override {
        BleMidiStateCallback cb_copy;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cb_copy = state_cb_;
        }
        if (cb_copy) {
            cb_copy(id, BleMidiConnectionState::Failed,
                    BleMidiError::Unsupported);
        }
        return false;
    }

    // PR4: connect/notify.
    void disconnect(const std::string&) override {}

    void set_state_callback(BleMidiStateCallback cb) override {
        std::lock_guard<std::mutex> lock(mu_);
        state_cb_ = std::move(cb);
    }

    // PR4: connect/notify. No GATT link yet, so no MIDI port mapping.
    std::string midi_input_port_for(const std::string&) const override {
        return {};
    }
    std::string midi_output_port_for(const std::string&) const override {
        return {};
    }

private:
    // Fired on a WinRT thread-pool thread for each filtered advertisement.
    void on_received(const wda::BluetoothLEAdvertisementWatcher&,
                     const wda::BluetoothLEAdvertisementReceivedEventArgs& args) {
        try {
            BleMidiPeripheral snapshot;
            snapshot.id = address_to_id(args.BluetoothAddress());
            snapshot.name = ::winrt::to_string(args.Advertisement().LocalName());
            snapshot.rssi = args.RawSignalStrengthInDBm();
            snapshot.last_seen = std::chrono::steady_clock::now();
            // The OS pairing/bond state is a GATT-side query; the scan probe
            // does not resolve it, so report false until PR4 wires connect.
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

    mutable std::mutex mu_;
    std::map<std::string, BleMidiPeripheral> peripherals_;
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
