// Baseline TIFF 6.0 subset reader + writer for pulp::canvas::TiffReader /
// TiffWriter. MIT-clean, no libtiff dependency.
//
// Supported envelope:
//   • Endianness: little- and big-endian byte orders.
//   • Bits per sample: 8.
//   • Samples per pixel: 1 (gray), 3 (RGB), 4 (RGBA).
//   • Photometric Interpretation: 0 (white-is-zero), 1 (black-is-zero),
//     2 (RGB).
//   • Compression: 1 (none), 32773 (PackBits).
//   • Planar configuration: 1 (chunky).
//   • Multi-strip: stitched in StripOffsets order.
//
// The writer always emits uncompressed little-endian RGBA8 single-
// strip TIFFs. Output is interoperable with Preview.app and any
// libtiff-based consumer.

#include <pulp/canvas/image_codecs.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace pulp::canvas {

namespace {

constexpr uint16_t kTagImageWidth        = 256;
constexpr uint16_t kTagImageLength       = 257;
constexpr uint16_t kTagBitsPerSample     = 258;
constexpr uint16_t kTagCompression       = 259;
constexpr uint16_t kTagPhotometric       = 262;
constexpr uint16_t kTagStripOffsets      = 273;
constexpr uint16_t kTagSamplesPerPixel   = 277;
constexpr uint16_t kTagRowsPerStrip      = 278;
constexpr uint16_t kTagStripByteCounts   = 279;
constexpr uint16_t kTagPlanarConfig      = 284;
constexpr uint16_t kTagExtraSamples      = 338;

constexpr uint16_t kTypeShort = 3;
constexpr uint16_t kTypeLong  = 4;

struct ByteOrder {
    bool little = true;

    uint16_t u16(const uint8_t* p) const {
        return little ? static_cast<uint16_t>(p[0] | (p[1] << 8))
                      : static_cast<uint16_t>((p[0] << 8) | p[1]);
    }
    uint32_t u32(const uint8_t* p) const {
        if (little) {
            return static_cast<uint32_t>(p[0])
                 | (static_cast<uint32_t>(p[1]) << 8)
                 | (static_cast<uint32_t>(p[2]) << 16)
                 | (static_cast<uint32_t>(p[3]) << 24);
        }
        return (static_cast<uint32_t>(p[0]) << 24)
             | (static_cast<uint32_t>(p[1]) << 16)
             | (static_cast<uint32_t>(p[2]) << 8)
             | static_cast<uint32_t>(p[3]);
    }
};

struct Ifd {
    uint16_t tag = 0;
    uint16_t type = 0;
    uint32_t count = 0;
    uint32_t value_or_offset = 0;
};

uint32_t ifd_short(const Ifd& e, const ByteOrder& bo) {
    // For type SHORT count 1, the value is stored in the low 2 bytes
    // of the 4-byte field — same bytes for either byte order because
    // we already byte-swapped on read.
    (void)bo;
    return e.value_or_offset & 0xFFFF;
}

bool read_strip(const uint8_t* data, std::size_t size,
                uint32_t offset, uint32_t count,
                uint16_t compression,
                std::vector<uint8_t>& out) {
    if (offset > size || count > size || offset + count > size) return false;
    if (compression == 1) {
        out.insert(out.end(), data + offset, data + offset + count);
        return true;
    }
    if (compression == 32773 /* PackBits */) {
        // PackBits: stream of (n, bytes...) per Apple spec.
        std::size_t p = offset;
        const std::size_t end = offset + count;
        while (p < end) {
            const int8_t n = static_cast<int8_t>(data[p++]);
            if (n >= 0) {
                const std::size_t run = static_cast<std::size_t>(n) + 1;
                if (p + run > end) return false;
                out.insert(out.end(), data + p, data + p + run);
                p += run;
            } else if (n != -128) {
                const std::size_t run = static_cast<std::size_t>(-n) + 1;
                if (p >= end) return false;
                const uint8_t v = data[p++];
                out.insert(out.end(), run, v);
            }
            // n == -128 → no-op.
        }
        return true;
    }
    return false;
}

}  // namespace

bool TiffReader::is_tiff(const uint8_t* data, std::size_t size) {
    if (data == nullptr || size < 8) return false;
    const bool little = (data[0] == 'I' && data[1] == 'I');
    const bool big    = (data[0] == 'M' && data[1] == 'M');
    if (!little && !big) return false;
    ByteOrder bo{little};
    const uint16_t magic = bo.u16(data + 2);
    return magic == 42;
}

