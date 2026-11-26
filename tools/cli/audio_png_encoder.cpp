#include "audio_png_encoder.hpp"

#include <pulp/runtime/zip.hpp>

#include <array>
#include <cstddef>

namespace pulp_cli {
namespace {

constexpr std::array<std::uint8_t, 8> kSignature{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k)
            c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    return table;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    static const auto table = make_crc_table();
    std::uint32_t c = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i)
        c = table[(c ^ data[i]) & 0xffu] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
}

// A chunk is length, type, payload, then the CRC of type and payload.
void put_chunk(std::vector<std::uint8_t>& out, const char (&type)[5],
               const std::vector<std::uint8_t>& payload) {
    put_u32(out, static_cast<std::uint32_t>(payload.size()));
    std::vector<std::uint8_t> checked(type, type + 4);
    checked.insert(checked.end(), payload.begin(), payload.end());
    out.insert(out.end(), checked.begin(), checked.end());
    put_u32(out, crc32(checked.data(), checked.size()));
}

} // namespace

std::vector<std::uint8_t> encode_png_rgba8(std::uint32_t width, std::uint32_t height,
                                           const std::vector<std::uint8_t>& rgba,
                                           std::string* error_out) {
    auto fail = [&](const char* reason) {
        if (error_out)
            *error_out = reason;
        return std::vector<std::uint8_t>{};
    };
    if (width == 0 || height == 0)
        return fail("PNG dimensions must be non-zero");
    const std::size_t row = static_cast<std::size_t>(width) * 4u;
    if (rgba.size() != row * height)
        return fail("pixel buffer size does not match the PNG dimensions");

    // Filter type 0 (none) in front of every scanline.
    std::vector<std::uint8_t> raw;
    raw.reserve((row + 1) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        const auto begin = rgba.begin() + static_cast<std::ptrdiff_t>(y * row);
        raw.insert(raw.end(), begin, begin + static_cast<std::ptrdiff_t>(row));
    }
    const auto compressed = pulp::runtime::zlib_compress(raw.data(), raw.size());
    if (!compressed)
        return fail("zlib compression failed");

    std::vector<std::uint8_t> header;
    put_u32(header, width);
    put_u32(header, height);
    header.insert(header.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA, deflate, no filter bank, no interlace

    std::vector<std::uint8_t> png(kSignature.begin(), kSignature.end());
    put_chunk(png, "IHDR", header);
    put_chunk(png, "IDAT", *compressed);
    put_chunk(png, "IEND", {});
    return png;
}

} // namespace pulp_cli
