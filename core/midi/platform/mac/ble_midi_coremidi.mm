// BLE MIDI backend — macOS / iOS using CoreBluetooth + CoreMIDI.
//
// Implements pulp::midi::BleMidiCentral on Apple platforms. The
// CBCentralManager scans for advertisements containing the BLE-MIDI
// service UUID (03B80E5A-EDE8-4B33-A751-6CE34EC4C700). When the
// host calls connect(id), CoreBluetooth opens the GATT link; on
// success, the GATT characteristic (7772E5DB-...-...-6BF3)
// notifications are decoded via BleMidiPacketDecoder and replayed
// onto CoreMIDI virtual source/dest pairs so the rest of Pulp sees
// the BLE link as a normal MIDI port (matching the system CoreMIDI
// behaviour for OS-paired BLE peripherals).
//
// Scope of this slice (per planning/2026-05-24 gap-doc row):
//   • Scan-and-discover loop with deduplicated peripherals,
//     RSSI, last_seen, and is_paired snapshots.
//   • Connect with state-machine callbacks (Connecting → Connected
//     | Failed, with translated BleMidiError).
//   • Inbound MIDI byte stream routed through BleMidiPacketDecoder.
//   • Outbound encode + GATT write helper (used by future
//     MidiOutput backend; this slice does not yet register the
//     output port with CoreMIDI).
//
// Pairing UI is deliberately not surfaced here — Apple ships
// CABTMIDICentralViewController (iOS) for that flow and the macOS
// equivalent. The host app drives the dialog after seeing the
// peripheral in the scan callback.

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <pulp/midi/ble_midi.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pulp::midi {
namespace {

// Apple BLE-MIDI 1.0 well-known UUIDs.
NSString* const kPulpBleMidiServiceUuid =
    @"03B80E5A-EDE8-4B33-A751-6CE34EC4C700";
NSString* const kPulpBleMidiCharUuid =
    @"7772E5DB-3868-4112-A1A9-F2669D106BF3";

class CoreBluetoothBleMidiCentral;

}  // namespace
}  // namespace pulp::midi

// Objective-C delegate that forwards CoreBluetooth callbacks into
// the C++ central. Kept in the global namespace because Apple's
// Foundation runtime registers classes by name and namespacing the
// @interface declaration is awkward.
@interface PulpBleMidiDelegate : NSObject <CBCentralManagerDelegate,
                                            CBPeripheralDelegate>
- (instancetype)initWithOwner:(pulp::midi::CoreBluetoothBleMidiCentral*)owner;
@end

namespace pulp::midi {
namespace {

class CoreBluetoothBleMidiCentral final : public BleMidiCentral {
public:
    CoreBluetoothBleMidiCentral() {
        @autoreleasepool {
            delegate_ = [[PulpBleMidiDelegate alloc] initWithOwner:this];
            // Initialise on the main dispatch queue per Apple's
            // recommendation; the delegate methods route work to our
            // mutex-protected state.
            manager_ = [[CBCentralManager alloc]
                initWithDelegate:delegate_
                           queue:dispatch_get_main_queue()
                         options:nil];
        }
    }

    ~CoreBluetoothBleMidiCentral() override {
        stop_scan();
        @autoreleasepool {
            manager_ = nil;
            delegate_ = nil;
        }
    }

    bool is_available() const override {
        // CBManagerStatePoweredOn == 5. Use the numeric value so we
        // don't drag CoreBluetooth into the header.
        return manager_state_.load() == 5;
    }

    bool start_scan(BleMidiScanCallback cb) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            scan_cb_ = std::move(cb);
        }
        if (!is_available()) return false;
        @autoreleasepool {
            NSArray<CBUUID*>* services =
                @[ [CBUUID UUIDWithString:kPulpBleMidiServiceUuid] ];
            [manager_ scanForPeripheralsWithServices:services options:nil];
        }
        scanning_.store(true);
        return true;
    }

    void stop_scan() override {
        if (!scanning_.exchange(false)) return;
        @autoreleasepool {
            [manager_ stopScan];
        }
    }

    bool is_scanning() const override { return scanning_.load(); }

    std::vector<BleMidiPeripheral> known_peripherals() const override {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<BleMidiPeripheral> out;
        out.reserve(peripherals_.size());
        for (const auto& [id, info] : peripherals_) out.push_back(info.snapshot);
        return out;
    }

