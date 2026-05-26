#pragma once

// ZIP archive and GZIP compression utilities via miniz.

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <optional>

namespace pulp::runtime {

// ── GZIP compression/decompression ──────────────────────────────────────
//
// gzip_compress() emits a real RFC 1952 gzip stream:
//
//   header(10) || raw deflate || CRC32(little-endian, 4) || ISIZE(little-endian, 4)
//
// gzip_decompress() accepts both gzip (RFC 1952, magic 0x1f 0x8b) and zlib
// (RFC 1950) input. The zlib lane is preserved for backward compatibility
// with older callers and test fixtures that historically passed zlib-wrapped
// data through this entry point. Earlier revisions of this API silently
// produced and consumed zlib streams under a "gzip" name; that asymmetry
// has been removed — the function now actually handles RFC 1952 gzip.

/// Compress data into an RFC 1952 gzip stream.
std::optional<std::vector<uint8_t>> gzip_compress(const uint8_t* data, size_t size,
                                                   int level = 6);

/// Decompress a gzip (RFC 1952) or zlib (RFC 1950) stream.
std::optional<std::vector<uint8_t>> gzip_decompress(const uint8_t* data, size_t size);

/// Convenience: compress a string into an RFC 1952 gzip stream.
std::optional<std::vector<uint8_t>> gzip_compress(std::string_view data, int level = 6);

/// Convenience: decompress a gzip (RFC 1952) or zlib (RFC 1950) stream into a string.
std::optional<std::string> gzip_decompress_string(const uint8_t* data, size_t size);

// ── Raw deflate compression ─────────────────────────────────────────────

/// Compress data using raw deflate (no header).
std::optional<std::vector<uint8_t>> deflate_compress(const uint8_t* data, size_t size,
                                                      int level = 6);

/// Decompress raw deflate data.
std::optional<std::vector<uint8_t>> deflate_decompress(const uint8_t* data, size_t size);

// ── zlib (RFC 1950) compression ─────────────────────────────────────────
//
// zlib_compress emits a real RFC 1950 zlib stream:
//
//   CMF(1) FLG(1) || raw deflate || Adler32(big-endian, 4)
//
// Used for the MIDI-CI 1.2 Property Exchange `zlib+Mcoded7` payload
// encoding (PE spec §6.3). For symmetry with gzip_compress this
// emits a self-contained stream; gzip_decompress() already accepts
// zlib input on the inflate side.

/// Compress data into an RFC 1950 zlib stream.
std::optional<std::vector<uint8_t>> zlib_compress(const uint8_t* data, size_t size,
                                                  int level = 6);

}  // namespace pulp::runtime
