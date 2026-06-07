// Cross-platform Skia raster screenshot backend (#3329 Win/Linux parity).
//
// Apple has a native CoreGraphics + Skia screenshot path in
// platform/mac/screenshot_mac.mm. On Windows/Linux there was previously NO
// built-in render backend — render_to_png/file/rgba returned empty unless a
// host registered a ScreenshotProvider (screenshot_stub.cpp). That left the
// foreign-host embed SDK (pulp_embed_render_frame_rgba / _png) unable to
// produce a deterministic frame on non-Apple platforms out of the box.
//
// This file provides the missing built-in backend using Skia's portable raster
// surface (SkSurfaces::Raster — pure CPU, no GPU/llvmpipe required, so it works
// in a headless VM) and SkPngEncoder for the PNG encode. It is the exact same
// raster shape as render_*_skia in screenshot_mac.mm, lifted to a platform-
// neutral C++ TU so Linux + Windows share one implementation.
//
// Compilation: only on non-Apple builds that have Skia (PULP_HAS_SKIA). The
// provider-registration API (set/clear/has_screenshot_provider) stays in
// screenshot_stub.cpp and is always compiled; the stub's render_* fallback
// impls are gated OFF when this file is present (see screenshot_stub.cpp and
// core/view/CMakeLists.txt) so there is exactly one definition.

#include <pulp/view/screenshot.hpp>

#if !defined(__APPLE__) && defined(PULP_HAS_SKIA)

#include <pulp/canvas/skia_canvas.hpp>

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"

#include <fstream>
#include <mutex>
#include <utility>

namespace pulp::view {

// Defined in screenshot_stub.cpp — the host-registered provider lookup. We
// declare it here so the built-in Skia backend can still honor an explicit
// host provider (e.g. a GPU-accurate capture the host wants to override with),
// preserving the pre-existing contract that a registered provider wins.
std::vector<uint8_t> invoke_screenshot_provider(View& root, uint32_t width,
                                                uint32_t height, float scale,
                                                ScreenshotBackend backend,
                                                bool* had_provider);

namespace {

// Paint the view tree into an already-configured SkCanvas. Mirrors the macOS
// raster path: dark fill, lay out at logical size, paint widgets + overlays.
void paint_root(SkCanvas* sk_canvas, View& root, uint32_t width,
                uint32_t height, float scale) {
    if (scale != 1.0f) sk_canvas->scale(scale, scale);

    pulp::canvas::SkiaCanvas canvas(sk_canvas);
    canvas.set_fill_color(pulp::canvas::Color::rgba8(30, 30, 46));
    canvas.fill_rect(0, 0, static_cast<float>(width), static_cast<float>(height));

    root.set_bounds({0, 0, static_cast<float>(width), static_cast<float>(height)});
    root.layout_children();
    root.paint_all(canvas);
    pulp::view::View::paint_overlays(canvas, &root);
}

// Raster-render `root` into a tightly packed pixel buffer of `color_type`.
// Returns empty on failure; writes pixel dims to out_w/out_h on success.
std::vector<uint8_t> raster_render(View& root, uint32_t width, uint32_t height,
                                   float scale, SkColorType color_type,
                                   uint32_t* out_w, uint32_t* out_h) {
    const uint32_t pixel_w = static_cast<uint32_t>(width * scale);
    const uint32_t pixel_h = static_cast<uint32_t>(height * scale);
    if (pixel_w == 0 || pixel_h == 0) return {};

    auto color_space = SkColorSpace::MakeSRGB();
    SkImageInfo info = SkImageInfo::Make(pixel_w, pixel_h, color_type,
                                         kPremul_SkAlphaType, color_space);
    auto surface = SkSurfaces::Raster(info);
    if (!surface) return {};
    auto* sk_canvas = surface->getCanvas();
    if (!sk_canvas) return {};

    paint_root(sk_canvas, root, width, height, scale);

    std::vector<uint8_t> pixels(static_cast<size_t>(pixel_w) * pixel_h * 4u);
    SkPixmap pixmap(info, pixels.data(), static_cast<size_t>(pixel_w) * 4u);
    if (!surface->readPixels(pixmap, 0, 0)) return {};

    if (out_w) *out_w = pixel_w;
    if (out_h) *out_h = pixel_h;
    return pixels;
}

}  // namespace

std::vector<uint8_t> render_to_png(View& root, uint32_t width, uint32_t height,
                                   float scale, ScreenshotBackend backend) {
    // Honor an explicit host-registered provider first (parity with the
    // pre-built-in-backend contract: a host that installed a provider expects
    // it to be used). Falls through to the built-in Skia raster otherwise.
    bool had_provider = false;
    auto via_provider = invoke_screenshot_provider(root, width, height, scale,
                                                   backend, &had_provider);
    if (had_provider) return via_provider;

    // The only built-in non-Apple backend is Skia raster; coregraphics is
    // Apple-only, so any requested backend maps to the Skia path here.
    uint32_t pw = 0, ph = 0;
    auto pixels = raster_render(root, width, height, scale, kRGBA_8888_SkColorType,
                                &pw, &ph);
    if (pixels.empty()) return {};

    SkImageInfo info = SkImageInfo::Make(pw, ph, kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    SkPixmap pixmap(info, pixels.data(), static_cast<size_t>(pw) * 4u);
    // 2-arg pixmap overload (returns sk_sp<SkData>); the 3-arg
    // Encode(SkWStream*, ...) returns bool, so a null first arg there is the
    // wrong overload (matches headless_surface.cpp).
    sk_sp<SkData> png = SkPngEncoder::Encode(pixmap, SkPngEncoder::Options{});
    if (!png || png->isEmpty()) return {};

    const auto* bytes = static_cast<const uint8_t*>(png->data());
    return std::vector<uint8_t>(bytes, bytes + png->size());
}

bool render_to_file(View& root, uint32_t width, uint32_t height,
                    const std::string& output_path, float scale,
                    ScreenshotBackend backend) {
    auto png = render_to_png(root, width, height, scale, backend);
    if (png.empty()) return false;
    std::ofstream out(output_path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(png.data()),
              static_cast<std::streamsize>(png.size()));
    return out.good();
}

std::vector<uint8_t> render_to_rgba(View& root, uint32_t width, uint32_t height,
                                    float scale, uint32_t* out_width,
                                    uint32_t* out_height) {
    if (out_width) *out_width = 0;
    if (out_height) *out_height = 0;
    // Explicit RGBA byte order (R,G,B,A) so the buffer is endianness-
    // independent and host-uploadable, matching render_to_rgba_skia on macOS.
    return raster_render(root, width, height, scale, kRGBA_8888_SkColorType,
                         out_width, out_height);
}

}  // namespace pulp::view

#endif  // !defined(__APPLE__) && defined(PULP_HAS_SKIA)
