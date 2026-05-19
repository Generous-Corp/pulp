#include <pulp/runtime/base64.hpp>

namespace pulp::runtime {

static constexpr char kEncodeTable[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static constexpr uint8_t kDecodeTable[] = {
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255, 62,255,255,255, 63,
     52, 53, 54, 55, 56, 57, 58, 59, 60, 61,255,255,255,  0,255,255,
    255,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
     15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,255,255,255,255,255,
    255, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
     41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,255,255,255,255,255,
};

std::string base64_encode(const uint8_t* data, size_t length) {
    if (data == nullptr || length == 0)
        return {};

    std::string result;
    result.reserve(((length + 2) / 3) * 4);

    size_t i = 0;
    while (i + 2 < length) {
        uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                          (static_cast<uint32_t>(data[i + 1]) << 8) |
                          static_cast<uint32_t>(data[i + 2]);
        result += kEncodeTable[(triple >> 18) & 0x3F];
        result += kEncodeTable[(triple >> 12) & 0x3F];
        result += kEncodeTable[(triple >> 6) & 0x3F];
        result += kEncodeTable[triple & 0x3F];
        i += 3;
    }

    if (i + 1 == length) {
        uint32_t val = static_cast<uint32_t>(data[i]) << 16;
        result += kEncodeTable[(val >> 18) & 0x3F];
        result += kEncodeTable[(val >> 12) & 0x3F];
        result += '=';
        result += '=';
    } else if (i + 2 == length) {
        uint32_t val = (static_cast<uint32_t>(data[i]) << 16) |
                       (static_cast<uint32_t>(data[i + 1]) << 8);
        result += kEncodeTable[(val >> 18) & 0x3F];
        result += kEncodeTable[(val >> 12) & 0x3F];
        result += kEncodeTable[(val >> 6) & 0x3F];
        result += '=';
    }

    return result;
}

std::string base64_encode(std::string_view input) {
    return base64_encode(reinterpret_cast<const uint8_t*>(input.data()), input.size());
}

std::optional<std::vector<uint8_t>> base64_decode(std::string_view input) {
    // Strip whitespace while validating that padding, if present, is terminal.
    std::string clean;
    clean.reserve(input.size());
    bool saw_padding = false;
    size_t padding_count = 0;
    for (char c : input) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
            continue;
        if (c == '=') {
            saw_padding = true;
            ++padding_count;
            if (padding_count > 2)
                return std::nullopt;
            clean += c;
            continue;
        }
        if (saw_padding)
            return std::nullopt;
        if (static_cast<unsigned char>(c) >= 128 || kDecodeTable[static_cast<unsigned char>(c)] == 255)
            return std::nullopt;
        clean += c;
    }

    if (clean.size() % 4 == 1)
        return std::nullopt;
    if (padding_count > 0) {
        if (clean.size() % 4 != 0 || padding_count > clean.size())
            return std::nullopt;
        clean.resize(clean.size() - padding_count);
    }

    std::vector<uint8_t> result;
    result.reserve((clean.size() * 3) / 4);

    size_t i = 0;
    while (i + 3 < clean.size()) {
        uint32_t val = (kDecodeTable[static_cast<unsigned char>(clean[i])] << 18) |
                       (kDecodeTable[static_cast<unsigned char>(clean[i + 1])] << 12) |
                       (kDecodeTable[static_cast<unsigned char>(clean[i + 2])] << 6) |
                       kDecodeTable[static_cast<unsigned char>(clean[i + 3])];
        result.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
        result.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        result.push_back(static_cast<uint8_t>(val & 0xFF));
        i += 4;
    }

    if (i + 1 < clean.size()) {
        uint32_t val = (kDecodeTable[static_cast<unsigned char>(clean[i])] << 18) |
                       (kDecodeTable[static_cast<unsigned char>(clean[i + 1])] << 12);
        result.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
        if (i + 2 < clean.size()) {
            val |= kDecodeTable[static_cast<unsigned char>(clean[i + 2])] << 6;
            result.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        }
    }

    return result;
}

}  // namespace pulp::runtime
