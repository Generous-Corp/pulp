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
// Reference: MIDI Association Universal MIDI Packet (UMP) and MIDI 2.0
// Protocol Specification, System Exclusive 7-bit Data Messages.
// https://midi.org/universal-midi-packet-ump-and-midi-2-0-protocol-specification
//
// Why a shared reassembler
// ========================
//
// Pulp historically open-coded the same state machine inline in both
// `core/format/src/au_adapter.mm` (AUv3 MIDIEventList path) and
// `core/midi/platform/mac/coremidi_device.mm` (CoreMIDI 2.0 input
// callback). Both implementations needed the same cursor-advance guard
// and the same edge-case handling for orphaned continue/end packets.
// Extracting the state machine here:
//
//   1. Eliminates duplicated state and bit-twiddling.
//   2. Centralises test coverage for the second word's top nibble not
//      masquerading as a new message header, plus preservation of an
//      unrelated in-progress stream when a complete packet interleaves.
//   3. Gives future UMP-aware backends (WinRT MIDI 2.0, ALSA UMP,
//      iOS CoreMIDI 2.0) a drop-in dependency.
//
// Thread-safety
// =============
//
// The reassembler holds independent state for each UMP group. A configured
// maximum bounds every logical message; overflow discards the whole message.
// It is **not** thread-safe — each input port / source should own its
// own instance. The CoreMIDI input callback is single-threaded per
// port, and AUv3 render blocks are single-threaded per audio bus, so
// the per-stream model fits both call sites.
//
// RT-safety
// =========
//
// `feed_packet()` may allocate inside the internal `std::vector`s as payload
// storage grows past current capacity. Callers that want strict RT-safety
// should `reserve()` the configured maximum up front via `reserve(N)`. This
// also makes N the admission limit; packets that would exceed it are dropped
// atomically with an overflow diagnostic.
//
// Copies retain prepared capacity and partial state; copy/move operations are
// non-RT. A moved-from instance must be reserved again before RT reuse.
//
// Unit tests live in `test/test_ump_sysex7_reassembler.cpp`.

#include <cstddef>
#include <cstdint>
#include <array>
#include <utility>
#include <vector>

namespace pulp::midi {

/// Callback shape: receives the complete UMP SysEx7 payload bytes, with no
/// synthesised MIDI 1.0 F0/F7 framing. UMP framing is implicit in the
/// start/continue/end status nibble; callers crossing into MIDI 1.0 must add
/// F0/F7 explicitly at that boundary.
///
/// The reassembler invokes this callback exactly once per logical
/// sysex (either a single complete packet, or a start → continue* →
/// end span), including a zero-byte payload. Orphaned continue/end packets (i.e. arrival without a
/// preceding start) are dropped silently — corrupting the downstream
/// sink with a partial payload is worse than dropping one malformed
/// sysex.
using Sysex7EmitFn = void(*)(const std::vector<std::uint8_t>& payload,
                             void* user);

/// Reassembler state machine for UMP Type-0x3 sysex7 packet streams.
///
/// Per-stream — see "Thread-safety" in the file header.
class UmpSysex7Reassembler {
public:
    static constexpr std::size_t default_maximum_payload_bytes = 64u * 1024u;

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

    /// Why the last packet was dropped (or why a partial message was reset).
    /// This is deliberately separate from Status so existing callers that
    /// only distinguish emitted versus dropped packets remain source
    /// compatible while diagnostics can fail closed.
    enum class DropReason {
        none,
        orphan,
        reserved_status,
        invalid_size,
        high_bit_payload,
        overflow,
        nested_start,
        reset,
    };

    explicit UmpSysex7Reassembler(
        std::size_t maximum_payload_bytes = default_maximum_payload_bytes)
        : maximum_payload_bytes_(maximum_payload_bytes) {}

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
        const std::uint8_t group = static_cast<std::uint8_t>((word0 >> 24) & 0x0F);
        auto& stream = streams_[group];
        last_drop_reason_ = DropReason::none;
        const std::uint8_t status =
            static_cast<std::uint8_t>((word0 >> 20) & 0x0F);
        const std::uint32_t size =
            static_cast<std::uint32_t>((word0 >> 16) & 0x0F);

