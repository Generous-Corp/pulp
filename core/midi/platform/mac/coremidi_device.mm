#include <pulp/midi/device.hpp>
#include <pulp/midi/ump.hpp>
#include <pulp/midi/ump_conversion.hpp>
#include <pulp/midi/ump_sysex7_reassembler.hpp>
#include <pulp/runtime/log.hpp>
#include <CoreMIDI/CoreMIDI.h>
#include <TargetConditionals.h>
#include <mach/mach_time.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#endif
#include <dispatch/dispatch.h>

#include <memory>
#include <mutex>

#include "coremidi_shared_client.h"

namespace pulp::midi::mac {

namespace {

bool event_list_api_available() {
#if TARGET_OS_IPHONE
    if (@available(iOS 14.0, *)) return true;
#else
    if (@available(macOS 11.0, *)) return true;
#endif
    return false;
}

double host_ticks_to_seconds(MIDITimeStamp ticks) {
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t value{};
        mach_timebase_info(&value);
        return value;
    }();
    if (timebase.denom == 0) return 0.0;
    return static_cast<double>(static_cast<long double>(ticks) *
                               static_cast<long double>(timebase.numer) /
                               static_cast<long double>(timebase.denom) /
                               1.0e9L);
}

// True while `endpoint` is still the live object registered under
// `unique_id`. A device that is unplugged, or that disappears across a system
// sleep, takes its endpoint object with it; when it comes back it is a new
// object under the same unique id. A port connected to the old object receives
// nothing, but nothing tells the port so. Comparing the registered object with
// the one we connected to detects exactly that.
bool endpoint_is_current(MIDIEndpointRef endpoint, SInt32 unique_id) {
    if (endpoint == 0) return false;
    if (unique_id == 0) return true;  // No id to verify against.
    MIDIObjectRef found = 0;
    MIDIObjectType type = kMIDIObjectType_Other;
    const OSStatus status = MIDIObjectFindByUniqueID(unique_id, &found, &type);
    return status == noErr && found == endpoint;
}

}  // namespace

// A CoreMIDI client is a per-process registration with the system
// MIDIServer, not a per-port handle. Creating one per input and per output
// multiplies those registrations by the number of open ports — and a plug-in
// loaded at many instances multiplies that again. Every port in this process
// shares the one client below.
//
// The client is created on first use and deliberately never disposed:
// CoreMIDI ties port and endpoint lifetime to the owning client, so disposing
// it while any other port is still open would invalidate that port. It is
// released when the process exits.
MIDIClientRef shared_client() {
    // Function-local static initialisation is thread-safe, so concurrent
    // first opens cannot race two clients into existence.
    static const MIDIClientRef client = [] {
        MIDIClientRef created = 0;
        const OSStatus status =
            MIDIClientCreate(CFSTR("Pulp"), nullptr, nullptr, &created);
        if (status != noErr) {
            runtime::log_error("CoreMIDI: could not create MIDI client ({})",
                               static_cast<int>(status));
            return MIDIClientRef{0};
        }
        return created;
    }();
    return client;
}

class CoreMidiInput : public MidiInput {
public:
    ~CoreMidiInput() override { close(); }

