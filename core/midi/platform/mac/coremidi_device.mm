#include <pulp/midi/device.hpp>
#include <pulp/runtime/log.hpp>
#include <CoreMIDI/CoreMIDI.h>

namespace pulp::midi::mac {

class CoreMidiInput : public MidiInput {
public:
    ~CoreMidiInput() override { close(); }

    bool open(const std::string& port_id, MidiInputCallback callback) override {
        callback_ = std::move(callback);

        OSStatus status = MIDIClientCreate(CFSTR("PulpMIDIIn"), nullptr, nullptr, &client_);
        if (status != noErr) {
            runtime::log_error("CoreMIDI: could not create input client ({})", static_cast<int>(status));
            return false;
        }

        status = MIDIInputPortCreateWithProtocol(client_, CFSTR("PulpInput"),
            kMIDIProtocol_1_0, &port_,
            ^(const MIDIEventList* evtlist, void* __nullable) {
                // Parse MIDI 1.0 events from the event list
                const MIDIEventPacket* packet = &evtlist->packet[0];
                for (UInt32 i = 0; i < evtlist->numPackets; ++i) {
                    for (UInt32 wordIdx = 0; wordIdx < packet->wordCount; ++wordIdx) {
                        uint32_t word = packet->words[wordIdx];
                        uint8_t type = (word >> 28) & 0x0F;

                        if (type == 0x02) { // MIDI 1.0 channel voice message
                            MidiEvent evt;
                            evt.message = choc::midi::ShortMessage(
                                static_cast<uint8_t>((word >> 16) & 0xFF),
                                static_cast<uint8_t>((word >> 8) & 0xFF),
                                static_cast<uint8_t>(word & 0xFF));
                            evt.timestamp = static_cast<double>(packet->timeStamp) / 1e9;
                            if (this->callback_) this->callback_(evt);
                        }
                    }
                    packet = MIDIEventPacketNext(packet);
                }
            });

        if (status != noErr) {
            runtime::log_error("CoreMIDI: could not create input port ({})", static_cast<int>(status));
            close();
            return false;
        }

        // Connect to the specified source
        auto source_id = static_cast<MIDIObjectRef>(std::stoul(port_id));
        // Find the source endpoint matching this ID
        ItemCount source_count = MIDIGetNumberOfSources();
        for (ItemCount i = 0; i < source_count; ++i) {
            MIDIEndpointRef src = MIDIGetSource(i);
            SInt32 unique_id = 0;
            MIDIObjectGetIntegerProperty(src, kMIDIPropertyUniqueID, &unique_id);
            if (static_cast<MIDIObjectRef>(unique_id) == source_id) {
                status = MIDIPortConnectSource(port_, src, nullptr);
                if (status == noErr) {
                    is_open_ = true;
                    return true;
                }
            }
        }

        // If port_id is "0" or empty, connect to first available source
        if (source_count > 0) {
            status = MIDIPortConnectSource(port_, MIDIGetSource(0), nullptr);
            is_open_ = (status == noErr);
        }

        return is_open_;
    }

    void close() override {
        if (port_) { MIDIPortDispose(port_); port_ = 0; }
        if (client_) { MIDIClientDispose(client_); client_ = 0; }
        is_open_ = false;
    }

    bool is_open() const override { return is_open_; }

private:
    MIDIClientRef client_ = 0;
    MIDIPortRef port_ = 0;
    MidiInputCallback callback_;
    bool is_open_ = false;
};

class CoreMidiOutput : public MidiOutput {
public:
    ~CoreMidiOutput() override { close(); }

    bool open(const std::string& port_id) override {
        OSStatus status = MIDIClientCreate(CFSTR("PulpMIDIOut"), nullptr, nullptr, &client_);
        if (status != noErr) return false;

        status = MIDIOutputPortCreate(client_, CFSTR("PulpOutput"), &port_);
        if (status != noErr) { close(); return false; }

        // Find destination
        ItemCount dest_count = MIDIGetNumberOfDestinations();
        if (dest_count > 0) {
            if (port_id.empty() || port_id == "0") {
                dest_ = MIDIGetDestination(0);
            } else {
                auto target_id = std::stoi(port_id);
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
        return is_open_;
    }

    void close() override {
        if (port_) { MIDIPortDispose(port_); port_ = 0; }
        if (client_) { MIDIClientDispose(client_); client_ = 0; }
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
    MIDIClientRef client_ = 0;
    MIDIPortRef port_ = 0;
    MIDIEndpointRef dest_ = 0;
    bool is_open_ = false;
};

class CoreMidiSystem : public MidiSystem {
public:
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
};

} // namespace pulp::midi::mac

namespace pulp::midi {

std::unique_ptr<MidiSystem> create_midi_system() {
    return std::make_unique<mac::CoreMidiSystem>();
}

} // namespace pulp::midi
