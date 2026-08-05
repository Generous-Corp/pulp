// Decoder-independent PNG transport validation.

#include <pulp/view/screenshot_compare.hpp>

#include <pulp/runtime/zip.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace pulp::view {

static constexpr uint64_t kMaxDecodedPixels = 8192ull * 8192ull;
static constexpr uint64_t kMaxValidatedPngBytes = 256ull * 1024ull * 1024ull;
static constexpr std::array<uint32_t, 7> kAdam7XStart{0, 4, 0, 2, 0, 1, 0};
static constexpr std::array<uint32_t, 7> kAdam7YStart{0, 0, 4, 0, 2, 0, 1};
static constexpr std::array<uint32_t, 7> kAdam7XStep{8, 8, 4, 4, 2, 2, 1};
static constexpr std::array<uint32_t, 7> kAdam7YStep{8, 8, 8, 4, 4, 2, 2};

static uint32_t png_crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1u) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static int png_channels(uint8_t bit_depth, uint8_t color_type) {
    switch (color_type) {
    case 0:
        return (bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8 ||
                bit_depth == 16)
                   ? 1
                   : 0;
    case 2:
        return (bit_depth == 8 || bit_depth == 16) ? 3 : 0;
    case 3:
        return (bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8) ? 1 : 0;
    case 4:
        return (bit_depth == 8 || bit_depth == 16) ? 2 : 0;
    case 6:
        return (bit_depth == 8 || bit_depth == 16) ? 4 : 0;
    default:
        return 0;
    }
}

static std::optional<size_t> png_scanline_bytes(uint32_t width, uint32_t height,
                                                uint8_t bits_per_pixel, uint8_t interlace) {
    uint64_t expected = 0;
    const size_t pass_count = interlace == 0 ? 1 : kAdam7XStart.size();
    for (size_t pass = 0; pass < pass_count; ++pass) {
        const uint32_t start_x = interlace == 0 ? 0 : kAdam7XStart[pass];
        const uint32_t start_y = interlace == 0 ? 0 : kAdam7YStart[pass];
        const uint32_t step_x = interlace == 0 ? 1 : kAdam7XStep[pass];
        const uint32_t step_y = interlace == 0 ? 1 : kAdam7YStep[pass];
        const uint64_t pass_width =
            width <= start_x ? 0 : (static_cast<uint64_t>(width - start_x) + step_x - 1) / step_x;
        const uint64_t pass_height =
            height <= start_y ? 0 : (static_cast<uint64_t>(height - start_y) + step_y - 1) / step_y;
        if (pass_width == 0 || pass_height == 0)
            continue;
        const uint64_t row_bytes = (pass_width * bits_per_pixel + 7u) / 8u;
        const uint64_t pass_bytes = pass_height * (row_bytes + 1u);
        if (pass_bytes > kMaxValidatedPngBytes - expected)
            return std::nullopt;
        expected += pass_bytes;
    }
    return static_cast<size_t>(expected);
}

static bool validate_png_scanlines(const std::vector<uint8_t>& raw, uint32_t width, uint32_t height,
                                   uint8_t bits_per_pixel, uint8_t interlace) {
    const auto expected = png_scanline_bytes(width, height, bits_per_pixel, interlace);
    if (!expected || *expected != raw.size())
        return false;
    size_t raw_offset = 0;
    const size_t pass_count = interlace == 0 ? 1 : kAdam7XStart.size();
    for (size_t pass = 0; pass < pass_count; ++pass) {
        const uint32_t start_x = interlace == 0 ? 0 : kAdam7XStart[pass];
        const uint32_t start_y = interlace == 0 ? 0 : kAdam7YStart[pass];
        const uint32_t step_x = interlace == 0 ? 1 : kAdam7XStep[pass];
        const uint32_t step_y = interlace == 0 ? 1 : kAdam7YStep[pass];
        const uint64_t pass_width =
            width <= start_x ? 0 : (static_cast<uint64_t>(width - start_x) + step_x - 1) / step_x;
        const uint64_t pass_height =
            height <= start_y ? 0 : (static_cast<uint64_t>(height - start_y) + step_y - 1) / step_y;
        if (pass_width == 0 || pass_height == 0)
            continue;
        const uint64_t row_bytes = (pass_width * bits_per_pixel + 7u) / 8u;
        for (uint64_t row = 0; row < pass_height; ++row) {
            if (raw_offset >= raw.size() || raw[raw_offset] > 4)
                return false;
            raw_offset += static_cast<size_t>(row_bytes + 1u);
        }
    }
    return raw_offset == raw.size();
}

