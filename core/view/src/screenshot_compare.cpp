#include <pulp/view/screenshot_compare.hpp>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <cstring>

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif

namespace pulp::view {

// ── PNG decoding (platform-specific) ────────────────────────────────────

struct RawImage {
    std::vector<uint8_t> pixels;  // RGBA, 4 bytes per pixel
    uint32_t width = 0;
    uint32_t height = 0;
};

#ifdef __APPLE__
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

    img.width = static_cast<uint32_t>(CGImageGetWidth(cgImage));
    img.height = static_cast<uint32_t>(CGImageGetHeight(cgImage));
    img.pixels.resize(img.width * img.height * 4);

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(
        img.pixels.data(), img.width, img.height, 8, img.width * 4,
        cs, kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);

    if (ctx) {
        CGContextDrawImage(ctx, CGRectMake(0, 0, img.width, img.height), cgImage);
        CGContextRelease(ctx);
    }

    CGImageRelease(cgImage);
    return img;
}
#else
static RawImage decode_png(const std::vector<uint8_t>&) {
    return {};  // PNG decoding not available on this platform
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
    std::vector<uint8_t> diff(out_w * out_h * 4, 0);

    for (uint32_t y = 0; y < out_h; ++y) {
        for (uint32_t x = 0; x < out_w; ++x) {
            size_t out_idx = (y * out_w + x) * 4;

            if (x >= cmp_w || y >= cmp_h) {
                // Outside overlap: magenta = size mismatch
                diff[out_idx] = 255; diff[out_idx+1] = 0;
                diff[out_idx+2] = 255; diff[out_idx+3] = 255;
                continue;
            }

            size_t ref_idx = (y * ref.width + x) * 4;
            size_t ren_idx = (y * ren.width + x) * 4;

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

    // Encode diff to PNG using CoreGraphics C API
#ifdef __APPLE__
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(
        diff.data(), out_w, out_h, 8, out_w * 4,
        cs, kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    if (!ctx) return {};

    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!img) return {};

    CFMutableDataRef cf_data = CFDataCreateMutable(nullptr, 0);
    if (!cf_data) { CGImageRelease(img); return {}; }

    CFStringRef png_type = CFSTR("public.png");
    CGImageDestinationRef dest = CGImageDestinationCreateWithData(cf_data, png_type, 1, nullptr);
    if (dest) {
        CGImageDestinationAddImage(dest, img, nullptr);
        CGImageDestinationFinalize(dest);
        CFRelease(dest);
    }
    CGImageRelease(img);

    auto len = static_cast<size_t>(CFDataGetLength(cf_data));
    std::vector<uint8_t> result(len);
    std::memcpy(result.data(), CFDataGetBytePtr(cf_data), len);
    CFRelease(cf_data);
    return result;
#else
    return {};
#endif
}

} // namespace pulp::view
