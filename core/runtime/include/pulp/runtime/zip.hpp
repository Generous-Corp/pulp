#pragma once

// ZIP archive and GZIP compression utilities via miniz.

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <optional>

namespace pulp::runtime {

// ── GZIP compression/decompression ──────────────────────────────────────

/// Compress data using GZIP.
std::optional<std::vector<uint8_t>> gzip_compress(const uint8_t* data, size_t size,
                                                   int level = 6);

/// Decompress GZIP data.
std::optional<std::vector<uint8_t>> gzip_decompress(const uint8_t* data, size_t size);

/// Convenience: compress a string.
std::optional<std::vector<uint8_t>> gzip_compress(std::string_view data, int level = 6);

/// Convenience: decompress to a string.
std::optional<std::string> gzip_decompress_string(const uint8_t* data, size_t size);

// ── Raw deflate compression ─────────────────────────────────────────────

/// Compress data using raw deflate (no header).
std::optional<std::vector<uint8_t>> deflate_compress(const uint8_t* data, size_t size,
                                                      int level = 6);

/// Decompress raw deflate data.
std::optional<std::vector<uint8_t>> deflate_decompress(const uint8_t* data, size_t size);

}  // namespace pulp::runtime
