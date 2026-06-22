#pragma once

#include <pulp/view/view.hpp>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

namespace pulp::view {

enum class ScreenshotBackend {
    default_backend,  ///< Platform default raster (CoreGraphics on Apple).
    coregraphics,     ///< Apple CoreGraphics raster.
    skia,             ///< CPU Skia raster (SkSurfaces::Raster).
    gpu,              ///< Offscreen GPU (Dawn + Skia via pulp::render::HeadlessSurface).
                      ///< The ONLY backend that renders `requires_gpu_host()` views correctly.
    auto_select,      ///< Smart dispatch (see capture_view): native-overlay → refuse;
                      ///< requires_gpu_host → gpu; else → raster.
};

// Render a view tree to a PNG image buffer (headless, no window needed).
//
// Platform support:
//   - macOS: native CoreGraphics-backed capture.
//   - iOS: provider-backed fallback only; no native capture backend yet.
//   - Windows/Linux: built-in Skia raster backend when Skia is available.
//   - Android or builds without a native backend: the host app registers a
//     provider via set_screenshot_provider(). Without one, returns empty
//     vector / false explicitly.
//
// Callers can probe has_screenshot_provider() to distinguish
// "no backend installed" from "render failed".
std::vector<uint8_t> render_to_png(
    View& root,
    uint32_t width,
    uint32_t height,
    float scale = 2.0f,  // Retina scale factor
    ScreenshotBackend backend = ScreenshotBackend::default_backend
);

// Render a view tree to a PNG file
bool render_to_file(
    View& root,
    uint32_t width,
    uint32_t height,
    const std::string& output_path,
    float scale = 2.0f,
    ScreenshotBackend backend = ScreenshotBackend::default_backend
);

// ── Smart, always-honest capture (the "always-capturable" must-have) ─────
//
// One entry point so agents/CI don't pick the wrong backend. capture_view()
// inspects the view tree and does the right thing, and NEVER returns a silent
// blank:
//   1. A subtree marked `contains_native_overlay()` (a WebView / native NSView
//      composited by the OS, not painted into the Pulp canvas) → ok=false with a
//      reason; such overlays are invisible to headless capture (use a real
//      window / OS screencapture).
//   2. `requires_gpu_host()` anywhere in the tree → the `gpu` backend
//      (HeadlessSurface, Dawn+Skia) which renders GPU content correctly.
//   3. Otherwise → CPU raster (`skia` / platform default).
// The captured PNG is gated by the content floor (analyze_screenshot_content):
// a blank / clear-only frame sets ok=false with a reason. `png` may still be
// populated when !ok so callers can save it for debugging.
struct CaptureResult {
    std::vector<uint8_t> png;
    bool ok = false;
    ScreenshotBackend used = ScreenshotBackend::default_backend;
    std::string reason;  ///< Empty when ok; otherwise why the capture isn't trustworthy.
};

CaptureResult capture_view(
    View& root,
    uint32_t width,
    uint32_t height,
    float scale = 2.0f,
    ScreenshotBackend backend = ScreenshotBackend::auto_select  // smart by default
);

// Render a view tree through the offscreen GPU surface (Dawn + Skia via
// pulp::render::HeadlessSurface). Returns empty bytes if this build has no GPU
// backend (PULP_HAS_SKIA off / no pulp-render). Unlike the raster backends,
// this renders `requires_gpu_host()` views correctly.
std::vector<uint8_t> render_to_png_gpu(
    View& root,
    uint32_t width,
    uint32_t height,
    float scale = 2.0f
);

// True when render_to_png_gpu can produce frames in this build (GPU backend
// compiled in). Lets capture_view fall back honestly when GPU is unavailable.
bool has_gpu_capture();

// Raw-RGBA sibling of render_to_png: render a view tree headlessly and hand
// back the decoded pixel buffer instead of PNG bytes — for callers that want
// to composite or upload the frame themselves (e.g. a foreign host that draws
// Pulp's output into its own surface) without paying a PNG encode + decode
// round-trip. The internal Skia raster path already holds these pixels before
// encoding, so this exposes them directly.
//
// Output: tightly packed RGBA8 (R,G,B,A byte order), premultiplied alpha,
// sRGB, top-to-bottom rows, stride == out_width * 4. The pixel dimensions are
// the logical width/height multiplied by `scale`, returned via out_width /
// out_height so the caller can size its buffer exactly. Returns an empty
// vector on failure (no Skia backend, surface alloc failed, read-back failed).
//
// Backend: forces the Skia raster path when available (the only backend that
// produces a stable, host-independent RGBA buffer). Without Skia this returns
// empty (CoreGraphics capture is PNG-only here).
std::vector<uint8_t> render_to_rgba(
    View& root,
    uint32_t width,
    uint32_t height,
    float scale,
    uint32_t* out_width,
    uint32_t* out_height
);

// ── Host-registered screenshot provider (#299) ──────────────────────────
//
// Non-Apple platforms don't have a built-in screenshot backend in
// core/view. A host app (or future platform module) installs a
// provider here — e.g., a Skia-backed raster renderer on Linux/
// Windows — and render_to_png/file will delegate to it. Apple
// platforms' native impls ignore the provider.

using ScreenshotProvider = std::function<std::vector<uint8_t>(
    View& root,
    uint32_t width,
    uint32_t height,
    float scale,
    ScreenshotBackend backend)>;

void set_screenshot_provider(ScreenshotProvider provider);
void clear_screenshot_provider();
bool has_screenshot_provider();

} // namespace pulp::view
