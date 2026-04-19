#pragma once

// SysExAccumulator (#86) — shared MIDI SysEx aggregation state machine.
//
// MIDI SysEx messages (F0 ... F7) can span multiple transport packets:
// Android's MIDI API delivers raw bytes whenever they arrive, BLE MIDI
// fragments into ~20-byte chunks, USB-MIDI sends 4-byte event packets,
// etc. None of those transports guarantee a full sysex arrives in one
// delivery. This accumulator buffers until F7 or an abort, then emits
// the complete payload.
//
// State machine (per #406 + ALSA reference):
//
//   Idle     --[F0]-->   Buffering
//   Buffering --[data]->  Buffering (append)
//   Buffering --[F7]-->   emit complete, back to Idle
//   Buffering --[status != F7, != realtime]--> emit aborted, retry with status
//   Any       --[realtime (F8-FF except F7)]--> pass-through (realtime bytes
//                         can interrupt sysex without terminating it per
//                         the MIDI 1.0 spec — they're delivered outside
//                         the sysex stream).
//
// The accumulator is not thread-safe; wrap it per-port. Android's MIDI
// receiver is single-threaded per device, so the per-port model fits.
//
// Unit tests live in test/test_sysex_accumulator.cpp — see that file
// for coverage of the edge cases referenced in the state machine above.

#include <cstdint>
#include <functional>
#include <vector>

namespace pulp::midi {

/// Callback shape: receives the complete sysex payload (including the
/// leading 0xF0 and trailing 0xF7) plus a flag indicating whether the
/// message was properly terminated (`true`) or aborted by a new status
/// byte (`false`). Callers typically ignore aborted messages or log
/// them at debug level.
using SysexEmitFn =
    std::function<void(const std::vector<std::uint8_t>& payload, bool complete)>;

class SysexAccumulator {
public:
    /// Feed a single byte through the state machine. Invokes `emit`
    /// when a complete or aborted SysEx is assembled. Realtime bytes
    /// (0xF8-0xFE) pass through without affecting state — they should
    /// be handled as independent short messages by the caller.
    ///
    /// Returns the classification of the byte just consumed:
    ///   - in_sysex: byte was buffered, no emit.
    ///   - completed: SysEx just closed on F7; `emit` fired with complete=true.
    ///   - aborted: SysEx interrupted by new non-F7 status; `emit` fired
    ///              with complete=false and the aborter byte is NOT yet
    ///              consumed — feed it again as a fresh status to let
    ///              the caller route it normally.
    ///   - passthrough: byte is outside a SysEx (or was a realtime byte)
    ///                  and should be handled by the caller's regular
    ///                  short-message path.
    enum class Classification {
        in_sysex,
        completed,
        aborted,
        passthrough,
    };

    Classification feed(std::uint8_t byte, const SysexEmitFn& emit) {
        // Realtime messages F8-FE are delivered asynchronously and
        // don't participate in sysex framing. Pass them through.
        if (is_realtime(byte)) return Classification::passthrough;

        if (!in_sysex_) {
            if (byte == 0xF0) {
                in_sysex_ = true;
                buffer_.clear();
                buffer_.push_back(0xF0);
                return Classification::in_sysex;
            }
            // Regular short-message byte — caller handles it.
            return Classification::passthrough;
        }

        // In-sysex branch.
        if (byte == 0xF7) {
            buffer_.push_back(0xF7);
            emit(buffer_, /*complete=*/true);
            in_sysex_ = false;
            buffer_.clear();
            return Classification::completed;
        }

        if (is_status(byte)) {
            // New status interrupted an unterminated F0. Flush the
            // partial payload (without F7) and return without consuming
            // the aborter — caller re-feeds it as a fresh status.
            emit(buffer_, /*complete=*/false);
            in_sysex_ = false;
            buffer_.clear();
            return Classification::aborted;
        }

        // Normal data byte inside an active sysex.
        buffer_.push_back(byte);
        return Classification::in_sysex;
    }

    /// Feed a run of bytes — convenience over a delivery boundary.
    /// Handles the aborted-byte reclassification automatically.
    void feed(const std::uint8_t* data, std::size_t count,
              const SysexEmitFn& emit) {
        for (std::size_t i = 0; i < count; ++i) {
            auto cls = feed(data[i], emit);
            if (cls == Classification::aborted) {
                // Re-feed the aborter byte as a fresh status.
                feed(data[i], emit);
            }
        }
    }

    /// True when buffer_ holds an open F0 without a matching F7.
    bool in_progress() const noexcept { return in_sysex_; }

    /// Clear any accumulated partial state. Intended for port-close
    /// or shutdown paths — drops any partially-received message.
    void reset() noexcept {
        in_sysex_ = false;
        buffer_.clear();
    }

    /// Size of the in-progress payload (0 when idle). Includes the
    /// leading 0xF0 byte.
    std::size_t partial_size() const noexcept { return buffer_.size(); }

private:
    static bool is_status(std::uint8_t b) noexcept { return (b & 0x80) != 0; }
    static bool is_realtime(std::uint8_t b) noexcept {
        // MIDI 1.0 System Realtime Messages: 0xF8 (Timing Clock),
        // 0xFA-0xFC (Start/Continue/Stop), 0xFE (Active Sensing),
        // 0xFF (System Reset). 0xF9 and 0xFD are undefined. 0xF7 is
        // EOX and intentionally excluded (handled by feed() explicitly).
        return b >= 0xF8 && b <= 0xFF && b != 0xF9 && b != 0xFD;
    }

    bool in_sysex_ = false;
    std::vector<std::uint8_t> buffer_;
};

} // namespace pulp::midi