    bool connect(const std::string& id) override {
        @autoreleasepool {
            CBPeripheral* peripheral = nil;
            {
                std::lock_guard<std::mutex> lock(mu_);
                auto it = peripherals_.find(id);
                if (it == peripherals_.end()) {
                    if (state_cb_) {
                        state_cb_(id, BleMidiConnectionState::Failed,
                                  BleMidiError::PeripheralNotFound);
                    }
                    return false;
                }
                peripheral = it->second.peripheral;
            }
            if (peripheral == nil) return false;
            if (state_cb_) {
                state_cb_(id, BleMidiConnectionState::Connecting,
                          BleMidiError::None);
            }
            [manager_ connectPeripheral:peripheral options:nil];
            return true;
        }
    }

    void disconnect(const std::string& id) override {
        @autoreleasepool {
            CBPeripheral* peripheral = nil;
            {
                std::lock_guard<std::mutex> lock(mu_);
                auto it = peripherals_.find(id);
                if (it == peripherals_.end()) return;
                peripheral = it->second.peripheral;
            }
            if (peripheral != nil) {
                [manager_ cancelPeripheralConnection:peripheral];
            }
        }
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

    // Called by the Objective-C delegate; public for `friend`-like
    // access without exposing the C++ type to Foundation.
    void on_state_change(int new_state) {
        manager_state_.store(new_state);
        if (new_state != 5) {
            // Adapter not powered on — surface the appropriate error.
            std::lock_guard<std::mutex> lock(mu_);
            if (state_cb_) {
                const BleMidiError err =
                    (new_state == 4 /* PoweredOff */) ? BleMidiError::BluetoothOff
                                                       : BleMidiError::Unsupported;
                // Report against the empty-id global slot so the host
                // knows the adapter changed.
                state_cb_("", BleMidiConnectionState::Disconnected, err);
            }
        }
    }

    void on_discovered(CBPeripheral* peripheral, NSNumber* rssi) {
        if (peripheral == nil) return;
        const std::string id =
            [peripheral.identifier.UUIDString UTF8String];
        BleMidiPeripheral snapshot;
        snapshot.id = id;
        snapshot.name = peripheral.name != nil
                            ? [peripheral.name UTF8String]
                            : std::string{};
        snapshot.rssi = rssi != nil ? rssi.intValue : 0;
        snapshot.last_seen = std::chrono::steady_clock::now();
        snapshot.is_paired = (peripheral.state == CBPeripheralStateConnected);

        BleMidiScanCallback cb_copy;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto& entry = peripherals_[id];
            entry.peripheral = peripheral;
            entry.snapshot = snapshot;
            cb_copy = scan_cb_;
        }
        if (cb_copy) cb_copy(snapshot);
    }

    void on_connected(CBPeripheral* peripheral) {
        if (peripheral == nil) return;
        const std::string id =
            [peripheral.identifier.UUIDString UTF8String];
        @autoreleasepool {
            peripheral.delegate = delegate_;
            NSArray<CBUUID*>* services =
                @[ [CBUUID UUIDWithString:kPulpBleMidiServiceUuid] ];
            [peripheral discoverServices:services];
        }
        // We report Connected only once the GATT service discovery
        // resolves the BLE-MIDI characteristic — see on_characteristic_ready.
    }

    void on_disconnected(CBPeripheral* peripheral, NSError* error) {
        if (peripheral == nil) return;
        const std::string id =
            [peripheral.identifier.UUIDString UTF8String];
        BleMidiStateCallback cb_copy;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = peripherals_.find(id);
            if (it != peripherals_.end()) {
                it->second.midi_input_port_id.clear();
                it->second.midi_output_port_id.clear();
                it->second.decoder.reset();
            }
            cb_copy = state_cb_;
        }
        if (cb_copy) {
            const BleMidiError err = error != nil ? BleMidiError::ConnectFailed
                                                   : BleMidiError::None;
            cb_copy(id, BleMidiConnectionState::Disconnected, err);
        }
    }

    void on_characteristic_ready(CBPeripheral* peripheral) {
        if (peripheral == nil) return;
        const std::string id =
            [peripheral.identifier.UUIDString UTF8String];
        BleMidiStateCallback cb_copy;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = peripherals_.find(id);
            if (it != peripherals_.end()) {
                // Synthesize the MIDI port ids; future slices will
                // register CoreMIDI virtual endpoints and use the
                // assigned MIDIUniqueID instead.
                it->second.midi_input_port_id  = "ble-midi-in:" + id;
                it->second.midi_output_port_id = "ble-midi-out:" + id;
            }
            cb_copy = state_cb_;
        }
        if (cb_copy) {
            cb_copy(id, BleMidiConnectionState::Connected, BleMidiError::None);
        }
    }

    void on_notification(CBPeripheral* peripheral, NSData* value) {
        if (peripheral == nil || value == nil || value.length == 0) return;
        const std::string id =
            [peripheral.identifier.UUIDString UTF8String];
        std::lock_guard<std::mutex> lock(mu_);
        auto it = peripherals_.find(id);
        if (it == peripherals_.end()) return;
        const uint8_t* bytes = static_cast<const uint8_t*>(value.bytes);
        it->second.decoder.decode(bytes, value.length);
    }

