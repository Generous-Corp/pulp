#include <pulp/view/screenshot_compare.hpp>
#include <pulp/runtime/zip.hpp>
#include <fstream>
#include <array>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#elif defined(PULP_HAS_SKIA)
#include "include/codec/SkPngDecoder.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/encode/SkPngEncoder.h"
#endif

namespace pulp::view {

// ── PNG decoding (platform-specific) ────────────────────────────────────

struct RawImage {
    std::vector<uint8_t> pixels;  // RGBA, 4 bytes per pixel
    uint32_t width = 0;
    uint32_t height = 0;
};

static constexpr uint64_t kMaxDecodedPixels = 8192ull * 8192ull;
static constexpr uint32_t kMaxTrackedContentColors = 65536u;
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
    case 0: return (bit_depth == 1 || bit_depth == 2 || bit_depth == 4
                    || bit_depth == 8 || bit_depth == 16) ? 1 : 0;
    case 2: return (bit_depth == 8 || bit_depth == 16) ? 3 : 0;
    case 3: return (bit_depth == 1 || bit_depth == 2 || bit_depth == 4
                    || bit_depth == 8) ? 1 : 0;
    case 4: return (bit_depth == 8 || bit_depth == 16) ? 2 : 0;
    case 6: return (bit_depth == 8 || bit_depth == 16) ? 4 : 0;
    default: return 0;
    }
}