    bool open(const std::string& port_id, MidiInputCallback callback) override {
        callback_ = std::move(callback);

        // MIDIInputPortCreateWithProtocol is the UMP/event-list API added in
        // iOS 14 and macOS 11. Pulp's current iOS floor is newer, but retain a
        // runtime guard so an SDK consumer cannot weak-link and call it on an
        // older OS by accident.
        if (!event_list_api_available()) return false;

        const MIDIClientRef client = shared_client();
        if (client == 0) return false;

        OSStatus status = MIDIInputPortCreateWithProtocol(client, CFSTR("PulpInput"),
            kMIDIProtocol_1_0, &port_,
            ^(const MIDIEventList* evtlist, void* __nullable) {
                const MIDIEventPacket* packet = &evtlist->packet[0];
                for (UInt32 i = 0; i < evtlist->numPackets; ++i) {
                    walk_ump_packet(
                        packet->words, packet->wordCount,
                        [&](uint8_t type, const uint32_t* words,
                            uint32_t word_count) {
                        const uint32_t word = words[0];
                        if (type == 0x02) {
                            MidiEvent evt;
                            evt.message = choc::midi::ShortMessage(
                                static_cast<uint8_t>((word >> 16) & 0xFF),
                                static_cast<uint8_t>((word >> 8) & 0xFF),
                                static_cast<uint8_t>(word & 0xFF));
                            evt.timestamp = host_ticks_to_seconds(packet->timeStamp);
                            if (this->callback_) this->callback_(evt);
                        } else if (type == 0x04) {
                            UmpPacket p;
                            p.word_count = 2;
                            p.words[0] = word;
                            p.words[1] = words[1];
                            MidiEvent evt{};
                            if (ump_to_midi1_event(p, evt)) {
                                evt.timestamp =
                                        host_ticks_to_seconds(packet->timeStamp);
                                if (this->callback_) this->callback_(evt);
                            }
                        } else if (type == 0x03) {
                            // SysEx7 (UMP type 3) — reassemble multi-
                            // packet payloads via the shared
                            // UmpSysex7Reassembler. The reassembler state
                            // lives on this CoreMidiInput so a multi-packet
                            // sysex spanning callback invocations accumulates
                            // correctly and shares the tested state machine
                            // with the AUv3 adapter.
                            const uint32_t word1 = words[1];
                            struct EmitCtx {
                                MidiSysexCallback* cb;
                                double ts_sec;
                            };
                            EmitCtx ctx{&this->sysex_callback_,
                                        host_ticks_to_seconds(packet->timeStamp)};
                            auto emit = [](const std::vector<uint8_t>& p,
                                           void* user) {
                                auto* c = static_cast<EmitCtx*>(user);
                                if (*c->cb) (*c->cb)(p, c->ts_sec);
                            };
                            this->sysex_reassembler_.feed_packet(
                                word, word1, emit, &ctx);
                        }
                        // Other UMP types (utility, system, SysEx8,
                        // stream, flex) are ignored by this MIDI 1.0 input
                        // adapter for now.
                        (void)word_count;
                    });
                    packet = MIDIEventPacketNext(packet);
                }
            });

        if (status != noErr) {
            runtime::log_error("CoreMIDI: could not create input port ({})", static_cast<int>(status));
            close();
            return false;
        }

        ItemCount source_count = MIDIGetNumberOfSources();
        if (port_id.empty() || port_id == "0") {
            if (source_count > 0) {
                MIDIEndpointRef first = MIDIGetSource(0);
                status = MIDIPortConnectSource(port_, first, nullptr);
                is_open_ = (status == noErr);
                if (is_open_) remember_source(first);
            }
            if (!is_open_) close();
            return is_open_;
        }

        SInt32 source_id = 0;
        try {
            source_id = static_cast<SInt32>(std::stol(port_id));
        } catch (...) {
            close();
            return false;
        }

        // Find the source endpoint matching this ID.
        for (ItemCount i = 0; i < source_count; ++i) {
            MIDIEndpointRef src = MIDIGetSource(i);
            SInt32 unique_id = 0;
            MIDIObjectGetIntegerProperty(src, kMIDIPropertyUniqueID, &unique_id);
            if (unique_id == source_id) {
                status = MIDIPortConnectSource(port_, src, nullptr);
                if (status == noErr) {
                    is_open_ = true;
                    remember_source(src);
                    return true;
                }
            }
        }

        close();
        return false;
    }

    void close() override {
        // Only the port is ours to dispose; the client is shared by every
        // port in the process (see shared_client()).
        if (port_) { MIDIPortDispose(port_); port_ = 0; }
        // Drop any in-progress sysex so a reopened port starts fresh
        // and never emits a stale partial payload.
        sysex_reassembler_.reset();
        source_ = 0;
        source_unique_id_ = 0;
        is_open_ = false;
    }

    // Open means the port exists AND the endpoint it was connected to is
    // still the one registered under that id. After a sleep or unplug the
    // answer turns false, which tells an owner that reopens on `!is_open()`
    // (AudioDeviceManager) to connect again.
    bool is_open() const override {
        return is_open_ && endpoint_is_current(source_, source_unique_id_);
    }

    void set_sysex_callback(MidiSysexCallback cb) override {
        sysex_callback_ = std::move(cb);
    }

private:
    void remember_source(MIDIEndpointRef source) {
        source_ = source;
        source_unique_id_ = 0;
        MIDIObjectGetIntegerProperty(source, kMIDIPropertyUniqueID, &source_unique_id_);
    }

