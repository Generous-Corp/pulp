// Canvas — CoreGraphics image-draw fixtures. [#6223 S34]
//
// Before S34, CoreGraphicsCanvas inherited the base no-op draw_image_from_*
// verbs (return false), so ImageView on the macOS CPU paint path rendered each
// image's FILENAME as placeholder text — the "filename-placeholder trap". These
// tests drive CoreGraphics directly: encode a known image to a temp PNG, draw
// it back through CoreGraphicsCanvas, read the pixels, and prove the actual
// image (correct color + orientation) lands — not a fallback.

#ifdef __APPLE__

#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/cg_canvas.hpp>
#include <pulp/canvas/recording_canvas.hpp>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <CoreServices/CoreServices.h>
#include <array>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace pulp::canvas;

namespace {

struct CgPixelGrid {
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    // Sample straight-RGBA in [0,255]. Canvas y=0 is the TOP of the bitmap; the
    // bitmap memory is bottom-up, so flip y (matches CoreGraphicsCanvas ctor).
    std::array<int, 4> at(int x, int y) const {
        const int by = (h - 1) - y;
        const size_t off = (static_cast<size_t>(by) * w + x) * 4u;
        return {pixels[off + 0], pixels[off + 1], pixels[off + 2], pixels[off + 3]};
    }
};

CGContextRef cg_make_bitmap(int w, int h, std::vector<uint8_t>& pixels) {
    pixels.assign(static_cast<size_t>(w) * h * 4u, 0u);
    auto cs = CGColorSpaceCreateDeviceRGB();
    if (!cs) return nullptr;
    const uint32_t bitmap_info =
        static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) |
        static_cast<uint32_t>(kCGBitmapByteOrder32Big);
    CGContextRef ctx = CGBitmapContextCreate(pixels.data(), w, h, 8, w * 4u, cs,
                                             bitmap_info);
    CGColorSpaceRelease(cs);
    return ctx;
}

// Build a 32x32 source image with a red TOP-LEFT quadrant on white, using
// CoreGraphicsCanvas itself (top-down coords), seal it to a CGImage, and write
// it out as a PNG. Returns the temp path (caller unlinks) or "" on failure.
std::string write_source_png() {
    const int W = 32, H = 32;
    std::vector<uint8_t> src_px;
    CGContextRef ctx = cg_make_bitmap(W, H, src_px);
    if (!ctx) return "";
    {
        CoreGraphicsCanvas canvas(ctx, W, H);
        canvas.set_fill_color(Color::rgba(1, 1, 1));  // white bg
        canvas.fill_rect(0, 0, W, H);
        canvas.set_fill_color(Color::rgba(1, 0, 0));  // red top-left quadrant
        canvas.fill_rect(0, 0, W / 2, H / 2);
    }
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!img) return "";

    char tmpl[] = "/tmp/pulp_cg_image_XXXXXX.png";
    int fd = mkstemps(tmpl, 4);
    if (fd < 0) { CGImageRelease(img); return ""; }
    close(fd);
    std::string path = tmpl;

    CFStringRef p = CFStringCreateWithCString(kCFAllocatorDefault, path.c_str(),
                                              kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, p,
                                                 kCFURLPOSIXPathStyle, false);
    CFRelease(p);
    CGImageDestinationRef dest =
        CGImageDestinationCreateWithURL(url, kUTTypePNG, 1, nullptr);
    CFRelease(url);
    if (!dest) { CGImageRelease(img); unlink(path.c_str()); return ""; }
    CGImageDestinationAddImage(dest, img, nullptr);
    bool ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    CGImageRelease(img);
    if (!ok) { unlink(path.c_str()); return ""; }
    return path;
}

std::vector<uint8_t> read_file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool reddish(const std::array<int, 4>& c) {
    return c[0] > 150 && c[1] < 100 && c[2] < 100;
}
bool whitish(const std::array<int, 4>& c) {
    return c[0] > 150 && c[1] > 150 && c[2] > 150;
}

} // namespace

TEST_CASE("CoreGraphicsCanvas reports image-draw capability",
          "[canvas][cg][image][issue-6223]") {
    std::vector<uint8_t> px;
    CGContextRef ctx = cg_make_bitmap(8, 8, px);
    REQUIRE(ctx != nullptr);
    CoreGraphicsCanvas canvas(ctx, 8, 8);
    REQUIRE(canvas.supports_image_draw());
    CGContextRelease(ctx);

    // The base Canvas / RecordingCanvas (no decoder) must report false so
    // headless tooling can warn instead of rendering unfaithfully.
    RecordingCanvas rec;
    REQUIRE_FALSE(rec.supports_image_draw());
}