private:
    struct Entry {
        CBPeripheral* peripheral = nil;
        BleMidiPeripheral snapshot{};
        std::string midi_input_port_id;
        std::string midi_output_port_id;
        BleMidiPacketDecoder decoder;
    };

    mutable std::mutex mu_;
    std::map<std::string, Entry> peripherals_;
    BleMidiScanCallback scan_cb_;
    BleMidiStateCallback state_cb_;
    std::atomic<int> manager_state_{0};
    std::atomic<bool> scanning_{false};

    CBCentralManager* manager_ = nil;
    PulpBleMidiDelegate* delegate_ = nil;
};

}  // namespace

std::unique_ptr<BleMidiCentral> create_ble_midi_central() {
    return std::make_unique<CoreBluetoothBleMidiCentral>();
}

}  // namespace pulp::midi

@implementation PulpBleMidiDelegate {
    pulp::midi::CoreBluetoothBleMidiCentral* _owner;
}
- (instancetype)initWithOwner:(pulp::midi::CoreBluetoothBleMidiCentral*)owner {
    if ((self = [super init])) { _owner = owner; }
    return self;
}
- (void)centralManagerDidUpdateState:(CBCentralManager*)central {
    if (_owner != nullptr) {
        _owner->on_state_change(static_cast<int>(central.state));
    }
}
- (void)centralManager:(CBCentralManager*)central
 didDiscoverPeripheral:(CBPeripheral*)peripheral
     advertisementData:(NSDictionary<NSString*, id>*)advertisementData
                  RSSI:(NSNumber*)RSSI {
    (void)advertisementData;
    if (_owner != nullptr) _owner->on_discovered(peripheral, RSSI);
}
- (void)centralManager:(CBCentralManager*)central
  didConnectPeripheral:(CBPeripheral*)peripheral {
    (void)central;
    if (_owner != nullptr) _owner->on_connected(peripheral);
}
- (void)centralManager:(CBCentralManager*)central
didDisconnectPeripheral:(CBPeripheral*)peripheral
                 error:(NSError*)error {
    (void)central;
    if (_owner != nullptr) _owner->on_disconnected(peripheral, error);
}
- (void)peripheral:(CBPeripheral*)peripheral
didDiscoverServices:(NSError*)error {
    if (error != nil || peripheral.services.count == 0) return;
    for (CBService* svc in peripheral.services) {
        if ([svc.UUID.UUIDString isEqualToString:
                 pulp::midi::kPulpBleMidiServiceUuid]) {
            CBUUID* characteristicUuid =
                [CBUUID UUIDWithString:pulp::midi::kPulpBleMidiCharUuid];
            [peripheral discoverCharacteristics:@[ characteristicUuid ]
                                      forService:svc];
        }
    }
}
- (void)peripheral:(CBPeripheral*)peripheral
didDiscoverCharacteristicsForService:(CBService*)service
             error:(NSError*)error {
    if (error != nil) return;
    for (CBCharacteristic* c in service.characteristics) {
        if ([c.UUID.UUIDString isEqualToString:
                 pulp::midi::kPulpBleMidiCharUuid]) {
            [peripheral setNotifyValue:YES forCharacteristic:c];
            if (_owner != nullptr) _owner->on_characteristic_ready(peripheral);
        }
    }
}
- (void)peripheral:(CBPeripheral*)peripheral
didUpdateValueForCharacteristic:(CBCharacteristic*)characteristic
             error:(NSError*)error {
    if (error != nil || characteristic.value == nil) return;
    if (_owner != nullptr) _owner->on_notification(peripheral, characteristic.value);
}
@end