    MIDIPortRef port_ = 0;
    MIDIEndpointRef source_ = 0;
    SInt32 source_unique_id_ = 0;
    MidiInputCallback callback_;
    MidiSysexCallback sysex_callback_;
    // SysEx7 (UMP type 0x03) reassembly state — shared implementation
    // in core/midi/include/pulp/midi/ump_sysex7_reassembler.hpp. The
    // OS callback fires once per MIDIEventList delivery, so a multi-
    // packet sysex (start → continue* → end) spans callback invocations
    // and the reassembler keeps the partial payload between deliveries.
    UmpSysex7Reassembler sysex_reassembler_;
    bool is_open_ = false;
};

class CoreMidiOutput : public MidiOutput {
public:
    ~CoreMidiOutput() override { close(); }

    bool open(const std::string& port_id) override {
        if (!event_list_api_available()) return false;

        const MIDIClientRef client = shared_client();
        if (client == 0) return false;

        OSStatus status = MIDIOutputPortCreate(client, CFSTR("PulpOutput"), &port_);
        if (status != noErr) { close(); return false; }

        // Find destination
        ItemCount dest_count = MIDIGetNumberOfDestinations();
        if (dest_count > 0) {
            if (port_id.empty() || port_id == "0") {
                dest_ = MIDIGetDestination(0);
            } else {
                SInt32 target_id = 0;
                try {
                    target_id = static_cast<SInt32>(std::stol(port_id));
                } catch (...) {
                    close();
                    return false;
                }
                for (ItemCount i = 0; i < dest_count; ++i) {
                    SInt32 unique_id = 0;
                    MIDIObjectGetIntegerProperty(MIDIGetDestination(i), kMIDIPropertyUniqueID, &unique_id);
                    if (unique_id == target_id) {
                        dest_ = MIDIGetDestination(i);
                        break;
                    }
                }
            }
        }

        is_open_ = (dest_ != 0);
        if (is_open_) {
            dest_unique_id_ = 0;
            MIDIObjectGetIntegerProperty(dest_, kMIDIPropertyUniqueID, &dest_unique_id_);
        } else {
            close();
        }
        return is_open_;
    }

    void close() override {
        // The client is shared process-wide and outlives this port.
        if (port_) { MIDIPortDispose(port_); port_ = 0; }
        dest_ = 0;
        dest_unique_id_ = 0;
        is_open_ = false;
    }

    // See CoreMidiInput::is_open(): a destination replaced across a sleep or
    // unplug is no longer the object this port was opened on.
    bool is_open() const override {
        return is_open_ && endpoint_is_current(dest_, dest_unique_id_);
    }

    void send(const MidiEvent& event) override {
        if (!is_open_ || !dest_ || !endpoint_is_current(dest_, dest_unique_id_)) return;

        // Build a MIDI 1.0 UMP word
        const auto* d = event.data();
        uint32_t word = 0x20000000; // Type 2: MIDI 1.0 channel voice
        word |= (static_cast<uint32_t>(d[0]) << 16);
        word |= (static_cast<uint32_t>(d[1]) << 8);
        word |= static_cast<uint32_t>(d[2]);

        MIDIEventList list;
        MIDIEventPacket* packet = MIDIEventListInit(&list, kMIDIProtocol_1_0);
        packet = MIDIEventListAdd(&list, sizeof(list), packet, 0, 1, &word);

        MIDISendEventList(port_, dest_, &list);
    }

private:
    MIDIPortRef port_ = 0;
    MIDIEndpointRef dest_ = 0;
    SInt32 dest_unique_id_ = 0;
    bool is_open_ = false;
};

class CoreMidiSystem : public MidiSystem {
public:
    CoreMidiSystem() : notify_(std::make_shared<NotifyState>()) {}

    ~CoreMidiSystem() override {
        notify_->set({});
        remove_wake_observer();
        if (system_client_) {
            MIDIClientDispose(system_client_);
            system_client_ = 0;
        }
    }

    void set_port_change_callback(PortChangeCallback cb) override {
        const bool have_callback = static_cast<bool>(cb);
        notify_->set(std::move(cb));

        if (!have_callback) {
            remove_wake_observer();
            return;
        }

        // Create a persistent client with notification callback for hotplug.
        // The block holds the shared notify state, not `this`, so a late
        // notification after destruction finds an empty callback.
        if (!system_client_) {
            auto notify = notify_;
            OSStatus status = MIDIClientCreateWithBlock(
                CFSTR("PulpMIDISystem"),
                &system_client_,
                ^(const MIDINotification* notification) {
                    if (notification->messageID == kMIDIMsgSetupChanged) {
                        notify->fire();
                    }
                });
            if (status != noErr) {
                runtime::log_warn("CoreMIDI: could not create system client for notifications ({})",
                                  static_cast<int>(status));
            }
        }
        install_wake_observer();
    }

