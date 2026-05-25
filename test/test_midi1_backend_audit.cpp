#include <catch2/catch_test_macros.hpp>
#include <pulp/midi/device.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::midi;

// ────────────────────────────────────────────────────────────────────────
// macOS plan item 8.6 — MIDI 1.0 backend audit on macOS.
//
// These tests don't open real OS MIDI ports (CI has none) — they
// confirm that the MidiSystem / MidiInput / MidiOutput contract is
// wired through to a real backend implementation and that the
// surface area survives the enumerate / create flow without crashing.
//
// On macOS the backend is CoreMIDI (core/midi/platform/mac/coremidi_device.mm).
// On non-macOS hosts in CI we still get a backend (Linux ALSA, Windows
// mmeapi, or a stub) — the tests stay portable by asserting only on
// the cross-platform contract.
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("MIDI 1.0 backend: create_midi_system returns a non-null instance",
          "[midi][backend][audit]") {
    auto system = create_midi_system();
    REQUIRE(system != nullptr);
}

TEST_CASE("MIDI 1.0 backend: enumerate_inputs/outputs returns a sane list",
          "[midi][backend][audit]") {
    auto system = create_midi_system();
    REQUIRE(system != nullptr);

    // No real ports are required in CI — the list is allowed to be
    // empty. What matters is that the call doesn't crash and that
    // every returned port carries a non-empty id + name.
    auto inputs = system->enumerate_inputs();
    for (const auto& port : inputs) {
        REQUIRE_FALSE(port.id.empty());
        REQUIRE_FALSE(port.name.empty());
        REQUIRE(port.is_input);
        REQUIRE_FALSE(port.is_output);
    }

    auto outputs = system->enumerate_outputs();
    for (const auto& port : outputs) {
        REQUIRE_FALSE(port.id.empty());
        REQUIRE_FALSE(port.name.empty());
        REQUIRE(port.is_output);
        REQUIRE_FALSE(port.is_input);
    }
}

TEST_CASE("MIDI 1.0 backend: create_input + create_output return objects",
          "[midi][backend][audit]") {
    auto system = create_midi_system();
    REQUIRE(system != nullptr);

    auto input = system->create_input();
    REQUIRE(input != nullptr);
    REQUIRE(input->is_open() == false);

    auto output = system->create_output();
    REQUIRE(output != nullptr);
    REQUIRE(output->is_open() == false);
}

TEST_CASE("MIDI 1.0 backend: open() with bogus port id fails gracefully",
          "[midi][backend][audit]") {
    auto system = create_midi_system();
    REQUIRE(system != nullptr);

    auto input = system->create_input();
    REQUIRE(input != nullptr);
    // No callback is required to fail-open, but registering one
    // exercises the std::function move path in the backend.
    const bool ok = input->open("99999999999999",
                                  [](const MidiEvent&) {});
    REQUIRE(ok == false);
    REQUIRE(input->is_open() == false);

    auto output = system->create_output();
    REQUIRE(output != nullptr);
    const bool ok2 = output->open("99999999999999");
    REQUIRE(ok2 == false);
    REQUIRE(output->is_open() == false);
}

TEST_CASE("MIDI 1.0 backend: set_sysex_callback registers without open()",
          "[midi][backend][audit]") {
    auto system = create_midi_system();
    REQUIRE(system != nullptr);

    auto input = system->create_input();
    REQUIRE(input != nullptr);

    std::atomic<int> sysex_calls{0};
    // The cross-platform contract is "callable before open() without
    // crashing" — backends without SysEx support no-op the call, but
    // every backend must accept the registration.
    input->set_sysex_callback(
        [&](const std::vector<uint8_t>& /*bytes*/, double /*ts*/) {
            sysex_calls.fetch_add(1, std::memory_order_relaxed);
        });
    // Closing without ever opening should also be a no-op.
    input->close();
    REQUIRE(sysex_calls.load() == 0); // never invoked without real input
}

TEST_CASE("MIDI 1.0 backend: set_port_change_callback registers + accepts nullptr",
          "[midi][backend][audit]") {
    auto system = create_midi_system();
    REQUIRE(system != nullptr);

    std::atomic<int> change_calls{0};
    system->set_port_change_callback(
        [&] { change_calls.fetch_add(1, std::memory_order_relaxed); });
    // Unregister — nullptr must be accepted.
    system->set_port_change_callback(nullptr);
    REQUIRE(change_calls.load() == 0); // no plug events occurred during the test
}

TEST_CASE("MIDI 1.0 backend: enumeration is stable across repeated calls",
          "[midi][backend][audit]") {
    auto system = create_midi_system();
    REQUIRE(system != nullptr);

    const auto a = system->enumerate_inputs();
    const auto b = system->enumerate_inputs();
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        REQUIRE(a[i].id == b[i].id);
        REQUIRE(a[i].name == b[i].name);
    }
}
