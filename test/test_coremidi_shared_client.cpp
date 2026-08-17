// The Apple CoreMIDI backend registers ONE client with the system MIDIServer
// for the whole process, not one per open port. A client per port multiplies
// MIDIServer registrations by the number of ports, and again by the number of
// loaded plug-in instances.
//
// These assertions run against the real CoreMIDI framework because the
// invariant being pinned is a property of that registration, not of a mock.
// They need no MIDI hardware and open no ports.

#include "../core/midi/platform/mac/coremidi_shared_client.h"
#include "../core/midi/src/ump_session_backend.hpp"

#include <pulp/midi/device.hpp>

#include <algorithm>
#include <string>
#include <vector>

#if defined(PULP_COREMIDI_SHARED_CLIENT_COMPILE_ONLY)

// The iOS SDK gate builds this TU with tests disabled. Keep the production
// declarations and their identity/lifetime use in that graph without pulling
// Catch2 into the device build.
namespace {
[[maybe_unused]] void compile_shared_client_contract_for_ios() {
    const MIDIClientRef first = pulp::midi::mac::shared_client();
    const MIDIClientRef second = pulp::midi::mac::shared_client();
    auto system = pulp::midi::create_midi_system();
    (void)first;
    (void)second;
    (void)system;
}
}  // namespace

#else

#include <catch2/catch_test_macros.hpp>

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

// ---------------------------------------------------------------------------
// Topology reducer.
//
// group_coremidi_ump_topology() is pure (vector in, vector out) and has no
// Apple dependency, but until now its only coverage was the iOS Simulator
// harness -- which never runs on the lane that gates PRs. That left the
// conservative-pairing rule, the one piece of judgement in the CoreMIDI
// backend, asserted nowhere a reviewer could see it fail.
//
// These cases live in this existing file deliberately: a new test file would be
// a fourteenth durable path outside P2-21's claimed envelope.
// ---------------------------------------------------------------------------

namespace {

pulp::midi::CoreMidiUmpTopologyEndpoint ep(std::string endpoint_id, std::string entity_id,
                                           std::string name, bool is_source) {
    pulp::midi::CoreMidiUmpTopologyEndpoint out;
    out.endpoint_id = std::move(endpoint_id);
    out.entity_id = std::move(entity_id);
    out.name = std::move(name);
    out.is_source = is_source;
    return out;
}

} // namespace

TEST_CASE("topology reducer pairs exactly one source with one destination per entity") {
    const auto grouped = pulp::midi::group_coremidi_ump_topology(
        {ep("src-1", "entity-a", "Keystation", true),
         ep("dst-1", "entity-a", "Keystation", false)});

    REQUIRE(grouped.size() == 1);
    // A paired entity is addressed by entity, not by either raw endpoint id.
    REQUIRE(grouped[0].id == "entity:entity-a");
    REQUIRE(grouped[0].name == "Keystation");
    REQUIRE(grouped[0].direction.can_receive);
    REQUIRE(grouped[0].direction.can_send);
}

TEST_CASE("topology reducer refuses to pair a multi-endpoint entity") {
    // Two sources and one destination is ambiguous. Pairing by position would
    // silently bind the wrong pair and drop an endpoint; the contract is to
    // enumerate all three as independent directions instead.
    const auto grouped = pulp::midi::group_coremidi_ump_topology(
        {ep("src-1", "entity-a", "Port 1", true), ep("src-2", "entity-a", "Port 2", true),
         ep("dst-1", "entity-a", "Port 1", false)});

    REQUIRE(grouped.size() == 3);
    for (const auto& info : grouped)
        REQUIRE(info.id.rfind("entity:", 0) != 0);
    const auto sources = std::count_if(grouped.begin(), grouped.end(), [](const auto& i) {
        return i.direction.can_receive && !i.direction.can_send;
    });
    REQUIRE(sources == 2);
}

TEST_CASE("topology reducer leaves entity-less endpoints independent") {
    const auto grouped = pulp::midi::group_coremidi_ump_topology(
        {ep("src-1", "", "Virtual In", true), ep("dst-1", "", "Virtual Out", false)});

    REQUIRE(grouped.size() == 2);
    REQUIRE(grouped[0].id == "src-1");
    REQUIRE(grouped[0].direction.can_receive);
    REQUIRE_FALSE(grouped[0].direction.can_send);
    REQUIRE(grouped[1].id == "dst-1");
    REQUIRE_FALSE(grouped[1].direction.can_receive);
    REQUIRE(grouped[1].direction.can_send);
}

TEST_CASE("topology reducer keeps distinct entities distinct and is empty-safe") {
    REQUIRE(pulp::midi::group_coremidi_ump_topology({}).empty());

    const auto grouped = pulp::midi::group_coremidi_ump_topology(
        {ep("src-a", "entity-a", "A", true), ep("dst-a", "entity-a", "A", false),
         ep("src-b", "entity-b", "B", true), ep("dst-b", "entity-b", "B", false)});
    REQUIRE(grouped.size() == 2);
    std::vector<std::string> ids{grouped[0].id, grouped[1].id};
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids[0] == "entity:entity-a");
    REQUIRE(ids[1] == "entity:entity-b");
}

TEST_CASE("topology reducer falls back to the destination name when the source is unnamed") {
    const auto grouped = pulp::midi::group_coremidi_ump_topology(
        {ep("src-1", "entity-a", "", true), ep("dst-1", "entity-a", "Fallback", false)});
    REQUIRE(grouped.size() == 1);
    REQUIRE(grouped[0].name == "Fallback");
}

#endif
