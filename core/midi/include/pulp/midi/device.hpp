#pragma once

#include <pulp/midi/message.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace pulp::midi {

/// One enumerated MIDI port.
///
/// Identity contract (CoreMIDI; other backends fill what they have):
///  - `id` is the key to pass back to `open()`. On CoreMIDI it is the
///    decimal `kMIDIPropertyUniqueID` of the endpoint. CoreMIDI documents
///    unique IDs as persistent for an endpoint, and says they can change
///    when a driver is reinstalled or a device is treated as new. Measured
///    here: two `MidiSystem` instances, and two processes, report the same
///    `id` for the same endpoint. Not measured here: reboot, replug and
///    moving a device to another USB port.
///  - `stable_key` is a second form built from names (`device|entity|endpoint`).
///    It can match after an `id` change but is ambiguous when two identical
///    devices are attached. `resolve_port()` uses it only when exactly one
///    port carries it. This fallback is a Pulp policy, not a documented
///    CoreMIDI guarantee and not a copy of any other host's behaviour.
///  - An endpoint an app creates in its own process has an `id` like any
///    other port and an empty `device_name`; its `id` lives as long as the
///    creating process keeps the endpoint, and may be reassigned if it
///    restarts.
///  - `open()` takes an `id`. An unknown or malformed `id` fails; it never
///    falls back to another port. The two exceptions are the empty string and
///    "0", which select the first port and are not stable identifiers.
struct MidiPortInfo {
    std::string id;
    std::string name;   // display name (device plus endpoint where the OS joins them)
    bool is_input = false;
    bool is_output = false;

    std::string device_name;    // owning device, empty for virtual endpoints
    std::string entity_name;    // owning entity within the device
    std::string endpoint_name;  // the endpoint's own name
    std::string stable_key;     // "device|entity|endpoint", empty when unknown
    bool online = true;         // false when the OS lists the port but its device is offline
};

/// Pick the port an app saved earlier. `id` wins when present and the port is
/// online. Otherwise `stable_key` is used, but only when exactly one port
/// carries it, so identical devices never get confused. Returns the index into
/// `ports`, or -1 when nothing matches unambiguously.
int resolve_port(const std::vector<MidiPortInfo>& ports,
                 const std::string& saved_id,
                 const std::string& saved_stable_key);

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

/// Largest System Exclusive message `MidiOutput::send_sysex` accepts, framing
/// bytes included. Real devices use a few kilobytes at most; the bound keeps a
/// bad caller from queueing an unbounded copy.
inline constexpr std::size_t kMaxSysexSendBytes = 1u << 20;

/// True when `bytes` is one complete System Exclusive message: it starts with
/// 0xF0, ends with 0xF7, and every byte between them is a data byte (< 0x80).
/// An embedded status byte would end the message early on the wire, so it is
/// rejected rather than sent. Pure, allocation-free, safe on any thread.
inline bool is_valid_sysex_message(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() < 2 || bytes.size() > kMaxSysexSendBytes)
        return false;
    if (bytes.front() != 0xF0 || bytes.back() != 0xF7)
        return false;
    for (std::size_t i = 1; i + 1 < bytes.size(); ++i)
        if (bytes[i] & 0x80)
            return false;
    return true;
}

// MIDI output port
class MidiOutput {
public:
    virtual ~MidiOutput() = default;
    virtual bool open(const std::string& port_id) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;
    virtual void send(const MidiEvent& event) = 0;

    /// Send one System Exclusive message (0xF0 ... 0xF7, see
    /// `is_valid_sysex_message`). Returns true when the backend accepted it
    /// for delivery; delivery itself is asynchronous and its completion is not
    /// reported. Returns false when the message is malformed or too large, the
    /// port is not open, or the backend has no SysEx output, which is the
    /// default so existing backends compile unchanged.
    ///
    /// CoreMIDI delivers a message at MIDI wire speed, about 3 KB/s even to an
    /// in-process destination (3000 bytes took 0.93 s when measured on macOS),
    /// so a large dump takes seconds to finish. At most 16 messages may be
    /// waiting at once; the next send returns false until one completes.
    ///
    /// Not real-time safe: it validates, copies the message and may take a
    /// lock. Call it from a UI, control or worker thread, never from an audio
    /// callback. The bytes are copied before this returns, so the caller may
    /// reuse its buffer at once.
    virtual bool send_sysex(std::span<const std::uint8_t> /*bytes*/) {
        return false;
    }
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
