#pragma once

// Extended image codecs — GIF and TIFF readers/writers.
//
// Skia (the default Pulp decode path via SkImages::DeferredFromEncodedData)
// covers PNG / JPEG / WebP / BMP / ICO in the m144 prebuilt that Pulp pins,
// but does NOT compile in the GIF decoder by default and ships no TIFF
// codec at all. The planning gap-doc row
//   "PNG / JPEG / GIF codecs — GIF/TIFF/SVG/PDF image codecs missing"
// in planning/2026-05-24-reference-framework-gap-analysis.md (P2) calls for
// dedicated reader/writer entry points.
//
// This header provides a Skia-free public surface that:
//   • Decodes GIF87a / GIF89a (single frame + animation frames + LZW + LCT)
//     into RGBA8 pixel buffers using an in-tree decoder. The decoder is
//     MIT-clean (no LZW patent — patent expired in 2004), header-light,
//     and depends only on the C++ standard library.
//   • Decodes Baseline TIFF 6.0 (uncompressed and PackBits compression,
//     8-bit Grayscale / RGB / RGBA, BitsPerSample=8) into RGBA8. Wider
//     TIFF coverage (LZW, JPEG-in-TIFF, multi-strip stitching) would
//     require libtiff which is heavyweight and only ~MIT-compatible
//     under its custom permissive licence; not pulled in for this slice.
//   • Encodes uncompressed Baseline TIFF (RGB / RGBA) and a static
//     GIF89a (single global palette, 256-colour quantisation via simple
//     median-cut). Writer paths are intended for editor / asset export
//     use; not optimised for runtime hot paths.
//
// The codec functions return std::optional<DecodedRaster> on success and
// std::nullopt on any malformed input — no exceptions thrown, callers
// can degrade gracefully (fall back to Skia, or report unsupported).
//
// Animation: GifReader::decode_first() decodes a single frame for
// callers that only want the cover image (matches existing
// SkImages::DeferredFromEncodedData behaviour). GifReader::decode_all()
// returns every frame in order plus per-frame delays (centi-seconds
// per the GIF89a spec) for animated UI elements.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pulp::canvas {

/// A decoded raster image. Pixels are RGBA8 in scanline order
/// (row-major, top-down, no row padding — stride == width * 4).
struct DecodedRaster {
    uint32_t width = 0;
    uint32_t height = 0;
    /// Tightly packed RGBA8. .size() == width * height * 4.
    std::vector<uint8_t> rgba;
};

struct GifFrame {
    DecodedRaster raster;
    /// Frame delay in milliseconds (the GIF spec uses centi-seconds;
    /// the codec converts so callers can hand the value directly to
    /// std::chrono::milliseconds).
    uint32_t delay_ms = 0;
};

/// GIF reader. Decodes GIF87a + GIF89a streams.
class GifReader {
public:
    /// Decode just the first frame of the GIF. Suitable for
    /// non-animated image-view consumers that want a single cover.
    /// Returns nullopt for malformed input.
    static std::optional<DecodedRaster> decode_first(const uint8_t* data,
                                                     std::size_t size);

    /// Decode every frame in order. delay_ms is filled from the
    /// Graphic Control Extension when present; absent values default
    /// to 100 ms to match common browser behaviour.
    static std::optional<std::vector<GifFrame>> decode_all(
        const uint8_t* data, std::size_t size);

    /// Quick probe — does the byte stream begin with a GIF87a/89a
    /// signature? Used by the host's encoded-format dispatcher to
    /// route to this reader before invoking the (more expensive)
    /// full decoder.
    static bool is_gif(const uint8_t* data, std::size_t size);
};

/// GIF writer. Encodes a single still frame as GIF89a with a
/// global colour table built via median-cut quantisation. Useful
/// for editor exports and tests; runtime callers that want
/// animation should compose multiple frames via a higher-level
/// helper (not provided in this slice).
class GifWriter {
public:
    /// Encode the raster as a standalone GIF89a file. Returns the
    /// encoded bytes; empty vector on failure (zero width/height
    /// or pixel count mismatch).
    static std::vector<uint8_t> encode_still(const DecodedRaster& raster);
};

/// TIFF reader. Decodes a Baseline TIFF 6.0 subset:
///   • Endianness: both little-endian (II) and big-endian (MM).
///   • Bits per sample: 8.
///   • Samples per pixel: 1 (grayscale), 3 (RGB), 4 (RGBA).
///   • Compression: 1 (none) and 32773 (PackBits).
///   • Planar config: 1 (chunky).
///   • Single strip OR multiple strips assembled in StripOffsets order.
/// Returns nullopt for anything outside that envelope. Callers that
/// need broader coverage should integrate libtiff at the application
/// layer (Pulp does not bundle libtiff to keep the dependency
/// footprint small — see planning gap-doc for rationale).
class TiffReader {
public:
    static std::optional<DecodedRaster> decode(const uint8_t* data,
                                               std::size_t size);

    static bool is_tiff(const uint8_t* data, std::size_t size);
};

/// TIFF writer. Encodes an RGBA8 raster as an uncompressed Baseline
/// TIFF 6.0 (single strip, little-endian, RGB+alpha samples). The
/// output is interoperable with standard viewers (Preview.app,
/// ImageMagick, libtiff readers).
class TiffWriter {
public:
    static std::vector<uint8_t> encode(const DecodedRaster& raster);
};

}  // namespace pulp::canvas
