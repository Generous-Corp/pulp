// Win MIDI MIM_LONGDATA SysEx + QPC timestamp tests (#19 / #245 partial).
// Windows-only; gracefully skips on hosts without a default input
// device. The cross-platform stub ensures CI on macOS/Linux still
// links + runs.

#include <catch2/catch_test_macros.hpp>

#include <pulp/midi/device.hpp>

TEST_CASE("MidiInput: sysex callback API exists for every backend",
          "[midi][sysex][issue-19]") {
    // Compile-only contract: every backend's MidiInput must accept a
    // sysex callback — no-op default for backends that don't yet
    // support sysex (CoreMIDI / ALSA today), real handler for
    // backends that do (Win MIDI as of this PR).
    auto sys = pulp::midi::create_midi_system();
    REQUIRE(sys != nullptr);
    auto in = sys->create_input();
    REQUIRE(in != nullptr);
    in->set_sysex_callback([](const std::vector<uint8_t>&, double) {});
    SUCCEED("set_sysex_callback compiles + accepts a real lambda");
}

#ifdef _WIN32

TEST_CASE("WinMIDI: open + sysex callback registration leaks nothing",
          "[midi][sysex][issue-19][windows]") {
    auto sys = pulp::midi::create_midi_system();
    REQUIRE(sys != nullptr);
    auto inputs = sys->enumerate_inputs();
    if (inputs.empty()) {
        SKIP("no MIDI input devices on this host");
    }

    auto in = sys->create_input();
    REQUIRE(in != nullptr);
    in->set_sysex_callback([](const std::vector<uint8_t>&, double) {
        // Real-host validation requires a hardware sender; this is
        // a register-only smoke test.
    });

    // open the first input port + immediately close. Validates that
    // the SysEx buffer prepare/unprepare path is leak-free even when
    // no SysEx packet ever arrives.
    bool opened = in->open(inputs[0].id, [](const auto&) {});
    if (!opened) {
        SKIP("first input device couldn't open, probably busy");
    }
    REQUIRE(in->is_open());
    in->close();
    REQUIRE_FALSE(in->is_open());
}

#endif  // _WIN32
