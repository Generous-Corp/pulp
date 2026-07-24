#pragma once

#include <pulp/playback/external_sync.hpp>

#include <cstdint>

namespace pulp::playback::detail {

struct FrameRateRatio {
    std::uint32_t numerator;
    std::uint32_t denominator;
    std::uint8_t nominal;
    std::uint8_t mtc_code;
};

FrameRateRatio frame_rate_ratio(MtcFrameRate rate) noexcept;
bool valid_frame_rate(MtcFrameRate rate) noexcept;
MtcTimecode frame_count_to_timecode(std::int64_t frames, MtcFrameRate rate) noexcept;

} // namespace pulp::playback::detail
