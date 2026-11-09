#pragma once

// Shared sample-rate tables for the built-in readers' and writers'
// capabilities() reports. Internal to pulp::audio.

#include <cstdint>
#include <vector>

namespace pulp::audio::detail {

/// Rates an MP3 stream can carry: MPEG-1, MPEG-2 and MPEG-2.5 layer III.
inline std::vector<std::uint32_t> mp3_sample_rates() {
    return {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
}

/// Rates the AAC-LC encoder accepts.
inline std::vector<std::uint32_t> aac_sample_rates() {
    return {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000};
}

} // namespace pulp::audio::detail