        if (size > 6) {
            stream.payload.clear();
            stream.in_progress = false;
            last_drop_reason_ = DropReason::invalid_size;
            return Status::dropped;
        }
        if (!payload_is_7bit(word0, word1, size)) {
            stream.payload.clear();
            stream.in_progress = false;
            last_drop_reason_ = DropReason::high_bit_payload;
            return Status::dropped;
        }

        switch (status) {
        case 0x0: {
            // Complete single-packet sysex. Use a scratch vector so
            // the call doesn't disturb any partial in-progress state
            // when a complete packet interleaves with another stream's
            // accumulator. This is genuinely possible: the UMP spec
            // permits independent sysex streams on different groups to
            // interleave through a single endpoint.
            // A complete packet on this group starts a new logical stream;
            // discard any unterminated message rather than splicing it.
            stream.payload.clear();
            stream.in_progress = false;
            stream.scratch.clear();
            if (size > maximum_payload_bytes_) {
                last_drop_reason_ = DropReason::overflow;
                return Status::dropped;
            }
            extract_bytes(word0, word1, size, stream.scratch);
            if (emit) {
                emit(stream.scratch, user);
            }
            return Status::single_packet;
        }
        case 0x1: {
            // Start — reset the accumulator and begin capture. If a
            // previous in-progress sysex was never closed, it is
            // silently dropped: the spec doesn't allow nested sysex,
            // and resetting is the only sane recovery.
            if (stream.in_progress)
                last_drop_reason_ = DropReason::nested_start;
            stream.payload.clear();
            stream.in_progress = false;
            if (size > maximum_payload_bytes_) {
                last_drop_reason_ = DropReason::overflow;
                return Status::dropped;
            }
            extract_bytes(word0, word1, size, stream.payload);
            stream.in_progress = true;
            return Status::start;
        }
        case 0x2:
            if (stream.in_progress) {
                if (size > maximum_payload_bytes_ ||
                    stream.payload.size() > maximum_payload_bytes_ - size) {
                    stream.payload.clear();
                    stream.in_progress = false;
                    last_drop_reason_ = DropReason::overflow;
                    return Status::dropped;
                }
                extract_bytes(word0, word1, size, stream.payload);
                return Status::continued;
            }
            // Orphan continue — drop.
            last_drop_reason_ = DropReason::orphan;
            return Status::dropped;
        case 0x3:
            if (stream.in_progress) {
                if (size > maximum_payload_bytes_ ||
                    stream.payload.size() > maximum_payload_bytes_ - size) {
                    stream.payload.clear();
                    stream.in_progress = false;
                    last_drop_reason_ = DropReason::overflow;
                    return Status::dropped;
                }
                extract_bytes(word0, word1, size, stream.payload);
                if (emit) {
                    emit(stream.payload, user);
                }
                stream.payload.clear();
                stream.in_progress = false;
                return Status::ended;
            }
            // Orphan end — drop.
            last_drop_reason_ = DropReason::orphan;
            return Status::dropped;
        default:
            // Reserved / unknown status nibble. Per the spec, type-0x3
            // status values 0x4..0xF are reserved; the only sane
            // behavior is to drop the packet and discard the addressed
            // group's partial message so malformed data cannot splice.
            stream.payload.clear();
            stream.in_progress = false;
            last_drop_reason_ = DropReason::reserved_status;
            return Status::dropped;
        }
    }

    /// True when an in-progress sysex is open (a 0x1 start packet has
    /// been seen and no 0x3 end packet has closed it yet).
    bool in_progress() const noexcept {
        for (const auto& stream : streams_)
            if (stream.in_progress) return true;
        return false;
    }

    /// Current accumulated payload size in bytes (0 when idle).
    std::size_t partial_size(std::uint8_t group = 0) const noexcept {
        return streams_[group & 0x0F].payload.size();
    }

