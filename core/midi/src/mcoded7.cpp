#include <pulp/midi/mcoded7.hpp>

namespace pulp::midi {

std::vector<uint8_t> mcoded7_encode(const uint8_t* data, std::size_t size) {
    std::vector<uint8_t> out;
    if (data == nullptr || size == 0) return out;

    // Worst-case: one extra "high-bit" byte for every group of 7 input bytes.
    out.reserve(((size + 6) / 7) * 8);

    std::size_t i = 0;
    while (i < size) {
        const std::size_t group = (size - i < 7) ? (size - i) : 7;

        // High-bit byte: bit (group-1) = high bit of the first body byte,
        // down to bit 0 = high bit of the last body byte. Bits beyond
        // `group` are zero.
        uint8_t hi = 0;
        for (std::size_t k = 0; k < group; ++k) {
            const uint8_t in_byte = data[i + k];
            if (in_byte & 0x80) {
                hi |= static_cast<uint8_t>(1u << (group - 1 - k));
            }
        }
        out.push_back(hi);
        for (std::size_t k = 0; k < group; ++k) {
            out.push_back(static_cast<uint8_t>(data[i + k] & 0x7F));
        }
        i += group;
    }
    return out;
}

std::vector<uint8_t> mcoded7_decode(const uint8_t* data, std::size_t size) {
    std::vector<uint8_t> out;
    if (data == nullptr || size == 0) return out;

    out.reserve((size / 8) * 7 + 7);

    std::size_t i = 0;
    while (i < size) {
        const uint8_t hi = data[i];
        if (hi & 0x80) {
            // Reserved: high-bit byte must itself have bit 7 clear.
            return {};
        }
        const std::size_t remaining = size - i - 1;
        const std::size_t group = (remaining < 7) ? remaining : 7;
        if (group == 0) {
            // Lone high-bit byte at end: malformed.
            return {};
        }
        for (std::size_t k = 0; k < group; ++k) {
            const uint8_t lo = data[i + 1 + k];
            if (lo & 0x80) return {};
            const uint8_t bit = static_cast<uint8_t>(
                (hi >> (group - 1 - k)) & 0x1u);
            out.push_back(static_cast<uint8_t>(lo | (bit ? 0x80 : 0)));
        }
        i += 1 + group;
    }
    return out;
}

}  // namespace pulp::midi