    std::vector<MidiPortInfo> enumerate_inputs() override {
        std::vector<MidiPortInfo> ports;
        ItemCount count = MIDIGetNumberOfSources();
        for (ItemCount i = 0; i < count; ++i) {
            MIDIEndpointRef src = MIDIGetSource(i);
            MidiPortInfo info;

            SInt32 unique_id = 0;
            MIDIObjectGetIntegerProperty(src, kMIDIPropertyUniqueID, &unique_id);
            info.id = std::to_string(unique_id);

            CFStringRef name = nullptr;
            MIDIObjectGetStringProperty(src, kMIDIPropertyDisplayName, &name);
            if (name) {
                char buf[256];
                CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
                info.name = buf;
                CFRelease(name);
            }
            info.is_input = true;
            ports.push_back(std::move(info));
        }
        return ports;
    }

    std::vector<MidiPortInfo> enumerate_outputs() override {
        std::vector<MidiPortInfo> ports;
        ItemCount count = MIDIGetNumberOfDestinations();
        for (ItemCount i = 0; i < count; ++i) {
            MIDIEndpointRef dest = MIDIGetDestination(i);
            MidiPortInfo info;

            SInt32 unique_id = 0;
            MIDIObjectGetIntegerProperty(dest, kMIDIPropertyUniqueID, &unique_id);
            info.id = std::to_string(unique_id);

            CFStringRef name = nullptr;
            MIDIObjectGetStringProperty(dest, kMIDIPropertyDisplayName, &name);
            if (name) {
                char buf[256];
                CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
                info.name = buf;
                CFRelease(name);
            }
            info.is_output = true;
            ports.push_back(std::move(info));
        }
        return ports;
    }

    std::unique_ptr<MidiInput> create_input() override {
        return std::make_unique<CoreMidiInput>();
    }

    std::unique_ptr<MidiOutput> create_output() override {
        return std::make_unique<CoreMidiOutput>();
    }

private:
    // Callback slot shared with the blocks the OS holds, so they never
    // dereference a destroyed system.
    struct NotifyState {
        void set(PortChangeCallback cb) {
            std::lock_guard<std::mutex> lk(mu);
            callback = std::move(cb);
        }
        void fire() {
            PortChangeCallback current;
            {
                std::lock_guard<std::mutex> lk(mu);
                current = callback;
            }
            if (current) current();
        }
        std::mutex mu;
        PortChangeCallback callback;
    };

    // After the host wakes from sleep, endpoints that vanished during sleep
    // return as new objects. CoreMIDI posts a setup change for that, but a
    // connection can also be lost without one, so wake itself reports a port
    // change: once straight away, and once more after the devices have had
    // time to re-enumerate. Consumers that reopen on a changed port list then
    // reconnect without any user action.
    void install_wake_observer() {
#if TARGET_OS_OSX
        if (wake_observer_) return;
        auto notify = notify_;
        id token = [[[NSWorkspace sharedWorkspace] notificationCenter]
            addObserverForName:NSWorkspaceDidWakeNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) {
                        notify->fire();
                        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                                       dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                                           notify->fire();
                                       });
                    }];
        wake_observer_ = token ? CFRetain((__bridge CFTypeRef)token) : nullptr;
#endif
    }

    void remove_wake_observer() {
#if TARGET_OS_OSX
        if (!wake_observer_) return;
        [[[NSWorkspace sharedWorkspace] notificationCenter]
            removeObserver:(__bridge id)wake_observer_];
        CFRelease(wake_observer_);
        wake_observer_ = nullptr;
#endif
    }

    MIDIClientRef system_client_ = 0;
    std::shared_ptr<NotifyState> notify_;
    CFTypeRef wake_observer_ = nullptr;
};

} // namespace pulp::midi::mac

namespace pulp::midi {

std::unique_ptr<MidiSystem> create_midi_system() {
    return std::make_unique<mac::CoreMidiSystem>();
}

} // namespace pulp::midi
