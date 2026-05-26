// wav_metadata.hpp — read + write WAV ancillary metadata chunks.
//
// Implements item 6.11 of the 2026-05-24 macOS plugin authoring plan
// (BWAV / iXML / ASWG / ACID). Reads metadata from any RIFF-WAVE or
// RF64-extended WAV file, lets you mutate it in-memory, then writes it
// back without rewriting the audio payload (the original `fmt ` and
// `data` chunks are preserved verbatim).
//
// Why a hand-rolled chunk codec on top of CHOC: CHOC's WAV reader
// gives us PCM samples but only surfaces a small subset of metadata
// (bext fields as a string map, no iXML/axml/acid). We need exact,
// reversible byte-level access to the named chunks so production-audio
// metadata round-trips losslessly through Pulp tooling.
//
// Chunks supported (canonical four-CCs in parens):
//   • bext  — EBU R 128 / EBU Tech 3285 "Broadcast Wave Format" extension.
//   • iXML  — Gallery iXML production-metadata XML chunk.
//   • axml  — ASWG / EBU 3285s5 generic XML metadata chunk (often
//             called "ASWG" by field-recorder makers).
//   • acid  — Sony ACIDized-loop info chunk (tempo / root note / loop
//             markers, used by ACID, Ableton Live, FL Studio, etc.).
//
// Unknown chunks present in the source file are preserved on write so
// `replace_metadata_in_file()` only touches the four supported chunk
// kinds.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pulp::audio {

// ── Broadcast Wave Format (bext) ────────────────────────────────────────────
//
// EBU Tech 3285 v2 layout. Field sizes are fixed by the spec; we model
// the strings as `std::string` for ergonomic round-trip but they are
// padded / truncated to the exact byte counts during encoding.
struct BwavMetadata {
    std::string description;          // 256 bytes
    std::string originator;           // 32 bytes
    std::string originator_reference; // 32 bytes
    std::string origination_date;     // 10 bytes "YYYY-MM-DD"
    std::string origination_time;     // 8 bytes "HH:MM:SS"
    uint64_t time_reference = 0;      // samples since midnight (low+high u32)
    uint16_t version = 1;             // bext version (1 or 2)
    std::array<uint8_t, 64> umid{};   // SMPTE 330M UMID (zero by default)
    int16_t loudness_value = 0;       // 1/100 LUFS (bext v2 only)
    int16_t loudness_range = 0;       // 1/100 LU
    int16_t max_true_peak_level = 0;  // 1/100 dBTP
    int16_t max_momentary_loudness = 0;
    int16_t max_short_term_loudness = 0;
    std::string coding_history;       // free-form ASCII (variable length)
};

// ── ACID loop info (acid) ───────────────────────────────────────────────────
//
// Sony ACIDized-loop chunk, 24 bytes total. `flags` bit semantics:
//   0x01  one-shot (no loop)
//   0x02  root note set
//   0x04  stretch
//   0x08  disk-based
//   0x10  ACIDizer (the file claims its metadata is authoritative)
struct AcidMetadata {
    uint32_t flags = 0;
    uint16_t root_note = 60;          // MIDI note number; only valid when flags & 0x02
    uint16_t reserved1 = 0;
    float reserved2 = 0.0f;
    uint32_t num_beats = 0;
    uint16_t meter_denominator = 4;   // bottom of time signature (2,4,8,16)
    uint16_t meter_numerator = 4;     // top of time signature
    float tempo_bpm = 0.0f;
};

// ── Aggregate ───────────────────────────────────────────────────────────────
//
// Each metadata kind is optional. Unset = chunk not present (read) or
// will not be written (write). `unknown_chunks` lets `replace_metadata_in_file`
// preserve every other RIFF chunk verbatim.
struct WavMetadata {
    std::optional<BwavMetadata> bwav;
    std::optional<AcidMetadata> acid;
    std::optional<std::string> ixml;  // raw XML payload (UTF-8)
    std::optional<std::string> axml;  // raw XML payload (ASWG)

    struct UnknownChunk {
        std::array<char, 4> id{};
        std::vector<uint8_t> data;
    };
    std::vector<UnknownChunk> unknown_chunks; // preserved as-is on write
};

// ── Public API ──────────────────────────────────────────────────────────────
//
// `read_wav_metadata` returns nullopt only on I/O error or a malformed
// RIFF header. A successful read with zero recognised chunks returns an
// empty WavMetadata struct, not nullopt.
std::optional<WavMetadata> read_wav_metadata(const std::string& path);

// `write_wav_metadata` writes a brand-new WAV with the given metadata
// chunks plus a minimal silent `fmt ` + `data`. Useful for fixtures
// and round-trip tests; prefer `replace_metadata_in_file` for editing
// existing files in place.
bool write_wav_metadata(const std::string& path,
                        const WavMetadata& metadata,
                        uint32_t sample_rate = 48000,
                        uint16_t num_channels = 1,
                        uint16_t bits_per_sample = 16);

// `replace_metadata_in_file` rewrites the metadata chunks of an
// existing WAV without touching its `fmt ` or `data` payload. Returns
// false on I/O error or if the source is not a RIFF-WAVE file. Any
// supported chunk that is absent from `metadata` is removed; unknown
// chunks present in `metadata.unknown_chunks` are written back as-is.
bool replace_metadata_in_file(const std::string& path, const WavMetadata& metadata);

// ── Codec primitives (exposed for tests / advanced callers) ─────────────────
std::vector<uint8_t> encode_bext_chunk(const BwavMetadata& bwav);
std::vector<uint8_t> encode_acid_chunk(const AcidMetadata& acid);

std::optional<BwavMetadata> decode_bext_chunk(const uint8_t* data, size_t size);
std::optional<AcidMetadata> decode_acid_chunk(const uint8_t* data, size_t size);

} // namespace pulp::audio
