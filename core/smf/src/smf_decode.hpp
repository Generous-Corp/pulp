#pragma once

#include "smf_error.hpp"

#include <pulp/runtime/result.hpp>
#include <pulp/timeline/smf.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pulp::timeline::detail {

// The decoded shape of one supported SMF event. Decoding resolves the byte
// stream (running status, variable-length quantities, chunk framing) into this
// closed set; the importer above it only reasons about musical meaning.
enum class SmfMessage : std::uint8_t {
    NoteOn,
    NoteOff,
    Tempo,
    TimeSignature,
};

struct SmfDecodedEvent {
    // Absolute position within the owning track, in SMF ticks.
    std::int64_t tick = 0;
    // Decode order within the track. Ties at one tick keep file order, which is
    // the only ordering the format defines for simultaneous events.
    std::uint32_t order = 0;
    SmfMessage message = SmfMessage::NoteOn;
    std::uint8_t channel = 0;
    std::uint8_t pitch = 0;
    // Note On/Off velocity, in the 7-bit MIDI domain.
    std::uint8_t velocity = 0;
    // Set Tempo payload.
    std::uint32_t microseconds_per_quarter = 0;
    // Time Signature numerator, and the power of two naming its denominator.
    std::uint8_t meter_numerator = 0;
    std::uint8_t meter_denominator_power = 0;
};

struct SmfDecodedTrack {
    std::string name;
    std::vector<SmfDecodedEvent> events;
};

struct SmfDecodedFile {
    std::uint16_t format = 0;
    std::uint16_t division = 0;
    std::vector<SmfDecodedTrack> tracks;
};

// Frame and decode SMF bytes. Enforces every limit before growing state and
// rejects malformed or out-of-subset input rather than skipping it.
runtime::Result<SmfDecodedFile, SmfError> decode_smf(std::span<const std::uint8_t> file_bytes,
                                                     const SmfImportOptions& options);

} // namespace pulp::timeline::detail
