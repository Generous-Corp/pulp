#include <pulp/timeline/assets.hpp>

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

} // namespace pulp::timeline
