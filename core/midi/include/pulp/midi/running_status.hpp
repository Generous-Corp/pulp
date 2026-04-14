#pragma once

// MIDI 1.0 running-status byte-stream parser (workstream 02 slice 2.5).
//
// Platform MIDI backends (ALSA raw MIDI, Win32 mmeapi MIM_LONGDATA, BLE
// MIDI) deliver raw bytes — status bytes may be omitted when they would
// repeat the previous channel-voice status ("running status"). The prior
// ALSA MIDI parser had a simple state machine that didn't track running
// status, so a note-on sequence like `90 3C 7F 3D 7F 3E 7F` would emit
// only the first note. This parser handles running status, real-time
// interleave, and system-common/sysex boundaries correctly.
//
// Pure C++ — consumed by platform backends + unit-tested without any
// MIDI device attached.

#include <pulp/midi/message.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace pulp::midi {

/// Callback invoked for each complete short message. SysEx payloads are
/// delivered via a separate callback so callers can grow their own buffer.
using ShortMessageSink = std::function<void(const MidiEvent&)>;
using SysexSink        = std::function<void(const uint8_t* data, std::size_t size)>;

class RunningStatusParser {
public:
    RunningStatusParser() = default;

    void on_short_message(ShortMessageSink sink) { short_sink_ = std::move(sink); }
    void on_sysex(SysexSink sink) { sysex_sink_ = std::move(sink); }

    /// Feed a byte stream. Call from the MIDI I/O thread; the parser
    /// itself is single-threaded (one instance per input port).
    void feed(const uint8_t* data, std::size_t size);

    /// Reset the parser — clears running status + any in-flight sysex.
    /// Platform backends invoke this on port open / reset events.
    void reset();

private:
    static int expected_data_bytes(uint8_t status);
    void emit_short(uint8_t status, uint8_t d1, uint8_t d2);

    uint8_t running_status_ = 0;        ///< last channel-voice status byte, 0 if none
    uint8_t current_system_common_ = 0; ///< one-shot F1/F2/F3 status
    uint8_t data_[2] = {0, 0};
    int data_count_ = 0;
    int data_expected_ = 0;             ///< matches current running_status_
    bool in_sysex_ = false;
    std::vector<uint8_t> sysex_buf_;

    ShortMessageSink short_sink_;
    SysexSink        sysex_sink_;
};

}  // namespace pulp::midi