std::optional<DecodedRaster> TiffReader::decode(const uint8_t* data,
                                                std::size_t size) {
    if (!is_tiff(data, size)) return std::nullopt;
    ByteOrder bo{data[0] == 'I'};
    const uint32_t ifd_offset = bo.u32(data + 4);
    if (ifd_offset + 2 > size) return std::nullopt;
    const uint16_t entries = bo.u16(data + ifd_offset);
    if (ifd_offset + 2 + entries * 12u > size) return std::nullopt;

    uint32_t width = 0, height = 0;
    uint16_t bits_per_sample = 8;
    uint16_t samples_per_pixel = 1;
    uint16_t photometric = 1;
    uint16_t compression = 1;
    uint16_t planar = 1;
    uint16_t extra_samples = 0;
    std::vector<uint32_t> strip_offsets;
    std::vector<uint32_t> strip_byte_counts;

    auto read_long_array = [&](const Ifd& e, std::vector<uint32_t>& out) {
        out.resize(e.count);
        if (e.count <= 1) {
            out[0] = e.value_or_offset;
            return true;
        }
        const std::size_t bytes = e.count * (e.type == kTypeShort ? 2u : 4u);
        if (e.value_or_offset > size || e.value_or_offset + bytes > size) return false;
        for (uint32_t i = 0; i < e.count; ++i) {
            if (e.type == kTypeShort) {
                out[i] = bo.u16(data + e.value_or_offset + i * 2);
            } else {
                out[i] = bo.u32(data + e.value_or_offset + i * 4);
            }
        }
        return true;
    };

    for (uint16_t i = 0; i < entries; ++i) {
        const uint8_t* entry = data + ifd_offset + 2 + i * 12;
        Ifd e;
        e.tag = bo.u16(entry);
        e.type = bo.u16(entry + 2);
        e.count = bo.u32(entry + 4);
        e.value_or_offset = bo.u32(entry + 8);

        switch (e.tag) {
            case kTagImageWidth:      width = ifd_short(e, bo); if (e.type == kTypeLong) width = e.value_or_offset; break;
            case kTagImageLength:     height = ifd_short(e, bo); if (e.type == kTypeLong) height = e.value_or_offset; break;
            case kTagBitsPerSample: {
                // BitsPerSample is N shorts where N == SamplesPerPixel.
                // We enforce all-channels-equal and 8 bits, so reading
                // the first entry suffices. For count==1 the value
                // lives inline in the entry field; for count > 1 it
                // lives at value_or_offset (since 2*count > 4 the
                // array can't fit inline for count >= 3, and for
                // count == 2 it does fit inline — both cases are
                // handled by reading the first 2 bytes of whichever
                // location is correct).
                if (e.count <= 2) {
                    bits_per_sample = static_cast<uint16_t>(ifd_short(e, bo));
                } else {
                    if (e.value_or_offset + 2 > size) return std::nullopt;
                    bits_per_sample = bo.u16(data + e.value_or_offset);
                }
                break;
            }
            case kTagCompression:     compression = static_cast<uint16_t>(ifd_short(e, bo)); break;
            case kTagPhotometric:     photometric = static_cast<uint16_t>(ifd_short(e, bo)); break;
            case kTagSamplesPerPixel: samples_per_pixel = static_cast<uint16_t>(ifd_short(e, bo)); break;
            case kTagRowsPerStrip:    /* informational only — strip count drives layout */ break;
            case kTagPlanarConfig:    planar = static_cast<uint16_t>(ifd_short(e, bo)); break;
            case kTagExtraSamples:    extra_samples = static_cast<uint16_t>(ifd_short(e, bo)); break;
            case kTagStripOffsets:    if (!read_long_array(e, strip_offsets)) return std::nullopt; break;
            case kTagStripByteCounts: if (!read_long_array(e, strip_byte_counts)) return std::nullopt; break;
            default: break;
        }
    }

    if (width == 0 || height == 0) return std::nullopt;
    if (bits_per_sample != 8) return std::nullopt;
    if (samples_per_pixel != 1 && samples_per_pixel != 3 && samples_per_pixel != 4) {
        return std::nullopt;
    }
    if (planar != 1) return std::nullopt;
    if (compression != 1 && compression != 32773) return std::nullopt;
    if (strip_offsets.empty() || strip_offsets.size() != strip_byte_counts.size()) {
        return std::nullopt;
    }
    (void)extra_samples;  // We assume the 4th sample is alpha when SPP==4.

    std::vector<uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(width) * height * samples_per_pixel);
    for (std::size_t i = 0; i < strip_offsets.size(); ++i) {
        if (!read_strip(data, size, strip_offsets[i], strip_byte_counts[i],
                        compression, raw)) {
            return std::nullopt;
        }
    }

    const std::size_t expected_raw =
        static_cast<std::size_t>(width) * height * samples_per_pixel;
    if (raw.size() < expected_raw) return std::nullopt;
    raw.resize(expected_raw);

    DecodedRaster out;
    out.width = width;
    out.height = height;
    out.rgba.resize(static_cast<std::size_t>(width) * height * 4);

    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        const uint8_t* src = raw.data() + i * samples_per_pixel;
        uint8_t* dst = out.rgba.data() + i * 4;
        if (samples_per_pixel == 1) {
            const uint8_t v = (photometric == 0) ? static_cast<uint8_t>(255 - src[0])
                                                  : src[0];
            dst[0] = v; dst[1] = v; dst[2] = v; dst[3] = 0xFF;
        } else if (samples_per_pixel == 3) {
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 0xFF;
        } else {
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
        }
    }
    return out;
}

