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
};

std::optional<AiffLayout> parse_aiff_layout(std::istream& stream,
                                            uint64_t file_size,
                                            bool require_pcm_data);

float decode_aiff_pcm_sample(const uint8_t* bytes, uint32_t bits_per_sample);

}  // namespace pulp::audio::detail
