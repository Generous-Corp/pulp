#include <pulp/view/screenshot.hpp>

#ifdef __APPLE__

#include <pulp/canvas/cg_canvas.hpp>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <Foundation/Foundation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace pulp::view {

std::vector<uint8_t> render_to_png(View& root, uint32_t width, uint32_t height, float scale) {
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

    canvas.set_fill_color(canvas::Color::rgba(30, 30, 46));
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

bool render_to_file(View& root, uint32_t width, uint32_t height,
                    const std::string& output_path, float scale) {
    auto png = render_to_png(root, width, height, scale);
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
std::vector<uint8_t> render_to_png(View&, uint32_t, uint32_t, float) { return {}; }
bool render_to_file(View&, uint32_t, uint32_t, const std::string&, float) { return false; }
}

#endif