TEST_CASE("CoreGraphicsCanvas draw_image_from_file paints the image right-side-up",
          "[canvas][cg][image][issue-6223]") {
    const std::string src = write_source_png();
    REQUIRE_FALSE(src.empty());

    const int W = 32, H = 32;
    std::vector<uint8_t> px;
    CGContextRef ctx = cg_make_bitmap(W, H, px);
    REQUIRE(ctx != nullptr);
    {
        CoreGraphicsCanvas canvas(ctx, W, H);
        // measure the intrinsic size (also newly wired on CG).
        float mw = 0, mh = 0;
        REQUIRE(canvas.measure_image_from_file(src, mw, mh));
        REQUIRE(mw == 32.0f);
        REQUIRE(mh == 32.0f);
        REQUIRE(canvas.draw_image_from_file(src, 0, 0, W, H));
    }
    CGContextRelease(ctx);
    CgPixelGrid grid{px, W, H};

    // Red top-left quadrant, white elsewhere — NOT filename text, NOT flipped.
    REQUIRE(reddish(grid.at(8, 8)));      // top-left
    REQUIRE(whitish(grid.at(24, 8)));     // top-right
    REQUIRE(whitish(grid.at(8, 24)));     // bottom-left
    REQUIRE(whitish(grid.at(24, 24)));    // bottom-right

    unlink(src.c_str());
}

TEST_CASE("CoreGraphicsCanvas draw_image_from_data decodes encoded bytes",
          "[canvas][cg][image][issue-6223]") {
    const std::string src = write_source_png();
    REQUIRE_FALSE(src.empty());
    const std::vector<uint8_t> bytes = read_file_bytes(src);
    unlink(src.c_str());
    REQUIRE_FALSE(bytes.empty());

    const int W = 32, H = 32;
    std::vector<uint8_t> px;
    CGContextRef ctx = cg_make_bitmap(W, H, px);
    REQUIRE(ctx != nullptr);
    {
        CoreGraphicsCanvas canvas(ctx, W, H);
        REQUIRE(canvas.draw_image_from_data(bytes.data(), bytes.size(),
                                            0, 0, W, H));
    }
    CGContextRelease(ctx);
    CgPixelGrid grid{px, W, H};
    REQUIRE(reddish(grid.at(8, 8)));
    REQUIRE(whitish(grid.at(24, 24)));
}

TEST_CASE("CoreGraphicsCanvas draw_image_from_file_rect crops the source",
          "[canvas][cg][image][issue-6223]") {
    const std::string src = write_source_png();
    REQUIRE_FALSE(src.empty());

    const int W = 16, H = 16;
    std::vector<uint8_t> px;
    CGContextRef ctx = cg_make_bitmap(W, H, px);
    REQUIRE(ctx != nullptr);
    {
        CoreGraphicsCanvas canvas(ctx, W, H);
        // Crop only the red top-left 16x16 sub-rect and stretch to fill.
        REQUIRE(canvas.draw_image_from_file_rect(src, 0, 0, 16, 16,
                                                 0, 0, W, H));
    }
    CGContextRelease(ctx);
    CgPixelGrid grid{px, W, H};
    // Whole canvas should now be red (we cropped only the red quadrant).
    REQUIRE(reddish(grid.at(4, 4)));
    REQUIRE(reddish(grid.at(12, 12)));

    unlink(src.c_str());
}

TEST_CASE("CoreGraphicsCanvas image verbs fail gracefully on bad input",
          "[canvas][cg][image][issue-6223]") {
    std::vector<uint8_t> px;
    CGContextRef ctx = cg_make_bitmap(8, 8, px);
    REQUIRE(ctx != nullptr);
    CoreGraphicsCanvas canvas(ctx, 8, 8);
    REQUIRE_FALSE(canvas.draw_image_from_file("/no/such/file.png", 0, 0, 8, 8));
    REQUIRE_FALSE(canvas.draw_image_from_file("", 0, 0, 8, 8));
    const uint8_t junk[] = {1, 2, 3, 4};
    REQUIRE_FALSE(canvas.draw_image_from_data(junk, sizeof(junk), 0, 0, 8, 8));
    float mw = 1, mh = 1;
    REQUIRE_FALSE(canvas.measure_image_from_file("/no/such/file.png", mw, mh));
    REQUIRE(mw == 0.0f);
    REQUIRE(mh == 0.0f);
    CGContextRelease(ctx);
}

#endif // __APPLE__