// ── TiffWriter ──────────────────────────────────────────────────────────────

namespace {

void put_u16_le(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void put_u32_le(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void write_ifd_entry(std::vector<uint8_t>& out,
                     uint16_t tag, uint16_t type, uint32_t count,
                     uint32_t value_or_offset) {
    put_u16_le(out, tag);
    put_u16_le(out, type);
    put_u32_le(out, count);
    put_u32_le(out, value_or_offset);
}

}  // namespace

std::vector<uint8_t> TiffWriter::encode(const DecodedRaster& raster) {
    if (raster.width == 0 || raster.height == 0) return {};
    const std::size_t pixel_bytes =
        static_cast<std::size_t>(raster.width) * raster.height * 4;
    if (raster.rgba.size() != pixel_bytes) return {};

    // Layout:
    //   [0..7]   Header (II, magic 42, IFD offset)
    //   [8..]    Pixel data
    //   then     IFD (count + entries + next-IFD pointer)
    //   then     BitsPerSample inline (4 shorts)
    //   then     ExtraSamples inline (1 short)
    // BitsPerSample and ExtraSamples values fit/don't fit in the
    // 4-byte entry slot depending on count, so write them as
    // external arrays for cleanliness.
    std::vector<uint8_t> out;
    out.reserve(pixel_bytes + 200);

    // Header.
    out.push_back('I'); out.push_back('I');
    put_u16_le(out, 42);
    put_u32_le(out, 0);  // IFD offset placeholder.

    // Pixel data starts at offset 8.
    const uint32_t strip_offset = static_cast<uint32_t>(out.size());
    out.insert(out.end(), raster.rgba.begin(), raster.rgba.end());

    // BitsPerSample (4 × 8) array.
    const uint32_t bps_offset = static_cast<uint32_t>(out.size());
    put_u16_le(out, 8); put_u16_le(out, 8); put_u16_le(out, 8); put_u16_le(out, 8);

    // IFD.
    const uint32_t ifd_offset = static_cast<uint32_t>(out.size());
    constexpr uint16_t kEntryCount = 10;
    put_u16_le(out, kEntryCount);
    write_ifd_entry(out, kTagImageWidth,      kTypeLong,  1, raster.width);
    write_ifd_entry(out, kTagImageLength,     kTypeLong,  1, raster.height);
    write_ifd_entry(out, kTagBitsPerSample,   kTypeShort, 4, bps_offset);
    write_ifd_entry(out, kTagCompression,     kTypeShort, 1, 1);
    write_ifd_entry(out, kTagPhotometric,     kTypeShort, 1, 2);  // RGB.
    write_ifd_entry(out, kTagStripOffsets,    kTypeLong,  1, strip_offset);
    write_ifd_entry(out, kTagSamplesPerPixel, kTypeShort, 1, 4);
    write_ifd_entry(out, kTagRowsPerStrip,    kTypeLong,  1, raster.height);
    write_ifd_entry(out, kTagStripByteCounts, kTypeLong,  1, static_cast<uint32_t>(pixel_bytes));
    write_ifd_entry(out, kTagExtraSamples,    kTypeShort, 1, 2);  // Unassociated alpha.
    put_u32_le(out, 0);  // Next IFD = none.

    // Patch IFD offset into the header.
    out[4] = static_cast<uint8_t>(ifd_offset & 0xFF);
    out[5] = static_cast<uint8_t>((ifd_offset >> 8) & 0xFF);
    out[6] = static_cast<uint8_t>((ifd_offset >> 16) & 0xFF);
    out[7] = static_cast<uint8_t>((ifd_offset >> 24) & 0xFF);

    return out;
}

}  // namespace pulp::canvas