static std::optional<size_t> png_scanline_bytes(uint32_t width, uint32_t height,
                                                uint8_t bits_per_pixel,
                                                uint8_t interlace) {
    uint64_t expected = 0;
    const size_t pass_count = interlace == 0 ? 1 : kAdam7XStart.size();
    for (size_t pass = 0; pass < pass_count; ++pass) {
        const uint32_t start_x = interlace == 0 ? 0 : kAdam7XStart[pass];
        const uint32_t start_y = interlace == 0 ? 0 : kAdam7YStart[pass];
        const uint32_t step_x = interlace == 0 ? 1 : kAdam7XStep[pass];
        const uint32_t step_y = interlace == 0 ? 1 : kAdam7YStep[pass];
        const uint64_t pass_width = width <= start_x
            ? 0 : (static_cast<uint64_t>(width - start_x) + step_x - 1) / step_x;
        const uint64_t pass_height = height <= start_y
            ? 0 : (static_cast<uint64_t>(height - start_y) + step_y - 1) / step_y;
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

static bool validate_png_scanlines(const std::vector<uint8_t>& raw, uint32_t width,
                                   uint32_t height, uint8_t bits_per_pixel,
                                   uint8_t interlace) {
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
        const uint64_t pass_width = width <= start_x
            ? 0 : (static_cast<uint64_t>(width - start_x) + step_x - 1) / step_x;
        const uint64_t pass_height = height <= start_y
            ? 0 : (static_cast<uint64_t>(height - start_y) + step_y - 1) / step_y;
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
        return (static_cast<uint32_t>(png[offset]) << 24u)
             | (static_cast<uint32_t>(png[offset + 1]) << 16u)
             | (static_cast<uint32_t>(png[offset + 2]) << 8u)
             | static_cast<uint32_t>(png[offset + 3]);
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
        if (png_crc32(png.data() + offset + 4, static_cast<size_t>(length) + 4)
            != stored_crc) {
            return {};
        }
        for (size_t type_index = offset + 4; type_index < offset + 8; ++type_index) {
            const auto byte = png[type_index];
            if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')))
                return {};
        }
        if ((png[offset + 6] & 0x20u) != 0)  // PNG's reserved chunk-name bit.
            return {};
        const bool is_ihdr = chunk_is(offset, "IHDR");
        const bool is_plte = chunk_is(offset, "PLTE");
        const bool is_idat = chunk_is(offset, "IDAT");
        const bool is_iend = chunk_is(offset, "IEND");
        const bool is_unknown_critical = (png[offset + 4] & 0x20u) == 0
            && !is_ihdr && !is_plte && !is_idat && !is_iend;
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
            if (metadata.width == 0 || metadata.height == 0 || pixels > kMaxDecodedPixels
                || channels == 0 || png[offset + 18] != 0 || png[offset + 19] != 0
                || interlace > 1) {
                return {};
            }
            bits_per_pixel = static_cast<uint8_t>(bit_depth * channels);
            saw_ihdr = true;
        } else if (is_ihdr) {
            return {};
        } else if (is_plte) {
            if (saw_plte || saw_idat || color_type == 0 || color_type == 4
                || length == 0 || length > 768 || length % 3 != 0) {
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
            if (length != 0 || !saw_idat || (color_type == 3 && !saw_plte)
                || next != png.size()) {
                return {};
            }
            const auto expected = png_scanline_bytes(metadata.width, metadata.height,
                                                     bits_per_pixel, interlace);
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

#ifdef __APPLE__
static constexpr CGBitmapInfo kRgbaBitmapInfo =
    static_cast<CGBitmapInfo>(static_cast<uint32_t>(kCGBitmapByteOrder32Big) |
                              static_cast<uint32_t>(kCGImageAlphaPremultipliedLast));

static RawImage decode_png(const std::vector<uint8_t>& png_data) {
    RawImage img;
    if (png_data.empty()) return img;

    CFDataRef data = CFDataCreate(nullptr, png_data.data(),
                                  static_cast<CFIndex>(png_data.size()));
    if (!data) return img;

    CGImageSourceRef source = CGImageSourceCreateWithData(data, nullptr);
    CFRelease(data);
    if (!source) return img;

    CGImageRef cgImage = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    CFRelease(source);
    if (!cgImage) return img;

    const auto width = static_cast<uint32_t>(CGImageGetWidth(cgImage));
    const auto height = static_cast<uint32_t>(CGImageGetHeight(cgImage));
    const auto pixel_count = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (width == 0 || height == 0 || pixel_count > kMaxDecodedPixels ||
        pixel_count > (std::numeric_limits<size_t>::max() / 4u)) {
        CGImageRelease(cgImage);
        return img;
    }

    img.width = width;
    img.height = height;
    img.pixels.resize(static_cast<size_t>(pixel_count) * 4u);

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(
        img.pixels.data(), img.width, img.height, 8, static_cast<size_t>(img.width) * 4u,
        cs, kRgbaBitmapInfo);
    CGColorSpaceRelease(cs);

    if (ctx) {
        CGContextDrawImage(ctx, CGRectMake(0, 0, img.width, img.height), cgImage);
        CGContextRelease(ctx);
    } else {
        img = {};
    }

    CGImageRelease(cgImage);
    return img;
}
#elif defined(PULP_HAS_SKIA)
static RawImage decode_png(const std::vector<uint8_t>& png_data) {
    RawImage img;
    if (png_data.empty()) return img;

    SkCodec::Result decode_result = SkCodec::kInvalidInput;
    auto codec = SkPngDecoder::Decode(SkData::MakeWithCopy(png_data.data(), png_data.size()),
                                      &decode_result);
    if (!codec || decode_result != SkCodec::kSuccess) return img;

    const auto encoded_info = codec->getInfo();
    const auto width = encoded_info.width();
    const auto height = encoded_info.height();
    const auto pixel_count = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (width <= 0 || height <= 0 || pixel_count > kMaxDecodedPixels ||
        pixel_count > (std::numeric_limits<size_t>::max() / 4u)) {
        return img;
    }

    img.width = static_cast<uint32_t>(width);
    img.height = static_cast<uint32_t>(height);
    img.pixels.resize(static_cast<size_t>(pixel_count) * 4u);

    SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    SkPixmap pixmap(info, img.pixels.data(), static_cast<size_t>(img.width) * 4u);
    if (codec->getPixels(pixmap) != SkCodec::kSuccess) {
        img = {};
    }
    return img;
}
#else
static RawImage decode_png(const std::vector<uint8_t>&) {
    return {};  // PNG decoding not available on this platform
}
#endif

#ifdef __APPLE__
static std::vector<uint8_t> encode_png_rgba(const uint8_t* pixels, uint32_t width, uint32_t height) {
    if (!pixels || width == 0 || height == 0) return {};

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(
        const_cast<uint8_t*>(pixels), width, height, 8, static_cast<size_t>(width) * 4u,
        cs, kRgbaBitmapInfo);
    CGColorSpaceRelease(cs);
    if (!ctx) return {};

    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!img) return {};

    CFMutableDataRef cf_data = CFDataCreateMutable(nullptr, 0);
    if (!cf_data) { CGImageRelease(img); return {}; }

    CGImageDestinationRef dest = CGImageDestinationCreateWithData(cf_data, CFSTR("public.png"), 1, nullptr);
    if (!dest) {
        CGImageRelease(img);
        CFRelease(cf_data);
        return {};
    }

    CGImageDestinationAddImage(dest, img, nullptr);
    const bool ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    CGImageRelease(img);
    if (!ok) {
        CFRelease(cf_data);
        return {};
    }

    auto len = static_cast<size_t>(CFDataGetLength(cf_data));
    std::vector<uint8_t> result(len);
    std::memcpy(result.data(), CFDataGetBytePtr(cf_data), len);
    CFRelease(cf_data);
    return result;
}
#elif defined(PULP_HAS_SKIA)
static std::vector<uint8_t> encode_png_rgba(const uint8_t* pixels, uint32_t width, uint32_t height) {
    if (!pixels || width == 0 || height == 0) return {};

    SkImageInfo info = SkImageInfo::Make(static_cast<int>(width),
                                         static_cast<int>(height),
                                         kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    SkPixmap pixmap(info, pixels, static_cast<size_t>(width) * 4u);
    sk_sp<SkData> png = SkPngEncoder::Encode(pixmap, SkPngEncoder::Options{});
    if (!png || png->isEmpty()) return {};

    const auto* bytes = static_cast<const uint8_t*>(png->data());
    return std::vector<uint8_t>(bytes, bytes + png->size());
}
#endif

static std::vector<uint8_t> read_file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    return {std::istreambuf_iterator<char>(f), {}};
}

// ── Comparison ──────────────────────────────────────────────────────────

CompareResult compare_screenshots(
    const std::vector<uint8_t>& reference_png,
    const std::vector<uint8_t>& rendered_png,
    uint8_t tolerance)
{
    CompareResult result;

    auto ref = decode_png(reference_png);
    auto ren = decode_png(rendered_png);

    if (ref.pixels.empty()) {
        result.error = "Failed to decode reference image";
        return result;
    }
    if (ren.pixels.empty()) {
        result.error = "Failed to decode rendered image";
        return result;
    }

    // Compare at the smaller of the two dimensions
    uint32_t cmp_w = std::min(ref.width, ren.width);
    uint32_t cmp_h = std::min(ref.height, ren.height);
    result.total_pixels = cmp_w * cmp_h;

    if (result.total_pixels == 0) {
        result.error = "Zero-size comparison area";
        return result;
    }

    double total_error = 0;
    uint32_t diff_count = 0;

    for (uint32_t y = 0; y < cmp_h; ++y) {
        for (uint32_t x = 0; x < cmp_w; ++x) {
            size_t ref_idx = (y * ref.width + x) * 4;
            size_t ren_idx = (y * ren.width + x) * 4;

            int dr = std::abs(static_cast<int>(ref.pixels[ref_idx])     - static_cast<int>(ren.pixels[ren_idx]));
            int dg = std::abs(static_cast<int>(ref.pixels[ref_idx + 1]) - static_cast<int>(ren.pixels[ren_idx + 1]));
            int db = std::abs(static_cast<int>(ref.pixels[ref_idx + 2]) - static_cast<int>(ren.pixels[ren_idx + 2]));

            float pixel_error = (dr + dg + db) / 3.0f;
            total_error += pixel_error;

            if (dr > tolerance || dg > tolerance || db > tolerance)
                diff_count++;
        }
    }

    result.valid = true;
    result.diff_pixels = diff_count;
    result.mean_error = static_cast<float>(total_error / result.total_pixels);
    result.similarity = 1.0f - static_cast<float>(diff_count) / static_cast<float>(result.total_pixels);

    // Penalize size mismatch
    if (ref.width != ren.width || ref.height != ren.height) {
        float size_ratio = static_cast<float>(cmp_w * cmp_h) /
                           static_cast<float>(std::max(ref.width * ref.height, ren.width * ren.height));
        result.similarity *= size_ratio;
    }

    return result;
}

CompareResult compare_screenshot_files(
    const std::string& reference_path,
    const std::string& rendered_path,
    uint8_t tolerance)
{
    auto ref = read_file_bytes(reference_path);
    if (ref.empty()) {
        CompareResult r;
        r.error = "Cannot read reference file: " + reference_path;
        return r;
    }
    auto ren = read_file_bytes(rendered_path);
    if (ren.empty()) {
        CompareResult r;
        r.error = "Cannot read rendered file: " + rendered_path;
        return r;
    }
    return compare_screenshots(ref, ren, tolerance);
}

std::vector<uint8_t> generate_diff_image(
    const std::vector<uint8_t>& reference_png,
    const std::vector<uint8_t>& rendered_png,
    uint8_t tolerance)
{
    auto ref = decode_png(reference_png);
    auto ren = decode_png(rendered_png);

    if (ref.pixels.empty() || ren.pixels.empty()) return {};

    uint32_t out_w = std::max(ref.width, ren.width);
    uint32_t out_h = std::max(ref.height, ren.height);
    uint32_t cmp_w = std::min(ref.width, ren.width);
    uint32_t cmp_h = std::min(ref.height, ren.height);

    // Build diff RGBA buffer
    const auto out_pixels = static_cast<uint64_t>(out_w) * static_cast<uint64_t>(out_h);
    if (out_pixels == 0 || out_pixels > kMaxDecodedPixels ||
        out_pixels > (std::numeric_limits<size_t>::max() / 4u)) {
        return {};
    }
    std::vector<uint8_t> diff(static_cast<size_t>(out_pixels) * 4u, 0);

    for (uint32_t y = 0; y < out_h; ++y) {
        for (uint32_t x = 0; x < out_w; ++x) {
            const size_t out_idx =
                (static_cast<size_t>(y) * static_cast<size_t>(out_w) +
                 static_cast<size_t>(x)) * 4u;

            if (x >= cmp_w || y >= cmp_h) {
                // Outside overlap: magenta = size mismatch
                diff[out_idx] = 255; diff[out_idx+1] = 0;
                diff[out_idx+2] = 255; diff[out_idx+3] = 255;
                continue;
            }

            const size_t ref_idx =
                (static_cast<size_t>(y) * static_cast<size_t>(ref.width) +
                 static_cast<size_t>(x)) * 4u;
            const size_t ren_idx =
                (static_cast<size_t>(y) * static_cast<size_t>(ren.width) +
                 static_cast<size_t>(x)) * 4u;

            int dr = std::abs(static_cast<int>(ref.pixels[ref_idx])     - static_cast<int>(ren.pixels[ren_idx]));
            int dg = std::abs(static_cast<int>(ref.pixels[ref_idx + 1]) - static_cast<int>(ren.pixels[ren_idx + 1]));
            int db = std::abs(static_cast<int>(ref.pixels[ref_idx + 2]) - static_cast<int>(ren.pixels[ren_idx + 2]));

            if (dr > tolerance || dg > tolerance || db > tolerance) {
                // Difference: red highlight
                diff[out_idx] = 255;
                diff[out_idx+1] = static_cast<uint8_t>(std::min(80 + dr, 255));
                diff[out_idx+2] = static_cast<uint8_t>(std::min(80 + db, 255));
                diff[out_idx+3] = 255;
            } else {
                // Match: dimmed reference
                diff[out_idx]   = ref.pixels[ref_idx] / 3;
                diff[out_idx+1] = ref.pixels[ref_idx+1] / 3;
                diff[out_idx+2] = ref.pixels[ref_idx+2] / 3;
                diff[out_idx+3] = 255;
            }
        }
    }

    // Encode diff to PNG using the platform decoder's matching encoder.
#if defined(__APPLE__) || defined(PULP_HAS_SKIA)
    return encode_png_rgba(diff.data(), out_w, out_h);
#else
    return {};
#endif
}

std::vector<uint8_t> crop_png(
    const std::vector<uint8_t>& png,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height)
{
    auto img = decode_png(png);
    if (img.pixels.empty() || width == 0 || height == 0) return {};

    if (x >= img.width || y >= img.height) return {};

    uint32_t crop_w = std::min(width, img.width - x);
    uint32_t crop_h = std::min(height, img.height - y);
    if (crop_w == 0 || crop_h == 0) return {};

    std::vector<uint8_t> cropped(crop_w * crop_h * 4);
    for (uint32_t row = 0; row < crop_h; ++row) {
        const auto* src = img.pixels.data() + ((y + row) * img.width + x) * 4;
        auto* dst = cropped.data() + (row * crop_w) * 4;
        std::memcpy(dst, src, static_cast<size_t>(crop_w) * 4);
    }

#if defined(__APPLE__) || defined(PULP_HAS_SKIA)
    return encode_png_rgba(cropped.data(), crop_w, crop_h);
#else
    return {};
#endif
}

DiffBounds diff_bounds(
    const std::vector<uint8_t>& reference_png,
    const std::vector<uint8_t>& rendered_png,
    uint8_t tolerance)
{
    DiffBounds bounds;

    auto ref = decode_png(reference_png);
    auto ren = decode_png(rendered_png);
    if (ref.pixels.empty() || ren.pixels.empty()) return bounds;

    uint32_t cmp_w = std::min(ref.width, ren.width);
    uint32_t cmp_h = std::min(ref.height, ren.height);
    if (cmp_w == 0 || cmp_h == 0) return bounds;

    uint32_t min_x = cmp_w;
    uint32_t min_y = cmp_h;
    uint32_t max_x = 0;
    uint32_t max_y = 0;

    for (uint32_t y = 0; y < cmp_h; ++y) {
        for (uint32_t x = 0; x < cmp_w; ++x) {
            size_t ref_idx = (y * ref.width + x) * 4;
            size_t ren_idx = (y * ren.width + x) * 4;

            int dr = std::abs(static_cast<int>(ref.pixels[ref_idx])     - static_cast<int>(ren.pixels[ren_idx]));
            int dg = std::abs(static_cast<int>(ref.pixels[ref_idx + 1]) - static_cast<int>(ren.pixels[ren_idx + 1]));
            int db = std::abs(static_cast<int>(ref.pixels[ref_idx + 2]) - static_cast<int>(ren.pixels[ren_idx + 2]));

            if (dr > tolerance || dg > tolerance || db > tolerance) {
                bounds.diff_pixels++;
                min_x = std::min(min_x, x);
                min_y = std::min(min_y, y);
                max_x = std::max(max_x, x);
                max_y = std::max(max_y, y);
            }
        }
    }

    if (bounds.diff_pixels == 0) return bounds;

    bounds.valid = true;
    bounds.x = min_x;
    bounds.y = min_y;
    bounds.width = max_x - min_x + 1;
    bounds.height = max_y - min_y + 1;
    return bounds;
}

ScreenshotContentStats analyze_screenshot_content(const std::vector<uint8_t>& png) {
    ScreenshotContentStats stats;

    auto img = decode_png(png);
    if (img.pixels.empty()) {
        stats.error = "Failed to decode screenshot image";
        return stats;
    }

    stats.width = img.width;
    stats.height = img.height;
    const auto total_pixels =
        static_cast<uint64_t>(img.width) * static_cast<uint64_t>(img.height);
    if (total_pixels > std::numeric_limits<uint32_t>::max()) {
        stats.error = "Screenshot image exceeds content-stat pixel limit";
        return stats;
    }
    stats.total_pixels = static_cast<uint32_t>(total_pixels);
    if (stats.total_pixels == 0) {
        stats.error = "Zero-size screenshot image";
        return stats;
    }

    std::unordered_map<uint32_t, uint32_t> color_counts;
    color_counts.reserve(std::min<uint32_t>(stats.total_pixels, kMaxTrackedContentColors));

    double luminance_sum = 0.0;
    double luminance_sq_sum = 0.0;
    double alpha_sum = 0.0;
    uint32_t opaque_pixels = 0;
    uint32_t dominant_count = 0;
    uint32_t majority_candidate = 0;
    uint32_t majority_votes = 0;

    for (uint32_t i = 0; i < stats.total_pixels; ++i) {
        const size_t idx = static_cast<size_t>(i) * 4u;
        const auto r = img.pixels[idx];
        const auto g = img.pixels[idx + 1];
        const auto b = img.pixels[idx + 2];
        const auto a = img.pixels[idx + 3];
        const uint32_t key =
            (static_cast<uint32_t>(r) << 24u) |
            (static_cast<uint32_t>(g) << 16u) |
            (static_cast<uint32_t>(b) << 8u) |
            static_cast<uint32_t>(a);
        if (majority_votes == 0) {
            majority_candidate = key;
            majority_votes = 1;
        } else if (majority_candidate == key) {
            ++majority_votes;
        } else {
            --majority_votes;
        }
        auto it = color_counts.find(key);
        if (it != color_counts.end()) {
            const auto count = ++it->second;
            dominant_count = std::max(dominant_count, count);
        } else if (color_counts.size() < kMaxTrackedContentColors) {
            color_counts.emplace(key, 1u);
            dominant_count = std::max(dominant_count, 1u);
        } else {
            stats.unique_colors_capped = true;
        }

        const double luminance = 0.2126 * static_cast<double>(r) +
                                 0.7152 * static_cast<double>(g) +
                                 0.0722 * static_cast<double>(b);
        luminance_sum += luminance;
        luminance_sq_sum += luminance * luminance;
        alpha_sum += static_cast<double>(a);
        if (a >= 250) ++opaque_pixels;
    }

    uint32_t majority_count = 0;
    for (uint32_t i = 0; i < stats.total_pixels; ++i) {
        const size_t idx = static_cast<size_t>(i) * 4u;
        const uint32_t key =
            (static_cast<uint32_t>(img.pixels[idx]) << 24u) |
            (static_cast<uint32_t>(img.pixels[idx + 1]) << 16u) |
            (static_cast<uint32_t>(img.pixels[idx + 2]) << 8u) |
            static_cast<uint32_t>(img.pixels[idx + 3]);
        if (key == majority_candidate) ++majority_count;
    }
    dominant_count = std::max(dominant_count, majority_count);

    stats.unique_colors = static_cast<uint32_t>(color_counts.size());
    stats.luminance_mean = luminance_sum / static_cast<double>(stats.total_pixels);
    const double mean_sq = luminance_sq_sum / static_cast<double>(stats.total_pixels);
    const double variance = std::max(0.0, mean_sq - stats.luminance_mean * stats.luminance_mean);
    stats.luminance_stddev = std::sqrt(variance);
    stats.alpha_mean = alpha_sum / static_cast<double>(stats.total_pixels);
    stats.opaque_coverage =
        static_cast<double>(opaque_pixels) / static_cast<double>(stats.total_pixels);
    stats.non_background_coverage =
        1.0 - (static_cast<double>(dominant_count) / static_cast<double>(stats.total_pixels));
    stats.valid = true;
    return stats;
}

} // namespace pulp::view
