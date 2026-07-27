// The macOS CoreMIDI backend registers ONE client with the system MIDIServer
// for the whole process, not one per open port. A client per port multiplies
// MIDIServer registrations by the number of ports, and again by the number of
// loaded plug-in instances.
//
// These assertions run against the real CoreMIDI framework because the
// invariant being pinned is a property of that registration, not of a mock.
// They need no MIDI hardware and open no ports.

#include <catch2/catch_test_macros.hpp>

#include "../core/midi/platform/mac/coremidi_shared_client.h"

#include <pulp/midi/device.hpp>

TEST_CASE("CoreMIDI client is created once for the process",
          "[midi][coremidi]") {
    const MIDIClientRef first = pulp::midi::mac::shared_client();
    const MIDIClientRef second = pulp::midi::mac::shared_client();

    // A per-call client would hand back a different handle each time.
    REQUIRE(first == second);
}

TEST_CASE("CoreMIDI shared client survives opening and closing ports",
          "[midi][coremidi]") {
    const MIDIClientRef before = pulp::midi::mac::shared_client();

    // Ports are disposed on close(); the client they were created against is
    // not, because other ports in the process are still using it. Disposing it
    // on the last close would invalidate any port opened afterwards.
    {
        auto system = pulp::midi::create_midi_system();
        REQUIRE(system != nullptr);
        auto output = system->create_output();
        REQUIRE(output != nullptr);
        // open() may legitimately fail on a machine with no MIDI destinations
        // (headless CI); either way the client must be untouched by close().
        (void)output->open("0");
        output->close();
    }

    const MIDIClientRef after = pulp::midi::mac::shared_client();
    REQUIRE(after == before);
    REQUIRE(after != 0);
}
