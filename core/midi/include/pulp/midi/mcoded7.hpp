#pragma once

// Mcoded7 — MIDI 2.0 Capability Inquiry property-exchange byte-packing.
// Packs 7 arbitrary 8-bit bytes into 8 MIDI bytes (a leading high-bit byte
// followed by seven low-7-bit bytes), so binary payloads can travel inside a
// SysEx envelope without ever setting bit 7.
//
// Reference: MIDI 2.0 / MIDI-CI v1.2 spec, "Mcoded7 Data Encoding".

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::midi {

/// Encode raw 8-bit bytes into Mcoded7. Output is `ceil(input/7) * 8 - extra`
/// where `extra` trims trailing slots when the final group is short.
/// The encoded stream contains only values <= 0x7F and is safe to embed in
/// MIDI SysEx.
std::vector<uint8_t> mcoded7_encode(const uint8_t* data, std::size_t size);

inline std::vector<uint8_t> mcoded7_encode(const std::vector<uint8_t>& data) {
    return mcoded7_encode(data.data(), data.size());
}

/// Decode an Mcoded7-encoded stream back to raw 8-bit bytes.
/// Returns an empty vector if the input is malformed (contains a byte
/// with bit 7 set anywhere it must not, or a final group's high-bit
/// byte references more low bytes than were actually transmitted).
std::vector<uint8_t> mcoded7_decode(const uint8_t* data, std::size_t size);

inline std::vector<uint8_t> mcoded7_decode(const std::vector<uint8_t>& data) {
    return mcoded7_decode(data.data(), data.size());
}

}  // namespace pulp::midi
