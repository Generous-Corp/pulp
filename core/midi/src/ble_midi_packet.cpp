// BLE-MIDI 1.0 packet codec — cross-platform, Skia-free.
//
// Implements the Apple BLE-MIDI 1.0 transport spec for both decoding
// inbound GATT notifications and encoding outbound MIDI messages.
//
// Packet layout (>= 3 bytes per the spec):
//   byte 0  : header     = 0x80 | (timestamp_hi & 0x3F)
//   byte 1  : timestamp  = 0x80 | (timestamp_lo & 0x7F)   (first message)
//   byte 2+ : MIDI bytes (may include running-status, multi-message,
//             and split-sysex per spec section 3.4)
//
// Subsequent messages in the same packet may either
//   (a) prefix another timestamp byte (0x80-bit set), or
//   (b) continue running status (no timestamp, status omitted), or
//   (c) be a sysex chunk (no timestamp) terminating with 0xF7.

#include <pulp/midi/ble_midi.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::midi {

namespace {

// Status-byte channel-voice message lengths (lower nibble = channel).
// Index by the upper nibble (>> 4). Real-time + system bytes handled
// separately because they don't carry channel info.
constexpr int channel_message_length(uint8_t status_high_nibble) {
    switch (status_high_nibble) {
        case 0x8: return 2;  // Note Off
        case 0x9: return 2;  // Note On
        case 0xA: return 2;  // Poly aftertouch
        case 0xB: return 2;  // CC
        case 0xC: return 1;  // Program Change
        case 0xD: return 1;  // Channel aftertouch
        case 0xE: return 2;  // Pitch Bend
        default:  return -1;
    }
}

constexpr int system_common_length(uint8_t status) {
    switch (status) {
        case 0xF1: return 1;  // MTC Quarter Frame
        case 0xF2: return 2;  // Song Position Pointer
        case 0xF3: return 1;  // Song Select
        case 0xF6: return 0;  // Tune Request
        // 0xF4, 0xF5 — undefined.
        default:   return -1;
    }
}

constexpr bool is_realtime(uint8_t b) {
    return b >= 0xF8;
}

constexpr bool is_status(uint8_t b) {
    return (b & 0x80) != 0;
}

}  // namespace

