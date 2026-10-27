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
