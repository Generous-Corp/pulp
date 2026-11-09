#pragma once

/// @file midi_learn_persistence.hpp
/// Saving and loading MIDI mappings: a versioned binary chunk for a plug-in's
/// state blob, and a JSON text form for preset/import-export files.
///
/// Both are pure functions over bytes or text (UI / main thread; they
/// allocate). Neither touches a `StateStore`.
///
/// Binary chunk (little-endian):
///   0  "PMLN"            magic
///   4  u16 version       currently 1
///   6  u16 record_size   bytes per record (28 in version 1)
///   8  u32 count
///   12 u32 checksum      FNV-1a over the `count * record_size` record bytes
///   16 records
/// A reader of an older version rejects a record_size below what it knows. A
/// newer version with larger records still parses: the known leading fields
/// of each record are read and the rest is skipped, and the result reports
/// `newer_version`. An empty buffer is a state saved before mappings existed
/// and parses as an empty, ok result.

#include <cstdint>
#include <pulp/state/midi_mapping.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::state {

inline constexpr uint16_t kMidiMapChunkVersion = 1;

struct MidiMapParseResult {
    enum class Status : uint8_t {
        ok,            ///< Parsed (possibly zero mappings).
        newer_version, ///< Written by a newer version; known fields were read.
        corrupt,       ///< Bad magic, size or checksum; nothing was read.
    };

    Status status = Status::ok;
    uint16_t version = 0;
    std::vector<MidiMapping> mappings; ///< `uid` is 0; the engine assigns it.
    uint32_t dropped = 0;              ///< Records skipped as invalid or over capacity.
};

/// Encode mappings as a binary chunk. At most `kMaxMidiMappings` are written.
std::vector<uint8_t> serialize_midi_mappings(std::span<const MidiMapping> mappings);

MidiMapParseResult parse_midi_mappings(std::span<const uint8_t> data);

/// Encode mappings as a JSON document (`{"version":1,"mappings":[...]}`).
std::string midi_mappings_to_json(std::span<const MidiMapping> mappings);

/// Parse a JSON document written by `midi_mappings_to_json`. Entries with an
/// unknown source kind or an out-of-range field are dropped and counted.
/// Malformed JSON yields `corrupt`.
MidiMapParseResult midi_mappings_from_json(std::string_view text);

/// True when `m` has an in-range source, mode and takeover for its kind.
bool is_valid_midi_mapping(const MidiMapping& m);

} // namespace pulp::state