bool BleMidiPacketDecoder::decode(const uint8_t* data, std::size_t size) {
    // Spec minimum: 1 header + 1 timestamp + 1 status byte.
    if (size < 3 || data == nullptr) return false;

    const uint8_t header = data[0];
    if ((header & 0x80) == 0) return false;       // Top bit must be set.
    last_header_ts_ = static_cast<uint8_t>(header & 0x3F);

    // BLE-MIDI running status is scoped to a SINGLE GATT packet
    // (Apple BLE-MIDI 1.0 spec §3.4). Carrying `last_status_` across
    // packets would let a leading data byte in the next packet be
    // mis-emitted as a fabricated channel-voice event using the prior
    // packet's status. Sysex is the only state explicitly defined to
    // span packets — we leave `sysex_buffer_` alone. Codex PR #3017 P2.
    last_status_ = 0;

    std::size_t i = 1;
    uint8_t cur_ts_lo = 0;
    bool ts_seen = false;

    while (i < size) {
        const uint8_t b = data[i];

        // Real-time bytes are inserted anywhere and may have their own
        // timestamp prefix per spec; emit immediately, do not affect
        // running status.
        if (is_realtime(b)) {
            std::vector<uint8_t> msg{b};
            const uint32_t ts = (static_cast<uint32_t>(last_header_ts_) << 7)
                              | static_cast<uint32_t>(cur_ts_lo & 0x7F);
            if (cb_) cb_(msg, ts);
            ++i;
            continue;
        }

        // Timestamp byte: top bit set on a non-status, non-realtime byte
        // ALSO with top bit set — i.e. we recognise it as a timestamp
        // when we are at a message boundary. The simple rule used by
        // the Apple reference: any byte at message-start with 0x80 set
        // is a timestamp byte UNLESS it falls inside an active sysex.
        if ((b & 0x80) != 0 && sysex_buffer_.empty()) {
            // Could be a timestamp OR a status byte. The spec encodes
            // both with the top bit set; status bytes are >= 0x80 and
            // include 0x80..0xEF (channel) and 0xF0..0xF7 (system).
            // Timestamp bytes are 0x80..0xFF but follow a fresh
            // header/timestamp boundary; the way the spec disambiguates
            // is positional — a timestamp ALWAYS precedes a status or a
            // running-status data sequence. Heuristic: if the previous
            // emitted byte position was not a data byte AND b looks like
            // a valid timestamp (we're at the start of a new message),
            // treat it as a timestamp.
            //
            // The deterministic test: if b is a recognised MIDI status
            // (>=0xF0 system) or channel status (0x80..0xEF), and
            // running status is unset, parse as status. Else timestamp.
            const bool looks_like_status =
                (b >= 0x80 && b <= 0xEF) || (b >= 0xF0 && b <= 0xF7);
            if (!looks_like_status || ts_seen == false) {
                // Treat as timestamp; the next byte must be status.
                cur_ts_lo = static_cast<uint8_t>(b & 0x7F);
                ts_seen = true;
                ++i;
                continue;
            }
        }

        // Sysex continuation. If a previous packet's 0xF0 left bytes in
        // the in-flight buffer, append data bytes until 0xF7. The
        // terminator byte is 0xF7 (status); when we hit it we emit the
        // accumulated message.
        if (!sysex_buffer_.empty()) {
            if (b == 0xF7) {
                sysex_buffer_.push_back(b);
                const uint32_t ts = (static_cast<uint32_t>(last_header_ts_) << 7)
                                  | static_cast<uint32_t>(cur_ts_lo & 0x7F);
                if (cb_) cb_(sysex_buffer_, ts);
                sysex_buffer_.clear();
                ++i;
                continue;
            }
            // Data byte mid-sysex — append and continue.
            sysex_buffer_.push_back(b);
            ++i;
            continue;
        }

        // Status byte: starts a new message OR continues running status.
        if (is_status(b)) {
            // System exclusive start.
            if (b == 0xF0) {
                sysex_buffer_.clear();
                sysex_buffer_.push_back(b);
                last_status_ = 0;  // Sysex cancels running status.
                ++i;
                continue;
            }
            // Channel-voice or system-common status.
            last_status_ = b;
            const int needed =
                (b >= 0xF0) ? system_common_length(b)
                            : channel_message_length(static_cast<uint8_t>(b >> 4));
            if (needed < 0) {
                // Unknown status — drop and resync.
                last_status_ = 0;
                ++i;
                continue;
            }
            if (i + 1 + static_cast<std::size_t>(needed) > size) {
                // Truncated message — drop and stop parsing this packet.
                return false;
            }
            std::vector<uint8_t> msg;
            msg.reserve(1 + needed);
            msg.push_back(b);
            for (int n = 0; n < needed; ++n) msg.push_back(data[i + 1 + n]);
            const uint32_t ts = (static_cast<uint32_t>(last_header_ts_) << 7)
                              | static_cast<uint32_t>(cur_ts_lo & 0x7F);
            if (cb_) cb_(msg, ts);
            i += 1 + needed;
            continue;
        }

        // Running-status data. Re-emit the previous status byte with
        // this data.
        if (last_status_ != 0) {
            const int needed = channel_message_length(
                static_cast<uint8_t>(last_status_ >> 4));
            if (needed < 0) {
                ++i;
                continue;
            }
            if (i + static_cast<std::size_t>(needed) - 1 >= size) return false;
            std::vector<uint8_t> msg;
            msg.reserve(1 + needed);
            msg.push_back(last_status_);
            for (int n = 0; n < needed; ++n) msg.push_back(data[i + n]);
            const uint32_t ts = (static_cast<uint32_t>(last_header_ts_) << 7)
                              | static_cast<uint32_t>(cur_ts_lo & 0x7F);
            if (cb_) cb_(msg, ts);
            i += needed;
            continue;
        }

        // Orphan data byte — drop and continue.
        ++i;
    }

    return true;
}

void BleMidiPacketDecoder::reset() {
    last_status_ = 0;
    sysex_buffer_.clear();
    last_header_ts_ = 0;
}

std::vector<uint8_t> encode_ble_midi_packet(const uint8_t* msg,
                                            std::size_t size,
                                            uint32_t timestamp_ms) {
    std::vector<uint8_t> out;
    if (msg == nullptr || size == 0) return out;

    // Truncate timestamp to 13 bits per spec (8192 ms rollover).
    const uint32_t ts13 = timestamp_ms & 0x1FFF;
    const uint8_t header = static_cast<uint8_t>(0x80 | ((ts13 >> 7) & 0x3F));
    const uint8_t ts_lo  = static_cast<uint8_t>(0x80 | (ts13 & 0x7F));

    out.reserve(size + 2);
    out.push_back(header);
    out.push_back(ts_lo);
    for (std::size_t i = 0; i < size; ++i) out.push_back(msg[i]);
    return out;
}

}  // namespace pulp::midi
