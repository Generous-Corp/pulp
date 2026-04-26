// Raw-MIDI byte-stream parser — pulls short messages, sysex, and
// realtime bytes out of a best-effort byte stream.
//
// Extracted into a pure helper so ALSA raw-midi, WinMM MIM_LONGDATA,
// and any future raw-byte MIDI transport can share the same
// accumulator logic and — more importantly — share a test suite.
//
// Not a public API; consumed from:
//   core/midi/platform/linux/alsa_midi_device.cpp
//   test/test_raw_midi_parser.cpp
//
// Design notes
// ────────────
// Sysex (F0..F7) is variable-length and can span multiple read() calls,
// so the accumulator state (sysex_buffer / sysex_in_progress) lives on
// the caller's RawMidiParserState and survives between invocations.
//
// MIDI 1.0 §3.1 allows real-time bytes (F8..FF) inside a sysex; they
// are emitted inline and do NOT terminate the accumulator.
//
// A non-realtime status byte (0x80..0xF7, excluding 0xF7 itself which
// terminates sysex) arriving mid-sysex means the device aborted the
// F0 stream without sending F7 — common on disconnect or firmware
// glitches. We drop the partial buffer and parse that status byte
// as the start of a new message, rather than silently consuming the
// next real message as sysex payload (#406 Codex P2).

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace pulp::midi {

// Typical device sysex dumps fit comfortably under 64 KB. If the
// accumulator collects more without seeing F7, assume the device is
// misbehaving and drop the buffer to cap memory use.
inline constexpr std::size_t kRawMidiSysexLimit = 64 * 1024;

struct RawMidiParserState {
    std::vector<uint8_t> sysex_buffer;
    bool sysex_in_progress = false;
};

// Short message = status byte + 0..2 data bytes (channel voice /
// system common, excluding realtime and sysex).
using RawMidiShortCallback =
    std::function<void(uint8_t status, uint8_t d1, uint8_t d2)>;

// Sysex callback fires once per complete F0..F7 run. The buffer passed
// in includes the leading F0 and trailing F7.
using RawMidiSysexCallback =
    std::function<void(const std::vector<uint8_t>& buf)>;

// Feed `n` bytes from `buf` into the parser. `state` holds cross-call
// sysex accumulator state and must be preserved between calls. `on_short`
// and `on_sysex` may be empty; callers that don't care about either
// stream pass a null std::function.
inline void parse_raw_midi_bytes(const uint8_t* buf,
                                 std::size_t n,
                                 RawMidiParserState& state,
                                 const RawMidiShortCallback& on_short,
                                 const RawMidiSysexCallback& on_sysex) {
    for (std::size_t i = 0; i < n; ++i) {
        const uint8_t b = buf[i];

        // Real-time (F8..FF) passes through unconditionally, even
        // inside a sysex run.
        if (b >= 0xF8) {
            if (on_short) on_short(b, 0, 0);
            continue;
        }

        if (state.sysex_in_progress) {
            if (b == 0xF7) {
                state.sysex_buffer.push_back(b);
                if (on_sysex) on_sysex(state.sysex_buffer);
                state.sysex_buffer.clear();
                state.sysex_in_progress = false;
                continue;
            }
            // A new non-realtime status byte means the F0 stream was
            // aborted. Drop the partial buffer and fall through so
            // this byte can be parsed as the start of a new message.
            if ((b & 0x80) != 0) {
                state.sysex_buffer.clear();
                state.sysex_in_progress = false;
                // fall through
            } else {
                state.sysex_buffer.push_back(b);
                if (state.sysex_buffer.size() > kRawMidiSysexLimit) {
                    state.sysex_buffer.clear();
                    state.sysex_in_progress = false;
                }
                continue;
            }
        }

        if (b == 0xF0) {
            state.sysex_buffer.clear();
            state.sysex_buffer.push_back(b);
            state.sysex_in_progress = true;
            continue;
        }

        if ((b & 0x80) == 0) continue; // stray data byte

        std::size_t msg_len = 3;
        if ((b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0) {
            msg_len = 2; // Program Change, Channel Pressure
        } else if (b == 0xF1 || b == 0xF3) {
            msg_len = 2; // MTC Quarter Frame, Song Select
        } else if (b == 0xF2) {
            msg_len = 3; // Song Position Pointer
        } else if (b == 0xF6 || b == 0xF7) {
            msg_len = 1; // Tune Request, stray End-of-Exclusive
        }

        if (i + msg_len <= n) {
            bool has_valid_data = true;
            for (std::size_t data_index = 1; data_index < msg_len; ++data_index) {
                if ((buf[i + data_index] & 0x80) != 0) {
                    has_valid_data = false;
                    break;
                }
            }

            if (!has_valid_data) continue;

            if (on_short) {
                on_short(b,
                         msg_len > 1 ? buf[i + 1] : 0,
                         msg_len > 2 ? buf[i + 2] : 0);
            }
            i += msg_len - 1; // outer ++i advances past the last byte
        }
    }
}

} // namespace pulp::midi
