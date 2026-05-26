#pragma once

// UmpSysex7Reassembler — shared reassembler for UMP Type-0x3 (sysex7)
// packet streams.
//
// Background
// ==========
//
// MIDI 2.0 UMP delivers sysex via "Data Messages — sysex7" (UMP message
// type 0x3), which is a 64-bit / 2-word packet carrying up to 6 bytes
// of sysex payload at a time. A logical sysex (F0 .. F7) longer than
// 6 bytes is split across multiple type-0x3 packets using a 4-bit
// status nibble (bits 20-23 of word 0):
//
//     0x0  complete   — single-packet sysex (size <= 6 bytes)
//     0x1  start      — first packet, more to follow
//     0x2  continue   — middle packet
//     0x3  end        — final packet
//
// The 4-bit "size" field (bits 16-19 of word 0) is the number of valid
// payload bytes in this packet (0..6). The payload bytes themselves are
// packed into bits 0..15 of word 0 and bits 0..31 of word 1, in that
// order:
//
//     byte 0 = (word0 >>  8) & 0xFF
//     byte 1 = (word0 >>  0) & 0xFF
//     byte 2 = (word1 >> 24) & 0xFF
//     byte 3 = (word1 >> 16) & 0xFF
//     byte 4 = (word1 >>  8) & 0xFF
//     byte 5 = (word1 >>  0) & 0xFF
//
// Reference: MIDI 2.0 UMP specification §4.7 "Data Messages — System
// Exclusive 7-bit (sysex7)".
//
// Why a shared reassembler
// ========================
//
// Pulp historically open-coded the same state machine inline in both
// `core/format/src/au_adapter.mm` (AUv3 MIDIEventList path) and
// `core/midi/platform/mac/coremidi_device.mm` (CoreMIDI 2.0 input
// callback). Both implementations carry the same regression notes
// (#239 / #292) and the same edge-case handling for orphaned
// continue/end packets. Extracting the state machine here:
//
//   1. Eliminates duplicated state and bit-twiddling.
//   2. Centralises the regression test for the #292 P1 bug ("second
//      word's top nibble can masquerade as a new message header") and
//      the #292 P2 boundary-preservation property.
//   3. Gives future UMP-aware backends (WinRT MIDI 2.0, ALSA UMP,
//      iOS CoreMIDI 2.0) a drop-in dependency.
//
// Thread-safety
// =============
//
// The reassembler holds per-stream state (`payload_` + `in_progress_`).
// It is **not** thread-safe — each input port / source should own its
// own instance. The CoreMIDI input callback is single-threaded per
// port, and AUv3 render blocks are single-threaded per audio bus, so
// the per-stream model fits both call sites.
//
// RT-safety
// =========
//
// `feed_packet()` may allocate inside the internal `std::vector` as
// the payload grows past its current capacity. Callers that want
// strict RT-safety should `reserve()` the worst-case sysex length up
// front via `reserve(N)`. AU adapter render-thread use is already
// allocating in this path today (pre-extraction), so this extraction
// does not regress that property.
//
// Unit tests live in `test/test_ump_sysex7_reassembler.cpp`.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace pulp::midi {

/// Callback shape: receives the complete sysex payload (the F0..F7
/// inner bytes — i.e. the payload bytes carried by the type-0x3
/// stream, with no synthesised F0/F7 framing). UMP sysex7 packets do
/// not carry F0/F7 framing bytes by design (the framing is implicit
/// in the start/continue/end status nibble), so the emitted payload
/// matches what every downstream Pulp MIDI sink expects from
/// `MidiBuffer::add_sysex()` / `MidiInput::sysex_callback_`.
///
/// The reassembler invokes this callback exactly once per logical
/// sysex (either a single complete packet, or a start → continue* →
/// end span). Orphaned continue/end packets (i.e. arrival without a
/// preceding start) are dropped silently — corrupting the downstream
/// sink with a partial payload is worse than dropping one malformed
/// sysex (#292 P2).
using Sysex7EmitFn = void(*)(const std::vector<std::uint8_t>& payload,
                             void* user);

/// Reassembler state machine for UMP Type-0x3 sysex7 packet streams.
///
/// Per-stream — see "Thread-safety" in the file header.
class UmpSysex7Reassembler {
public:
    /// Classification of the most recently consumed packet.
    enum class Status {
        /// Single-packet sysex (status nibble 0x0). The emit callback
        /// fired with the complete payload.
        single_packet,
        /// Start packet (status nibble 0x1). Accumulator is now armed;
        /// the emit callback has NOT fired yet.
        start,
        /// Continue packet (status nibble 0x2) appended to an armed
        /// accumulator. The emit callback has NOT fired yet.
        continued,
        /// End packet (status nibble 0x3) closed an armed accumulator.
        /// The emit callback fired with the complete payload.
        ended,
        /// Continue / end without a preceding start, OR a packet whose
        /// status nibble is neither 0x0/0x1/0x2/0x3. The accumulator is
        /// untouched; the callback did NOT fire.
        dropped,
    };

    UmpSysex7Reassembler() = default;

