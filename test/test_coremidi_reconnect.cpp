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

constexpr SInt32 kVirtualId = 0x50554C50; // arbitrary, unlikely to collide

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
        if (source) {
            MIDIEndpointDispose(source);
            source = 0;
        }
    }

    ~VirtualSource() {
        destroy();
        if (client)
            MIDIClientDispose(client);
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
    const UInt32 word = 0x20903C64; // note on, channel 0, note 60, velocity 100
    MIDIEventListAdd(&list, sizeof(list), packet, 0, 1, &word);
    MIDIReceivedEventList(source, &list);
}

} // namespace

TEST_CASE("CoreMIDI input reports closed when its source is replaced, and reopens",
          "[midi][coremidi][reconnect]") {
    VirtualSource vs;
    if (!vs.create())
        SKIP("CoreMIDI server not reachable; skipping");

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
    MIDIReadBlock sink = ^(const MIDIPacketList*, void*) {
    };
    REQUIRE(MIDIDestinationCreateWithBlock(client, CFSTR("Pulp Reconnect Dest"), &dest, sink) ==
            noErr);
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

// ---------------------------------------------------------------------------
// close() is a barrier: once it returns, no input callback is running and none
// will run again. An owner destroys whatever the callback captured right after
// close(), so a callback still in flight is a use-after-free.
// ---------------------------------------------------------------------------

#include <unistd.h>

#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>

namespace {

constexpr SInt32 kDrainId = 0x50554C51;

struct DrainSource {
    MIDIClientRef client = 0;
    MIDIEndpointRef source = 0;
    // The virtual source is visible system-wide while it exists, so it carries
    // an obvious test name and the pid, and is disposed when the test ends.
    bool create() {
        const std::string name = "pulp-test-coremidi-" + std::to_string(getpid());
        CFStringRef cf_name =
            CFStringCreateWithCString(kCFAllocatorDefault, name.c_str(), kCFStringEncodingUTF8);
        const OSStatus client_status = MIDIClientCreate(cf_name, nullptr, nullptr, &client);
        if (client_status != noErr) {
            CFRelease(cf_name);
            return false;
        }
        const OSStatus source_status = MIDISourceCreate(client, cf_name, &source);
        CFRelease(cf_name);
        if (source_status != noErr)
            return false;
        MIDIObjectSetIntegerProperty(source, kMIDIPropertyUniqueID, kDrainId);
        return true;
    }
    ~DrainSource() {
        if (source)
            MIDIEndpointDispose(source);
        if (client)
            MIDIClientDispose(client);
    }
};

// A one-shot gate a callback blocks on until the test lets it go.
struct Gate {
    std::mutex m;
    std::condition_variable cv;
    bool open = false;
    void wait() {
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return open; });
    }
    void release() {
        {
            std::lock_guard lock(m);
            open = true;
        }
        cv.notify_all();
    }
};

} // namespace

TEST_CASE("CoreMIDI input close() waits for a callback that is in flight",
          "[midi][coremidi][close]") {
    DrainSource vs;
    if (!vs.create())
        SKIP("CoreMIDI server not reachable; skipping");
    auto system = pulp::midi::create_midi_system();
    REQUIRE(system != nullptr);

    Gate gate;
    std::atomic<bool> in_callback{false};
    std::atomic<bool> callback_returned{false};
    std::atomic<bool> close_returned{false};
    // What the callback touches after it is unblocked; freed right after
    // close(), exactly as an owner would. A callback that outlives close()
    // reads freed memory (a hard failure under AddressSanitizer).
    auto payload = std::make_unique<std::atomic<int>>(0);
    auto* raw = payload.get();

    auto input = system->create_input();
    REQUIRE(input->open(std::to_string(kDrainId), [&, raw](const pulp::midi::MidiEvent&) {
        in_callback = true;
        gate.wait();
        raw->fetch_add(1);
        callback_returned = true;
    }));
    send_note(vs.source);
    for (int i = 0; i < 300 && !in_callback; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(in_callback);

    auto closer = std::async(std::launch::async, [&] {
        input->close();
        close_returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    // The callback is still blocked, so close() must still be waiting.
    CHECK_FALSE(close_returned.load());

    gate.release();
    REQUIRE(closer.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(callback_returned.load());
    payload.reset();
}

TEST_CASE("CoreMIDI input delivers nothing after close() returns", "[midi][coremidi][close]") {
    DrainSource vs;
    if (!vs.create())
        SKIP("CoreMIDI server not reachable; skipping");
    auto system = pulp::midi::create_midi_system();
    REQUIRE(system != nullptr);

    std::atomic<int> received{0};
    auto input = system->create_input();
    REQUIRE(
        input->open(std::to_string(kDrainId), [&](const pulp::midi::MidiEvent&) { ++received; }));
    for (int i = 0; i < 20; ++i)
        send_note(vs.source);
    input->close();
    const int at_close = received.load();
    for (int i = 0; i < 20; ++i)
        send_note(vs.source);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(received.load() == at_close);
}

TEST_CASE("CoreMIDI input close() from inside its own callback does not deadlock",
          "[midi][coremidi][close]") {
    DrainSource vs;
    if (!vs.create())
        SKIP("CoreMIDI server not reachable; skipping");
    auto system = pulp::midi::create_midi_system();
    REQUIRE(system != nullptr);

    auto input = system->create_input();
    std::atomic<int> received{0};
    auto* raw = input.get();
    REQUIRE(input->open(std::to_string(kDrainId), [&, raw](const pulp::midi::MidiEvent&) {
        ++received;
        raw->close();
    }));

    auto done = std::async(std::launch::async, [&] {
        send_note(vs.source);
        for (int i = 0; i < 300 && received.load() < 1; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });
    REQUIRE(done.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    CHECK(received.load() == 1);
    // A second close from outside is still safe.
    auto outside = std::async(std::launch::async, [&] { input->close(); });
    REQUIRE(outside.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK_FALSE(input->is_open());
}
