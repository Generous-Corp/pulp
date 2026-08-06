#include "import_png_codec.hpp"

#include <miniz.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace pulp::import_design {

namespace {

std::uint32_t png_be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

constexpr std::uint8_t kSignature[8] =
    {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

void put_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

/// Append a length-prefixed, CRC-suffixed PNG chunk. The CRC covers the type
/// tag AND the payload, so it is computed over the already-appended bytes
/// rather than over the payload alone.
void put_chunk(std::vector<std::uint8_t>& out, const char (&type)[5],
               const std::uint8_t* data, std::size_t size) {
    put_be32(out, static_cast<std::uint32_t>(size));
    const std::size_t crc_start = out.size();
    out.insert(out.end(), type, type + 4);
    if (size != 0) out.insert(out.end(), data, data + size);
    const auto crc = static_cast<std::uint32_t>(
        mz_crc32(MZ_CRC32_INIT, out.data() + crc_start, 4 + size));
    put_be32(out, crc);
}

}  // namespace

std::pair<int, int> read_png_dimensions(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.good()) return {0, 0};
    std::uint8_t header[24];
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    if (file.gcount() < static_cast<std::streamsize>(sizeof(header)))
        return {0, 0};
    if (std::memcmp(header, kSignature, 8) != 0) return {0, 0};
    const int w = static_cast<int>(png_be32(header + 16));
    const int h = static_cast<int>(png_be32(header + 20));
    if (w <= 0 || h <= 0) return {0, 0};
    return {w, h};
}

ImportPngImage decode_png_rgba(const std::uint8_t* data, std::size_t size) {
    ImportPngImage out;
    if (data == nullptr || size < 33 ||
        std::memcmp(data, kSignature, 8) != 0)
        return out;

    const int width = static_cast<int>(png_be32(data + 16));
    const int height = static_cast<int>(png_be32(data + 20));
    const int bit_depth = data[24];
    const int color_type = data[25];
    const int interlace = data[28];
    if (width <= 0 || height <= 0 || bit_depth != 8 || interlace != 0)
        return out;

    int channels;
    switch (color_type) {
        case 0: channels = 1; break;  // grey
        case 2: channels = 3; break;  // RGB
        case 4: channels = 2; break;  // grey + alpha
        case 6: channels = 4; break;  // RGBA
        default: return out;
    }

    // Concatenate IDAT chunk payloads.
    std::vector<std::uint8_t> idat;
    std::size_t pos = 8;
    while (pos + 8 <= size) {
        const std::uint32_t length = png_be32(data + pos);
        const std::uint8_t* type = data + pos + 4;
        const std::size_t body = pos + 8;
        if (body + length + 4 > size) break;
        if (std::memcmp(type, "IDAT", 4) == 0)
            idat.insert(idat.end(), data + body, data + body + length);
        else if (std::memcmp(type, "IEND", 4) == 0)
            break;
        pos = body + length + 4;  // skip CRC
    }
    if (idat.empty()) return out;

    // Inflate. Raw filtered size = height * (1 + width * channels).
    const std::size_t stride = static_cast<std::size_t>(width) * channels;
    mz_ulong raw_len = static_cast<mz_ulong>(height) * (stride + 1);
    std::vector<std::uint8_t> raw(raw_len);
    if (mz_uncompress(raw.data(), &raw_len, idat.data(),
                      static_cast<mz_ulong>(idat.size())) != MZ_OK)
        return out;
    if (raw_len < static_cast<mz_ulong>(height) * (stride + 1)) return out;

    // Un-filter (PNG filter types 0-4) into a contiguous channel buffer.
    std::vector<std::uint8_t> image(static_cast<std::size_t>(height) * stride);
    auto paeth = [](int a, int b, int c) {
        const int p = a + b - c;
        const int pa = std::abs(p - a);
        const int pb = std::abs(p - b);
        const int pc = std::abs(p - c);
        if (pa <= pb && pa <= pc) return a;
        return pb <= pc ? b : c;
    };
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src =
            raw.data() + static_cast<std::size_t>(y) * (stride + 1);
        const std::uint8_t filter = src[0];
        std::uint8_t* row = image.data() + static_cast<std::size_t>(y) * stride;
        const std::uint8_t* prev =
            (y > 0) ? image.data() + static_cast<std::size_t>(y - 1) * stride
                    : nullptr;
        for (std::size_t x = 0; x < stride; ++x) {
            const int a =
                (x >= static_cast<std::size_t>(channels)) ? row[x - channels] : 0;
            const int b = prev ? prev[x] : 0;
            const int c =
                (prev && x >= static_cast<std::size_t>(channels))
                    ? prev[x - channels] : 0;
            int v = src[1 + x];
            switch (filter) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += paeth(a, b, c); break;
                default: return out;
            }
            row[x] = static_cast<std::uint8_t>(v & 0xFF);
        }
    }

    // Expand to RGBA8.
    out.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (int i = 0; i < width * height; ++i) {
        const std::uint8_t* s = image.data() + static_cast<std::size_t>(i) * channels;
        std::uint8_t* d = out.rgba.data() + static_cast<std::size_t>(i) * 4;
        if (channels == 4) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; }
        else if (channels == 3) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; }
        else if (channels == 2) { d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; }
        else { d[0] = d[1] = d[2] = s[0]; d[3] = 255; }
    }
    out.width = width;
    out.height = height;
    return out;
}

std::vector<std::uint8_t> encode_png_rgba(const ImportPngImage& image) {
    if (!image.valid()) return {};
    const std::size_t stride = static_cast<std::size_t>(image.width) * 4;
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(image.height) * (stride + 1));
    for (int y = 0; y < image.height; ++y) {
        raw.push_back(0);  // filter type: none
        const std::uint8_t* row =
            image.rgba.data() + static_cast<std::size_t>(y) * stride;
        raw.insert(raw.end(), row, row + stride);
    }
    mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::vector<std::uint8_t> deflated(bound);
    if (mz_compress2(deflated.data(), &bound, raw.data(),
                     static_cast<mz_ulong>(raw.size()),
                     MZ_DEFAULT_COMPRESSION) != MZ_OK)
        return {};
    deflated.resize(bound);

    std::vector<std::uint8_t> out(std::begin(kSignature), std::end(kSignature));
    std::vector<std::uint8_t> ihdr;
    put_be32(ihdr, static_cast<std::uint32_t>(image.width));
    put_be32(ihdr, static_cast<std::uint32_t>(image.height));
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(6);  // colour type RGBA
    ihdr.push_back(0);  // compression
    ihdr.push_back(0);  // filter
    ihdr.push_back(0);  // interlace
    put_chunk(out, "IHDR", ihdr.data(), ihdr.size());
    put_chunk(out, "IDAT", deflated.data(), deflated.size());
    put_chunk(out, "IEND", nullptr, 0);
    return out;
}

}  // namespace pulp::import_design