    /// Feed a single 2-word UMP packet through the state machine.
    ///
    /// Caller must ensure the packet is actually a Type-0x3 (sysex7)
    /// packet — i.e. `(word0 >> 28) & 0x0F == 0x3`. Passing a non-0x3
    /// packet is a programmer error; the reassembler does not validate
    /// the message type itself because callers already need that
    /// nibble for dispatch / cursor-advance and re-checking it here
    /// would be redundant work in the hot path.
    ///
    /// `emit` is invoked at most once per call, only when this packet
    /// completes a logical sysex (status 0x0 or 0x3). `user` is passed
    /// through verbatim — keep it lightweight to preserve RT-safety.
    Status feed_packet(std::uint32_t word0, std::uint32_t word1,
                       Sysex7EmitFn emit, void* user) noexcept {
        const std::uint8_t status =
            static_cast<std::uint8_t>((word0 >> 20) & 0x0F);
        const std::uint32_t size =
            static_cast<std::uint32_t>((word0 >> 16) & 0x0F);

        switch (status) {
        case 0x0: {
            // Complete single-packet sysex. Use a scratch vector so
            // the call doesn't disturb any partial in-progress state
            // (per #292 P2 — an interleaved single-packet sysex must
            // not eat a separate in-progress accumulator). This is
            // genuinely possible: the UMP spec permits independent
            // sysex streams on different groups to interleave through
            // a single endpoint.
            std::vector<std::uint8_t> scratch;
            extract_bytes(word0, word1, size, scratch);
            if (!scratch.empty() && emit) {
                emit(scratch, user);
            }
            return Status::single_packet;
        }
        case 0x1: {
            // Start — reset the accumulator and begin capture. If a
            // previous in-progress sysex was never closed, it is
            // silently dropped: the spec doesn't allow nested sysex,
            // and resetting is the only sane recovery.
            payload_.clear();
            extract_bytes(word0, word1, size, payload_);
            in_progress_ = true;
            return Status::start;
        }
        case 0x2:
            if (in_progress_) {
                extract_bytes(word0, word1, size, payload_);
                return Status::continued;
            }
            // Orphan continue — drop. See #292 P2.
            return Status::dropped;
        case 0x3:
            if (in_progress_) {
                extract_bytes(word0, word1, size, payload_);
                if (!payload_.empty() && emit) {
                    emit(payload_, user);
                }
                payload_.clear();
                in_progress_ = false;
                return Status::ended;
            }
            // Orphan end — drop. See #292 P2.
            return Status::dropped;
        default:
            // Reserved / unknown status nibble. Per the spec, type-0x3
            // status values 0x4..0xF are reserved; the only sane
            // behaviour for forward compatibility is to drop the
            // packet without disturbing accumulator state.
            return Status::dropped;
        }
    }

    /// True when an in-progress sysex is open (a 0x1 start packet has
    /// been seen and no 0x3 end packet has closed it yet).
    bool in_progress() const noexcept { return in_progress_; }

    /// Current accumulated payload size in bytes (0 when idle).
    std::size_t partial_size() const noexcept { return payload_.size(); }

    /// Reserve worst-case payload capacity to avoid allocations in the
    /// hot path. Safe to call at port-open / prepare() time.
    void reserve(std::size_t worst_case_bytes) {
        payload_.reserve(worst_case_bytes);
    }

    /// Drop any in-progress sysex without emitting. Use on port-close
    /// or when a transport-level reset is observed (e.g. CoreMIDI
    /// endpoint reconnect).
    void reset() noexcept {
        payload_.clear();
        in_progress_ = false;
    }

private:
    static void extract_bytes(std::uint32_t w0, std::uint32_t w1,
                              std::uint32_t size,
                              std::vector<std::uint8_t>& out) {
        // Per the UMP spec, payload bytes are packed:
        //   byte 0 = (w0 >>  8) & 0xFF
        //   byte 1 = (w0 >>  0) & 0xFF
        //   byte 2 = (w1 >> 24) & 0xFF
        //   byte 3 = (w1 >> 16) & 0xFF
        //   byte 4 = (w1 >>  8) & 0xFF
        //   byte 5 = (w1 >>  0) & 0xFF
        // `size` is the 4-bit field from bits 16-19 of w0 and is in
        // 0..6. We clamp at 6 defensively.
        const std::uint8_t buf[6] = {
            static_cast<std::uint8_t>((w0 >>  8) & 0xFF),
            static_cast<std::uint8_t>((w0 >>  0) & 0xFF),
            static_cast<std::uint8_t>((w1 >> 24) & 0xFF),
            static_cast<std::uint8_t>((w1 >> 16) & 0xFF),
            static_cast<std::uint8_t>((w1 >>  8) & 0xFF),
            static_cast<std::uint8_t>((w1 >>  0) & 0xFF),
        };
        const std::uint32_t n = (size > 6) ? 6 : size;
        for (std::uint32_t i = 0; i < n; ++i) out.push_back(buf[i]);
    }

    std::vector<std::uint8_t> payload_;
    bool in_progress_ = false;
};

/// Convenience: feed a packet and collect the emitted payload (if any)
/// into `out`. Returns the same `Status` as `feed_packet`. Useful for
/// hosts that want a value-returning API rather than a callback. Not
/// RT-safe (the lambda capture allocates) — for hot-path use, prefer
/// the function-pointer `feed_packet` overload.
inline UmpSysex7Reassembler::Status feed_collect(
    UmpSysex7Reassembler& r, std::uint32_t word0, std::uint32_t word1,
    std::vector<std::uint8_t>& out) {
    struct Box { std::vector<std::uint8_t>* sink; };
    Box box{&out};
    return r.feed_packet(word0, word1,
        [](const std::vector<std::uint8_t>& payload, void* user) {
            auto* b = static_cast<Box*>(user);
            *b->sink = payload;
        },
        &box);
}

} // namespace pulp::midi
