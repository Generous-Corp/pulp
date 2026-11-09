#pragma once

#include <pulp/audio/audio_file.hpp>

#include <cstdint>
#include <istream>
#include <optional>

namespace pulp::audio::detail {

struct AiffLayout {
    AudioFileInfo info;
    uint64_t sample_data_offset = 0;
    uint64_t sample_data_size = 0;
    uint32_t bytes_per_sample = 0;
    uint32_t bytes_per_frame = 0;
    /// AIFF-C `sowt` stores its PCM sample points little-endian; AIFF and the
    /// `NONE`/`twos` AIFF-C spellings are big-endian.
    bool little_endian = false;
};

std::optional<AiffLayout> parse_aiff_layout(std::istream& stream,
                                            uint64_t file_size,
                                            bool require_pcm_data);

/// Decode one left-justified PCM sample point of `bits_per_sample` bits.
/// `little_endian` selects the `sowt` byte order; false is AIFF's own.
float decode_aiff_pcm_sample(const uint8_t* bytes, uint32_t bits_per_sample, bool little_endian);

}  // namespace pulp::audio::detail
