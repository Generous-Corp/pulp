#include <pulp/canvas/image_probe.hpp>

#ifdef PULP_HAS_SKIA
#include "include/codec/SkCodec.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#endif

namespace pulp::canvas {

#ifdef PULP_HAS_SKIA
namespace {

std::uint32_t read_be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Skia's PNG decoder fills rows it never received with transparent pixels and
// still reports success, so a truncated file "decodes" and paints blank. Walk
// the chunk list instead: every chunk whole, every CRC right, IEND reached.
bool png_structure_intact(const std::uint8_t* p, std::size_t size) {
    static constexpr std::uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (size < 8 || std::memcmp(p, kSignature, 8) != 0) return true;  // not a PNG
    std::size_t at = 8;
    while (at + 12 <= size) {
        const std::uint32_t length = read_be32(p + at);
        if (length > size - at - 12) return false;
        const std::uint8_t* type = p + at + 4;
        if (crc32(type, 4 + std::size_t(length)) != read_be32(type + 4 + length)) return false;
        if (std::memcmp(type, "IEND", 4) == 0) return true;
        at += 12 + std::size_t(length);
    }
    return false;
}

}  // namespace
#endif

ImageFileProbe probe_image_file(const std::string& path) {
#ifdef PULP_HAS_SKIA
    if (path.empty()) return ImageFileProbe::undecodable;
    auto data = SkData::MakeFromFileName(path.c_str());
    if (!data) return ImageFileProbe::undecodable;
    if (!png_structure_intact(data->bytes(), data->size())) return ImageFileProbe::undecodable;
    auto codec = SkCodec::MakeFromData(data);
    if (!codec) return ImageFileProbe::undecodable;
    auto [image, result] = codec->getImage();
    return image && result == SkCodec::kSuccess ? ImageFileProbe::decodes
                                                : ImageFileProbe::undecodable;
#else
    (void)path;
    return ImageFileProbe::unavailable;
#endif
}

}  // namespace pulp::canvas