    /// Reserve worst-case payload capacity to avoid allocations in the
    /// hot path. Safe to call at port-open / prepare() time. The single-
    /// packet scratch buffer is also prepared here so interleaved complete
    /// sysex packets do not allocate while another sysex is open. This sets
    /// the exact admission bound and discards any current partial streams;
    /// call it during non-RT configuration.
    void reserve(std::size_t worst_case_bytes) {
        reset();
        maximum_payload_bytes_ = worst_case_bytes;
        for (auto& stream : streams_) {
            stream.payload.reserve(worst_case_bytes);
            stream.scratch.reserve(6);
        }
    }

    std::size_t maximum_payload_bytes() const noexcept {
        return maximum_payload_bytes_;
    }

    DropReason last_drop_reason() const noexcept { return last_drop_reason_; }

    /// Drop any in-progress sysex without emitting. Use on port-close
    /// or when a transport-level reset is observed (e.g. CoreMIDI
    /// endpoint reconnect).
    void reset() noexcept {
        for (auto& stream : streams_) {
            stream.payload.clear();
            stream.in_progress = false;
        }
        last_drop_reason_ = DropReason::reset;
    }

private:
    // Copying is non-RT configuration work. Preserve prepared capacity even
    // for empty vectors so the copied instance has the same hot-path budget.
    struct StreamState {
        StreamState() = default;
        StreamState(const StreamState& other) { *this = other; }
        StreamState& operator=(const StreamState& other) {
            if (this == &other) return *this;
            payload.reserve(other.payload.capacity());
            scratch.reserve(other.scratch.capacity());
            payload.assign(other.payload.begin(), other.payload.end());
            scratch.assign(other.scratch.begin(), other.scratch.end());
            in_progress = other.in_progress;
            return *this;
        }
        StreamState(StreamState&& other) noexcept
            : payload(std::move(other.payload)), scratch(std::move(other.scratch)),
              in_progress(std::exchange(other.in_progress, false)) {}
        StreamState& operator=(StreamState&& other) noexcept {
            if (this == &other) return *this;
            payload = std::move(other.payload);
            scratch = std::move(other.scratch);
            in_progress = std::exchange(other.in_progress, false);
            return *this;
        }
        std::vector<std::uint8_t> payload;
        std::vector<std::uint8_t> scratch;
        bool in_progress = false;
    };

    static bool payload_is_7bit(std::uint32_t w0, std::uint32_t w1,
                                std::uint32_t size) noexcept {
        const std::uint8_t bytes[6] = {
            static_cast<std::uint8_t>((w0 >> 8) & 0xFF),
            static_cast<std::uint8_t>(w0 & 0xFF),
            static_cast<std::uint8_t>((w1 >> 24) & 0xFF),
            static_cast<std::uint8_t>((w1 >> 16) & 0xFF),
            static_cast<std::uint8_t>((w1 >> 8) & 0xFF),
            static_cast<std::uint8_t>(w1 & 0xFF),
        };
        for (std::uint32_t i = 0; i < size; ++i)
            if ((bytes[i] & 0x80) != 0) return false;
        return true;
    }

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
        // `size` is the 4-bit field from bits 16-19 of w0. feed_packet()
        // rejects values above the six payload bytes in a SysEx7 packet.
        const std::uint8_t buf[6] = {
            static_cast<std::uint8_t>((w0 >>  8) & 0xFF),
            static_cast<std::uint8_t>((w0 >>  0) & 0xFF),
            static_cast<std::uint8_t>((w1 >> 24) & 0xFF),
            static_cast<std::uint8_t>((w1 >> 16) & 0xFF),
            static_cast<std::uint8_t>((w1 >>  8) & 0xFF),
            static_cast<std::uint8_t>((w1 >>  0) & 0xFF),
        };
        for (std::uint32_t i = 0; i < size; ++i) out.push_back(buf[i]);
    }

    std::array<StreamState, 16> streams_{};
    std::size_t maximum_payload_bytes_ = default_maximum_payload_bytes;
    DropReason last_drop_reason_ = DropReason::none;
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
