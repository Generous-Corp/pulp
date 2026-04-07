#include <pulp/view/screenshot.hpp>

#ifdef __APPLE__

#include <pulp/canvas/cg_canvas.hpp>
#ifdef PULP_HAS_SKIA
#include <pulp/canvas/skia_canvas.hpp>
#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#endif
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <Foundation/Foundation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace pulp::view {

namespace {

std::vector<uint8_t> encode_rgba_to_png(const uint8_t* pixels,
                                        uint32_t pixel_w,
                                        uint32_t pixel_h,
                                        size_t row_bytes) {
    if (!pixels || pixel_w == 0 || pixel_h == 0) return {};

    @autoreleasepool {
        CGColorSpaceRef color_space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        if (!color_space) return {};

        CGBitmapInfo bitmap_info =
            static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) |
            static_cast<CGBitmapInfo>(kCGBitmapByteOrderDefault);

        CGDataProviderRef provider = CGDataProviderCreateWithData(
            nullptr, pixels, row_bytes * pixel_h, nullptr);
        if (!provider) {
            CGColorSpaceRelease(color_space);
            return {};
        }

        CGImageRef image = CGImageCreate(
            pixel_w, pixel_h,
            8, 32,
            row_bytes,
            color_space,
            bitmap_info,
            provider,
            nullptr,
            false,
            kCGRenderingIntentDefault);

        CGDataProviderRelease(provider);
        CGColorSpaceRelease(color_space);
        if (!image) return {};

        NSMutableData* data = [NSMutableData data];
        CGImageDestinationRef dest = CGImageDestinationCreateWithData(
            (__bridge CFMutableDataRef)data,
            (__bridge CFStringRef)UTTypePNG.identifier,
            1, nullptr
        );
        if (!dest) {
            CGImageRelease(image);
            return {};
        }

        CGImageDestinationAddImage(dest, image, nullptr);
        CGImageDestinationFinalize(dest);
        CFRelease(dest);
        CGImageRelease(image);

        std::vector<uint8_t> result(data.length);
        memcpy(result.data(), data.bytes, data.length);
        return result;
    }
}

std::vector<uint8_t> render_to_png_coregraphics(View& root,
                                                uint32_t width,
                                                uint32_t height,
                                                float scale) {
    uint32_t pixel_w = static_cast<uint32_t>(width * scale);
    uint32_t pixel_h = static_cast<uint32_t>(height * scale);

    CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(
        nullptr, pixel_w, pixel_h, 8, pixel_w * 4,
        colorSpace, kCGImageAlphaPremultipliedLast
    );
    CGColorSpaceRelease(colorSpace);
    if (!ctx) return {};

    CGContextScaleCTM(ctx, scale, scale);

    canvas::CoreGraphicsCanvas canvas(ctx,
        static_cast<float>(width), static_cast<float>(height));

    canvas.set_fill_color(canvas::Color::rgba8(30, 30, 46));
    canvas.fill_rect(0, 0, static_cast<float>(width), static_cast<float>(height));

    root.set_bounds({0, 0, static_cast<float>(width), static_cast<float>(height)});
    root.layout_children();
    root.paint_all(canvas);
    pulp::view::View::paint_overlays(canvas);

    CGImageRef image = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!image) return {};

    @autoreleasepool {
        NSMutableData* data = [NSMutableData data];
        CGImageDestinationRef dest = CGImageDestinationCreateWithData(
            (__bridge CFMutableDataRef)data,
            (__bridge CFStringRef)UTTypePNG.identifier,
            1, nullptr
        );
        if (!dest) { CGImageRelease(image); return {}; }

        CGImageDestinationAddImage(dest, image, nullptr);
        CGImageDestinationFinalize(dest);
        CFRelease(dest);
        CGImageRelease(image);

        std::vector<uint8_t> result(data.length);
        memcpy(result.data(), data.bytes, data.length);
        return result;
    }
}

#ifdef PULP_HAS_SKIA
std::vector<uint8_t> render_to_png_skia(View& root,
                                        uint32_t width,
                                        uint32_t height,
                                        float scale) {
    uint32_t pixel_w = static_cast<uint32_t>(width * scale);
    uint32_t pixel_h = static_cast<uint32_t>(height * scale);
    auto color_space = SkColorSpace::MakeSRGB();
    SkImageInfo info = SkImageInfo::Make(pixel_w, pixel_h, kN32_SkColorType,
                                         kPremul_SkAlphaType, color_space);
    auto surface = SkSurfaces::Raster(info);
    if (!surface) return {};

    auto* sk_canvas = surface->getCanvas();
    if (!sk_canvas) return {};
    if (scale != 1.0f) sk_canvas->scale(scale, scale);

    pulp::canvas::SkiaCanvas canvas(sk_canvas);
    canvas.set_fill_color(pulp::canvas::Color::rgba8(30, 30, 46));
    canvas.fill_rect(0, 0, static_cast<float>(width), static_cast<float>(height));

    root.set_bounds({0, 0, static_cast<float>(width), static_cast<float>(height)});
    root.layout_children();
    root.paint_all(canvas);
    pulp::view::View::paint_overlays(canvas);

    std::vector<uint8_t> pixels(static_cast<size_t>(pixel_w) * pixel_h * 4u);
    SkPixmap pixmap(info, pixels.data(), static_cast<size_t>(pixel_w) * 4u);
    if (!surface->readPixels(pixmap, 0, 0)) return {};

    return encode_rgba_to_png(pixels.data(), pixel_w, pixel_h,
                              static_cast<size_t>(pixel_w) * 4u);
}
#endif

} // namespace

std::vector<uint8_t> render_to_png(View& root,
                                   uint32_t width,
                                   uint32_t height,
                                   float scale,
                                   ScreenshotBackend backend) {
    ScreenshotBackend actual = backend;
    if (actual == ScreenshotBackend::default_backend) {
        actual = ScreenshotBackend::coregraphics;
    }

#ifdef PULP_HAS_SKIA
    if (actual == ScreenshotBackend::skia) {
        return render_to_png_skia(root, width, height, scale);
    }
#endif

    return render_to_png_coregraphics(root, width, height, scale);
}

bool render_to_file(View& root, uint32_t width, uint32_t height,
                    const std::string& output_path, float scale,
                    ScreenshotBackend backend) {
    auto png = render_to_png(root, width, height, scale, backend);
    if (png.empty()) return false;

    @autoreleasepool {
        NSData* data = [NSData dataWithBytes:png.data() length:png.size()];
        NSString* path = [NSString stringWithUTF8String:output_path.c_str()];
        return [data writeToFile:path atomically:YES];
    }
}

} // namespace pulp::view

#else

namespace pulp::view {
std::vector<uint8_t> render_to_png(View&, uint32_t, uint32_t, float, ScreenshotBackend) { return {}; }
bool render_to_file(View&, uint32_t, uint32_t, const std::string&, float, ScreenshotBackend) { return false; }
}

#endif
