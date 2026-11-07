// A MIDI input must notice when the device it is connected to goes away and
// comes back, which is what happens when the host sleeps and the OS re-creates
// its USB and Bluetooth endpoints. The endpoint object is replaced; a port
// connected to the old object receives nothing and says nothing.
//
// Drives the real CoreMIDI framework with in-process virtual endpoints, so no
// hardware is needed. Skips when no MIDI server is reachable (some sandboxes).

#include <catch2/catch_test_macros.hpp>

#include <pulp/midi/device.hpp>

#include <CoreMIDI/CoreMIDI.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace {

constexpr SInt32 kVirtualId = 0x50554C50;  // arbitrary, unlikely to collide

struct VirtualSource {
    MIDIClientRef client = 0;
    MIDIEndpointRef source = 0;

    bool create() {
        if (client == 0 &&
            MIDIClientCreate(CFSTR("PulpReconnectTest"), nullptr, nullptr, &client) != noErr) {
            return false;
        }
        if (MIDISourceCreate(client, CFSTR("Pulp Reconnect Source"), &source) != noErr) {
            return false;
        }
        MIDIObjectSetIntegerProperty(source, kMIDIPropertyUniqueID, kVirtualId);
        return true;
    }

    void destroy() {
        if (source) { MIDIEndpointDispose(source); source = 0; }
    }

    ~VirtualSource() {
        destroy();
        if (client) MIDIClientDispose(client);
    }
};

bool wait_for(const std::atomic<int>& counter, int at_least) {
    for (int i = 0; i < 200 && counter.load() < at_least; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return counter.load() >= at_least;
}

void send_note(MIDIEndpointRef source) {
    MIDIEventList list;
    MIDIEventPacket* packet = MIDIEventListInit(&list, kMIDIProtocol_1_0);
    const UInt32 word = 0x20903C64;  // note on, channel 0, note 60, velocity 100
    MIDIEventListAdd(&list, sizeof(list), packet, 0, 1, &word);
    MIDIReceivedEventList(source, &list);
}

}  // namespace

TEST_CASE("CoreMIDI input reports closed when its source is replaced, and reopens",
          "[midi][coremidi][reconnect]") {
    VirtualSource vs;
    if (!vs.create()) SKIP("CoreMIDI server not reachable; skipping");

    auto system = pulp::midi::create_midi_system();
    REQUIRE(system != nullptr);
    const std::string id = std::to_string(kVirtualId);

    std::atomic<int> received{0};
    auto input = system->create_input();
    REQUIRE(input->open(id, [&](const pulp::midi::MidiEvent&) { ++received; }));
    REQUIRE(input->is_open());

    send_note(vs.source);
    REQUIRE(wait_for(received, 1));

    // The device goes away (sleep, unplug)...
    vs.destroy();
    REQUIRE_FALSE(input->is_open());

    // ...and returns under the same identifier as a new endpoint object. The
    // old handle must still report closed: it is connected to the dead object.
    REQUIRE(vs.create());
    REQUIRE_FALSE(input->is_open());

    // Reopening on the same identifier connects to the new object.
    input->close();
    REQUIRE(input->open(id, [&](const pulp::midi::MidiEvent&) { ++received; }));
    REQUIRE(input->is_open());
    send_note(vs.source);
    REQUIRE(wait_for(received, 2));
}

TEST_CASE("CoreMIDI output stops reporting open when its destination is replaced",
          "[midi][coremidi][reconnect]") {
    MIDIClientRef client = 0;
    if (MIDIClientCreate(CFSTR("PulpReconnectTestOut"), nullptr, nullptr, &client) != noErr) {
        SKIP("CoreMIDI server not reachable; skipping");
    }
    MIDIEndpointRef dest = 0;
    MIDIReadBlock sink = ^(const MIDIPacketList*, void*) {};
    REQUIRE(MIDIDestinationCreateWithBlock(client, CFSTR("Pulp Reconnect Dest"), &dest, sink) == noErr);
    MIDIObjectSetIntegerProperty(dest, kMIDIPropertyUniqueID, kVirtualId + 1);

    auto system = pulp::midi::create_midi_system();
    auto output = system->create_output();
    REQUIRE(output->open(std::to_string(kVirtualId + 1)));
    REQUIRE(output->is_open());

    MIDIEndpointDispose(dest);
    REQUIRE_FALSE(output->is_open());
    output->close();
    MIDIClientDispose(client);
}
