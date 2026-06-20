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
// next real message as SysEx payload.
//
// RT-safety contract:
//
//   - Call RawMidiParserState::reserve_sysex(N) before entering the MIDI
//     callback, where N is the largest F0...F7 payload the transport accepts.
//   - After that preparation, parse_raw_midi_bytes() does not allocate while
//     SysEx payloads stay within the reserved size.
//   - Callback objects and callback-owned destination storage are caller-owned.
//     Keep them preconstructed and allocation-free when using this helper from
//     realtime code.

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
    /// Prepare internal SysEx storage for realtime use. The reserve includes
    /// the leading F0 and trailing F7 bytes.
    void reserve_sysex(std::size_t worst_case_bytes) {
        sysex_buffer.reserve(worst_case_bytes);
    }

    std::vector<uint8_t> sysex_buffer;
    bool sysex_in_progress = false;
    uint8_t running_status = 0;
    uint8_t pending_status = 0;
    uint8_t pending_data[2] = {};
    std::size_t pending_count = 0;
    std::size_t pending_expected = 0;
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
    auto clear_pending = [&state] {
        state.pending_status = 0;
        state.pending_data[0] = 0;
        state.pending_data[1] = 0;
        state.pending_count = 0;
        state.pending_expected = 0;
    };

    auto short_data_len = [](uint8_t status) -> std::size_t {
        if ((status & 0xF0) >= 0x80 && (status & 0xF0) <= 0xE0) {
            return ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) ? 1u : 2u;
        }
        if (status == 0xF1 || status == 0xF3) return 1;
        if (status == 0xF2) return 2;
        if (status == 0xF6 || status == 0xF7) return 0;
        return static_cast<std::size_t>(-1);
    };

    auto is_channel_status = [](uint8_t status) {
        return (status & 0xF0) >= 0x80 && (status & 0xF0) <= 0xE0;
    };

    auto emit_pending = [&] {
        if (on_short) {
            on_short(state.pending_status,
                     state.pending_expected > 0 ? state.pending_data[0] : 0,
                     state.pending_expected > 1 ? state.pending_data[1] : 0);
        }
        const uint8_t completed_status = state.pending_status;
        clear_pending();
        if (is_channel_status(completed_status)) state.running_status = completed_status;
    };

    auto start_pending = [&](uint8_t status) {
        clear_pending();
        const auto data_len = short_data_len(status);
        if (data_len == static_cast<std::size_t>(-1)) {
            state.running_status = 0;
            return;
        }
        if (is_channel_status(status)) {
            state.running_status = status;
        } else {
            state.running_status = 0;
        }
        if (data_len == 0) {
            if (on_short) on_short(status, 0, 0);
            return;
        }
        state.pending_status = status;
        state.pending_expected = data_len;
    };

    auto consume_data = [&](uint8_t data) {
        if (state.pending_status == 0) {
            if (state.running_status == 0) return;
            start_pending(state.running_status);
        }
        if (state.pending_status == 0 || state.pending_count >= 2) return;
        state.pending_data[state.pending_count++] = data;
        if (state.pending_count >= state.pending_expected) emit_pending();
    };

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
            state.running_status = 0;
            clear_pending();
            continue;
        }

        if ((b & 0x80) != 0) {
            start_pending(b);
            continue;
        }

        consume_data(b);
    }
}

} // namespace pulp::midi