PngMetadata inspect_png_metadata(const std::vector<uint8_t>& png) {
    constexpr uint8_t signature[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (png.size() < 8 || !std::equal(std::begin(signature), std::end(signature), png.begin()))
        return {};

    const auto read_be32 = [&png](size_t offset) {
        return (static_cast<uint32_t>(png[offset]) << 24u) |
               (static_cast<uint32_t>(png[offset + 1]) << 16u) |
               (static_cast<uint32_t>(png[offset + 2]) << 8u) |
               static_cast<uint32_t>(png[offset + 3]);
    };
    const auto chunk_is = [&png](size_t offset, const char (&type)[5]) {
        return std::equal(png.begin() + static_cast<std::ptrdiff_t>(offset + 4),
                          png.begin() + static_cast<std::ptrdiff_t>(offset + 8), type);
    };

    PngMetadata metadata;
    bool saw_ihdr = false;
    bool saw_idat = false;
    bool idat_closed = false;
    bool saw_plte = false;
    uint8_t bit_depth = 0;
    uint8_t color_type = 0;
    uint8_t bits_per_pixel = 0;
    uint8_t interlace = 0;
    std::vector<uint8_t> idat;
    size_t offset = 8;
    while (offset <= png.size() && png.size() - offset >= 12) {
        const auto length = static_cast<uint64_t>(read_be32(offset));
        const auto available = static_cast<uint64_t>(png.size() - offset - 12);
        if (length > available)
            return {};
        const auto next = offset + 12 + static_cast<size_t>(length);
        const auto stored_crc = read_be32(offset + 8 + static_cast<size_t>(length));
        if (png_crc32(png.data() + offset + 4, static_cast<size_t>(length) + 4) != stored_crc) {
            return {};
        }
        for (size_t type_index = offset + 4; type_index < offset + 8; ++type_index) {
            const auto byte = png[type_index];
            if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')))
                return {};
        }
        if ((png[offset + 6] & 0x20u) != 0) // PNG's reserved chunk-name bit.
            return {};
        const bool is_ihdr = chunk_is(offset, "IHDR");
        const bool is_plte = chunk_is(offset, "PLTE");
        const bool is_idat = chunk_is(offset, "IDAT");
        const bool is_iend = chunk_is(offset, "IEND");
        const bool is_unknown_critical =
            (png[offset + 4] & 0x20u) == 0 && !is_ihdr && !is_plte && !is_idat && !is_iend;
        if (is_unknown_critical)
            return {};
        if (saw_idat && !is_idat && !is_iend)
            idat_closed = true;

        if (!saw_ihdr) {
            if (!is_ihdr || length != 13)
                return {};
            metadata.width = read_be32(offset + 8);
            metadata.height = read_be32(offset + 12);
            bit_depth = png[offset + 16];
            color_type = png[offset + 17];
            const auto channels = png_channels(bit_depth, color_type);
            interlace = png[offset + 20];
            const auto pixels = static_cast<uint64_t>(metadata.width) * metadata.height;
            if (metadata.width == 0 || metadata.height == 0 || pixels > kMaxDecodedPixels ||
                channels == 0 || png[offset + 18] != 0 || png[offset + 19] != 0 || interlace > 1) {
                return {};
            }
            bits_per_pixel = static_cast<uint8_t>(bit_depth * channels);
            saw_ihdr = true;
        } else if (is_ihdr) {
            return {};
        } else if (is_plte) {
            if (saw_plte || saw_idat || color_type == 0 || color_type == 4 || length == 0 ||
                length > 768 || length % 3 != 0) {
                return {};
            }
            const auto entries = length / 3;
            if (color_type == 3 && entries > (uint64_t{1} << bit_depth))
                return {};
            saw_plte = true;
        } else if (is_idat) {
            if (idat_closed || (color_type == 3 && !saw_plte))
                return {};
            saw_idat = true;
            if (length > kMaxValidatedPngBytes - idat.size())
                return {};
            idat.insert(idat.end(), png.begin() + static_cast<std::ptrdiff_t>(offset + 8),
                        png.begin() + static_cast<std::ptrdiff_t>(offset + 8 + length));
        } else if (is_iend) {
            if (length != 0 || !saw_idat || (color_type == 3 && !saw_plte) || next != png.size()) {
                return {};
            }
            const auto expected =
                png_scanline_bytes(metadata.width, metadata.height, bits_per_pixel, interlace);
            if (!expected)
                return {};
            const auto raw = pulp::runtime::zlib_decompress(idat.data(), idat.size(), *expected);
            if (!raw || !validate_png_scanlines(*raw, metadata.width, metadata.height,
                                                bits_per_pixel, interlace)) {
                return {};
            }
            metadata.valid = true;
            return metadata;
        }
        offset = next;
    }
    return {};
}

} // namespace pulp::view
