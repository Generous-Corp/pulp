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

// Callback for received MIDI messages
using MidiInputCallback = std::function<void(const MidiEvent& event)>;

// MIDI input port
class MidiInput {
public:
    virtual ~MidiInput() = default;
    virtual bool open(const std::string& port_id, MidiInputCallback callback) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;
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
};

std::unique_ptr<MidiSystem> create_midi_system();

} // namespace pulp::midi
