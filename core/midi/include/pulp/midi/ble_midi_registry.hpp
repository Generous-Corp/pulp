#pragma once

// Process-wide registry that bridges a connected BLE-MIDI peripheral into the
// platform MidiSystem's port list.
//
// Why this exists: on Apple, CoreMIDI itself exposes an OS-paired BLE
// peripheral as a regular MIDI endpoint, so MidiSystem::enumerate_inputs()
// shows it for free. Off Apple there is no such OS bridge — ALSA-seq does NOT
// auto-create a port for a GATT notify stream. The BlueZ central therefore has
// to publish the connection somewhere the platform MidiSystem can see it, and
// the MidiSystem has to merge those published entries into its enumeration and
// route open() on a BLE port id to the registry instead of the raw transport.
//
// This registry is the smallest honest seam for that: the central
// register_input()/register_output() on connect (and unregister on
// disconnect); the MidiSystem queries list_inputs()/list_outputs() to merge,
// and attach_input_sink()/take_output_source() to wire delivery once a port is
// opened. The registry owns NO transport — it only forwards bytes between the
// central and the MidiInput/MidiOutput the host opened.
//
// All ids use the "ble-midi-in:<peripheral-id>" / "ble-midi-out:<peripheral-id>"
// convention also produced by the Apple backend, so a host can pre-select a BLE
// port via BleMidiCentral::midi_input_port_for() before it appears in
// enumerate_inputs().
//
// Cross-platform + Skia-free: compiled on every platform so the central and the
// MidiSystem link against one definition. On platforms whose central never
// registers anything (the stub central) the registry simply stays empty and the
// merge is a no-op.

#include <pulp/midi/device.hpp>
#include <pulp/midi/message.hpp>

#include <functional>
#include <string>
#include <vector>

namespace pulp::midi {

/// Singleton bridge between BLE-MIDI centrals and the platform MidiSystem.
/// Thread-safe: the central touches it from its D-Bus worker thread while the
/// MidiSystem / host touch it from the UI / audio-setup thread.
class BleMidiPortRegistry {
public:
    static BleMidiPortRegistry& instance();

    // ── Central side ────────────────────────────────────────────────────────

    /// Publish a connected peripheral's input port. `port_id` is
    /// "ble-midi-in:<peripheral-id>"; `name` is the human-readable peripheral
    /// name shown in port pickers. Idempotent per port_id.
    void register_input(const std::string& port_id, const std::string& name);
    /// Publish a connected peripheral's output port
    /// ("ble-midi-out:<peripheral-id>"). `sink` is invoked with raw MIDI bytes
    /// when the host sends to the opened MidiOutput; the central encodes them
    /// into a BLE packet and performs the GATT write. Idempotent per port_id.
    void register_output(const std::string& port_id, const std::string& name,
                         std::function<void(const std::vector<uint8_t>&)> sink);

    /// Remove a previously published port (on disconnect). Clears any attached
    /// host callback so a stale GATT notification cannot resurrect it.
    void unregister_input(const std::string& port_id);
    void unregister_output(const std::string& port_id);

    /// Deliver a decoded MIDI message from the central to the host callback
    /// attached to `port_id` (if the host has opened that input). No-op when the
    /// port is unknown or unopened. Called from the central's worker thread.
    void deliver_message(const std::string& port_id,
                         const std::vector<uint8_t>& bytes, double timestamp_sec);

    // ── MidiSystem side ─────────────────────────────────────────────────────

    /// Currently-published BLE input / output ports, for the platform
    /// MidiSystem to merge into enumerate_inputs() / enumerate_outputs().
    std::vector<MidiPortInfo> list_inputs() const;
    std::vector<MidiPortInfo> list_outputs() const;

    /// True if `port_id` is a registered BLE input / output (so the platform
    /// MidiInput/MidiOutput knows to route through the registry rather than the
    /// raw OS transport).
    bool is_input(const std::string& port_id) const;
    bool is_output(const std::string& port_id) const;

    /// Attach the host's MidiInput delivery callback to a BLE input port. The
    /// registry forwards every deliver_message() for that port to it until the
    /// input is closed (detach_input) or the port is unregistered. Returns false
    /// if `port_id` is not a registered BLE input.
    bool attach_input(const std::string& port_id, MidiInputCallback callback,
                      MidiSysexCallback sysex_callback);
    /// Detach the host's MidiInput delivery callback (on MidiInput::close()).
    void detach_input(const std::string& port_id);

    /// Fetch a forwarding handle for a BLE output port so the host's MidiOutput
    /// can forward sent events. Returns an empty function if the port is unknown;
    /// a previously fetched handle becomes a no-op after unregister_output().
    std::function<void(const std::vector<uint8_t>&)> output_sink(
        const std::string& port_id) const;

private:
    BleMidiPortRegistry() = default;

    struct Impl;
    // Pimpl keeps <mutex>/<map> out of this widely-included header.
    Impl& impl() const;
};

}  // namespace pulp::midi
