#pragma once

#include <pulp/midi/message.hpp>
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace pulp::midi {

struct MidiPortInfo {
    std::string id;
    std::string name;
    bool is_input = false;
    bool is_output = false;
};

// Callback for received MIDI short (channel-voice / system-realtime)
// messages. SysEx is delivered via MidiSysexCallback registered
// separately so the open() signature stays bit-compat.
using MidiInputCallback = std::function<void(const MidiEvent& event)>;
using MidiSysexCallback = std::function<void(const std::vector<uint8_t>& bytes,
                                             double timestamp_sec)>;

// MIDI input port
class MidiInput {
public:
    virtual ~MidiInput() = default;
    virtual bool open(const std::string& port_id, MidiInputCallback callback) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    /// Optional SysEx delivery. Backends without SysEx support leave
    /// this as a no-op; callers can still register safely. Backends
    /// that DO support it (Win mmeapi MIM_LONGDATA, future CoreMIDI
    /// reassembly, ALSA SND_SEQ_EVENT_SYSEX) call this BEFORE open()
    /// to register the receiver.
    virtual void set_sysex_callback(MidiSysexCallback /*cb*/) {}
};

// MIDI output port
class MidiOutput {
public:
    virtual ~MidiOutput() = default;
    virtual bool open(const std::string& port_id) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;
    virtual void send(const MidiEvent& event) = 0;
};

// MIDI system — enumerates ports and creates I/O instances
class MidiSystem {
public:
    virtual ~MidiSystem() = default;
    virtual std::vector<MidiPortInfo> enumerate_inputs() = 0;
    virtual std::vector<MidiPortInfo> enumerate_outputs() = 0;
    virtual std::unique_ptr<MidiInput> create_input() = 0;
    virtual std::unique_ptr<MidiOutput> create_output() = 0;

    /// Register a callback for MIDI port changes (device plug/unplug).
    /// The callback may fire on an OS thread. Pass nullptr to unregister.
    using PortChangeCallback = std::function<void()>;
    virtual void set_port_change_callback(PortChangeCallback) {}
};

std::unique_ptr<MidiSystem> create_midi_system();

} // namespace pulp::midi
