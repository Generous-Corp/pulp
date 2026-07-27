#include <pulp/midi/device.hpp>
#include <pulp/midi/ump.hpp>
#include <pulp/midi/ump_conversion.hpp>
#include <pulp/midi/ump_sysex7_reassembler.hpp>
#include <pulp/runtime/log.hpp>
#include <CoreMIDI/CoreMIDI.h>

#include "coremidi_shared_client.h"

namespace pulp::midi::mac {

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

        const MIDIClientRef client = shared_client();
        if (client == 0) return false;

        OSStatus status = MIDIInputPortCreateWithProtocol(client, CFSTR("PulpInput"),
            kMIDIProtocol_1_0, &port_,
            ^(const MIDIEventList* evtlist, void* __nullable) {
                // Walk UMP messages by their declared word size (MIDI 2.0
                // spec M2-104-UM) so multi-word packets advance the cursor
                // as a single message. Otherwise a type-0x04 packet's
                // second word can be mis-parsed as a new message header.
                static constexpr uint8_t kWordsByType[16] = {
                    1, 1, 1, 2, 2, 4, 4, 1,
                    2, 2, 2, 3, 3, 4, 4, 4
                };
                const MIDIEventPacket* packet = &evtlist->packet[0];
                for (UInt32 i = 0; i < evtlist->numPackets; ++i) {
                    UInt32 wordIdx = 0;
                    while (wordIdx < packet->wordCount) {
                        uint32_t word = packet->words[wordIdx];
                        uint8_t type = (word >> 28) & 0x0F;
                        const uint8_t words_in_message = kWordsByType[type];
                        if (wordIdx + words_in_message > packet->wordCount) break;

                        if (type == 0x02) {
                            MidiEvent evt;
                            evt.message = choc::midi::ShortMessage(
                                static_cast<uint8_t>((word >> 16) & 0xFF),
                                static_cast<uint8_t>((word >> 8) & 0xFF),
                                static_cast<uint8_t>(word & 0xFF));
                            evt.timestamp = static_cast<double>(packet->timeStamp) / 1e9;
                            if (this->callback_) this->callback_(evt);
                        } else if (type == 0x04) {
                            UmpPacket p;
                            p.word_count = 2;
                            p.words[0] = word;
                            p.words[1] = packet->words[wordIdx + 1];
                            MidiEvent evt{};
                            if (ump_to_midi1_event(p, evt)) {
                                evt.timestamp =
                                    static_cast<double>(packet->timeStamp) / 1e9;
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
                            const uint32_t word1 =
                                packet->words[wordIdx + 1];
                            struct EmitCtx {
                                MidiSysexCallback* cb;
                                double ts_sec;
                            };
                            EmitCtx ctx{&this->sysex_callback_,
                                        static_cast<double>(packet->timeStamp) / 1e9};
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
                        wordIdx += words_in_message;
                    }
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
                status = MIDIPortConnectSource(port_, MIDIGetSource(0), nullptr);
                is_open_ = (status == noErr);
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
        is_open_ = false;
    }

    bool is_open() const override { return is_open_; }

    void set_sysex_callback(MidiSysexCallback cb) override {
        sysex_callback_ = std::move(cb);
    }

private:
    MIDIPortRef port_ = 0;
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
        if (!is_open_) close();
        return is_open_;
    }

    void close() override {
        // The client is shared process-wide and outlives this port.
        if (port_) { MIDIPortDispose(port_); port_ = 0; }
        dest_ = 0;
        is_open_ = false;
    }

    bool is_open() const override { return is_open_; }

    void send(const MidiEvent& event) override {
        if (!is_open_ || !dest_) return;

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
    bool is_open_ = false;
};

class CoreMidiSystem : public MidiSystem {
public:
    CoreMidiSystem() = default;

    ~CoreMidiSystem() override {
        if (system_client_) {
            MIDIClientDispose(system_client_);
            system_client_ = 0;
        }
    }

    void set_port_change_callback(PortChangeCallback cb) override {
        port_change_cb_ = std::move(cb);

        // Create a persistent client with notification callback for hotplug
        if (port_change_cb_ && !system_client_) {
            OSStatus status = MIDIClientCreateWithBlock(
                CFSTR("PulpMIDISystem"),
                &system_client_,
                ^(const MIDINotification* notification) {
                    if (notification->messageID == kMIDIMsgSetupChanged) {
                        if (this->port_change_cb_) {
                            this->port_change_cb_();
                        }
                    }
                });
            if (status != noErr) {
                runtime::log_warn("CoreMIDI: could not create system client for notifications ({})",
                                  static_cast<int>(status));
            }
        }
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
    MIDIClientRef system_client_ = 0;
    PortChangeCallback port_change_cb_;
};

} // namespace pulp::midi::mac

namespace pulp::midi {

std::unique_ptr<MidiSystem> create_midi_system() {
    return std::make_unique<mac::CoreMidiSystem>();
}

} // namespace pulp::midi
