// PNG -> RGBA8 decoding for view tests that compare two rendered PNGs.

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

std::vector<uint8_t> decode_png_rgba_impl(const std::vector<uint8_t>& png, size_t& w, size_t& h) {
    std::vector<uint8_t> out;
    CFDataRef data = CFDataCreate(nullptr, png.data(), static_cast<CFIndex>(png.size()));
    CGImageSourceRef src = data ? CGImageSourceCreateWithData(data, nullptr) : nullptr;
    CGImageRef image = src ? CGImageSourceCreateImageAtIndex(src, 0, nullptr) : nullptr;
    if (image) {
        w = CGImageGetWidth(image);
        h = CGImageGetHeight(image);
        out.assign(w * h * 4, 0);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx =
            CGBitmapContextCreate(out.data(), w, h, 8, w * 4, cs,
                                  static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast));
        CGContextDrawImage(ctx, CGRectMake(0, 0, static_cast<CGFloat>(w), static_cast<CGFloat>(h)),
                           image);
        CGContextRelease(ctx);
        CGColorSpaceRelease(cs);
        CGImageRelease(image);
    }
    if (src)
        CFRelease(src);
    if (data)
        CFRelease(data);
    return out;
}

} // namespace

std::vector<uint8_t> decode_png_rgba(const std::vector<uint8_t>& png, size_t& w, size_t& h) {
    return decode_png_rgba_impl(png, w, h);
}

// RGBA8 -> PNG through ImageIO, for tests that regenerate golden images.
std::vector<uint8_t> encode_png_rgba(const std::vector<uint8_t>& rgba, size_t w, size_t h) {
    std::vector<uint8_t> out;
    if (w == 0 || h == 0 || rgba.size() != w * h * 4)
        return out;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx =
        CGBitmapContextCreate(const_cast<uint8_t*>(rgba.data()), w, h, 8, w * 4, cs,
                              static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast));
    CGImageRef image = ctx ? CGBitmapContextCreateImage(ctx) : nullptr;
    CFMutableDataRef data = CFDataCreateMutable(nullptr, 0);
    CGImageDestinationRef dest =
        (image && data) ? CGImageDestinationCreateWithData(data, CFSTR("public.png"), 1, nullptr)
                        : nullptr;
    if (dest) {
        CGImageDestinationAddImage(dest, image, nullptr);
        if (CGImageDestinationFinalize(dest)) {
            const auto* bytes = CFDataGetBytePtr(data);
            out.assign(bytes, bytes + CFDataGetLength(data));
        }
        CFRelease(dest);
    }
    if (data)
        CFRelease(data);
    if (image)
        CGImageRelease(image);
    if (ctx)
        CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    return out;
}
