#include <pulp/timeline/assets.hpp>
#include <pulp/timeline/schema_json.hpp>

#include "asset_validation.hpp"

#include <algorithm>

namespace pulp::timeline {
namespace {

constexpr int hex_digit(char value) noexcept {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    return -1;
}

} // namespace

std::optional<ContentHash> ContentHash::from_hex(std::string_view hex) noexcept {
    if (hex.size() != 64)
        return std::nullopt;
    std::array<std::uint8_t, 32> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto high = hex_digit(hex[index * 2]);
        const auto low = hex_digit(hex[index * 2 + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return ContentHash(bytes);
}

std::string ContentHash::to_hex() const {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(64, '0');
    for (std::size_t index = 0; index < bytes_.size(); ++index) {
        result[index * 2] = digits[bytes_[index] >> 4];
        result[index * 2 + 1] = digits[bytes_[index] & 0x0f];
    }
    return result;
}

bool detail::validate_and_canonicalize(AudioLoopInfo& loop, std::uint64_t frame_count) {
    const auto denominator = loop.meter.denominator;
    if ((loop.musical_length &&
         (loop.musical_length->value <= 0 || frame_count == 0)) ||
        loop.meter.numerator <= 0 || denominator <= 0 ||
        (static_cast<std::uint32_t>(denominator) &
         (static_cast<std::uint32_t>(denominator) - 1)) != 0 ||
        denominator > 4 * timebase::kTicksPerQuarter ||
        (4 * timebase::kTicksPerQuarter) % denominator != 0 ||
        (loop.root_note && *loop.root_note > 127))
        return false;
    if (loop.active_range &&
        (loop.active_range->end_frame <= loop.active_range->start_frame ||
         loop.active_range->end_frame > frame_count))
        return false;
    std::sort(loop.points.begin(), loop.points.end(),
              [](const AudioLoopPoint& lhs, const AudioLoopPoint& rhs) {
                  return lhs.frame < rhs.frame;
              });
    for (std::size_t index = 0; index < loop.points.size(); ++index)
        if (loop.points[index].frame > frame_count ||
            (loop.points[index].kind != AudioLoopPointKind::Manual &&
             loop.points[index].kind != AudioLoopPointKind::Automatic) ||
            (index != 0 && loop.points[index - 1].frame == loop.points[index].frame))
            return false;
    for (const auto& tag : loop.tags)
        if (tag.empty() || !is_valid_utf8(tag))
            return false;
    std::sort(loop.tags.begin(), loop.tags.end());
    return std::adjacent_find(loop.tags.begin(), loop.tags.end()) == loop.tags.end();
}

} // namespace pulp::timeline
