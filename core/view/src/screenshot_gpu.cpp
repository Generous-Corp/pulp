// Smart, always-honest view capture + the offscreen-GPU capture path.
//
// `render_to_png_gpu` renders a View tree through pulp::render::HeadlessSurface
// (offscreen Dawn + Skia + readback) — the only path that renders
// `requires_gpu_host()` views correctly (CPU raster backends do not). `capture_view`
// is the smart dispatcher: it refuses native-overlay subtrees (WebViews are composited
// by the OS, invisible to headless capture), routes GPU-required trees to the GPU
// backend, raster otherwise, and gates the result on the content floor so a blank /
// clear-only frame is a hard, explained failure instead of a silent pass.
//
// GPU support is compiled in only when pulp-render is linked (PULP_VIEW_HAS_GPU_CAPTURE,
// set by the CMake `if(TARGET pulp-render)` block). Otherwise the GPU path degrades to
// "unavailable" and capture_view falls back to raster honestly.

#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/runtime/log.hpp>

#ifdef PULP_VIEW_HAS_GPU_CAPTURE
#include <pulp/render/headless_surface.hpp>
#include <pulp/canvas/canvas.hpp>
#endif

namespace pulp::view {

namespace {

// Depth-first "does any node in the tree satisfy pred".
template <typename Pred>
bool subtree_any(const View& v, Pred pred) {
    if (pred(v)) return true;
    for (std::size_t i = 0; i < v.child_count(); ++i) {
        const View* c = v.child_at(i);
        if (c && subtree_any(*c, pred)) return true;
    }
    return false;
}

// First view in the tree that owns a native overlay (non-const, so we can call
// its capture_native_overlay_png hook).
View* find_native_overlay(View& v) {
    if (v.contains_native_overlay()) return &v;
    for (std::size_t i = 0; i < v.child_count(); ++i) {
        if (View* c = v.child_at(i)) {
            if (View* found = find_native_overlay(*c)) return found;
        }
    }
    return nullptr;
}

#ifdef PULP_VIEW_HAS_GPU_CAPTURE
// The standard headless paint sequence (mirrors render_to_png_skia): opaque
// background, then layout + paint the tree + overlays.
void paint_root(canvas::Canvas& c, View& root, uint32_t w, uint32_t h) {
    c.set_fill_color(canvas::Color::rgba8(30, 30, 46));
    c.fill_rect(0, 0, static_cast<float>(w), static_cast<float>(h));
    root.set_bounds({0, 0, static_cast<float>(w), static_cast<float>(h)});
    root.layout_children();
    root.paint_all(c);
    View::paint_overlays(c, &root);
}
#endif

}  // namespace

bool has_gpu_capture() {
#ifdef PULP_VIEW_HAS_GPU_CAPTURE
    return true;
#else
    return false;
#endif
}

std::vector<uint8_t> render_to_png_gpu(View& root, uint32_t width, uint32_t height,
                                       float scale) {
#ifdef PULP_VIEW_HAS_GPU_CAPTURE
    pulp::render::HeadlessSurface::Config cfg;
    cfg.width = width;
    cfg.height = height;
    cfg.scale_factor = scale;
    std::string err;
    auto surface = pulp::render::HeadlessSurface::create(cfg, &err);
    if (!surface) {
        runtime::log_warn("render_to_png_gpu: HeadlessSurface unavailable ({})",
                          err.empty() ? "no GPU surface" : err);
        return {};
    }
    return surface->render_png(
        [&](canvas::Canvas& c) { paint_root(c, root, width, height); });
#else
    (void)root; (void)width; (void)height; (void)scale;
    return {};
#endif
}

CaptureResult capture_view(View& root, uint32_t width, uint32_t height, float scale,
                           ScreenshotBackend backend) {
    CaptureResult r;

    // 1. Native overlay (WebView / native NSView). It isn't painted into the Pulp
    //    canvas, so ask the owning view for an in-process snapshot (e.g. WKWebView
    //    takeSnapshot). If one comes back non-blank we're done; only if there's no
    //    in-process snapshot do we refuse (rather than return a silent blank).
    if (View* overlay = find_native_overlay(root)) {
        std::vector<uint8_t> png = overlay->capture_native_overlay_png(width, height);
        if (!png.empty()) {
            const ScreenshotContentStats st = analyze_screenshot_content(png);
            r.png = std::move(png);
            r.used = ScreenshotBackend::default_backend;  // native-overlay snapshot
            if (st.passes_content_floor(/*colors=*/3, /*non_bg=*/0.001, /*opaque=*/0.0)) {
                r.ok = true;
            } else {
                r.reason = "native-overlay (WebView) snapshot came back essentially blank";
            }
            return r;
        }
        r.reason =
            "view contains a native overlay (WebView / native child view) with no "
            "in-process snapshot available — it's OS-composited, not on the Pulp canvas; "
            "use a real window or an OS screencapture";
        return r;  // ok=false
    }

    // 2. Resolve the backend.
    ScreenshotBackend chosen = backend;
    if (chosen == ScreenshotBackend::auto_select) {
        const bool needs_gpu =
            subtree_any(root, [](const View& v) { return v.requires_gpu_host(); });
        chosen = needs_gpu ? ScreenshotBackend::gpu : ScreenshotBackend::skia;
    }
    if (chosen == ScreenshotBackend::gpu && !has_gpu_capture()) {
        runtime::log_warn(
            "capture_view: GPU backend needed but not compiled in; falling back to "
            "raster (a requires_gpu_host view may render incompletely)");
        chosen = ScreenshotBackend::skia;
    }
    r.used = chosen;

    // 3. Render.
    r.png = (chosen == ScreenshotBackend::gpu)
                ? render_to_png_gpu(root, width, height, scale)
                : render_to_png(root, width, height, scale, chosen);
    if (r.png.empty()) {
        r.reason =
            "capture produced no bytes (no screenshot backend available for this "
            "build/platform — register a provider or build with a GPU/Skia backend)";
        return r;  // ok=false
    }

    // 4. Content floor — catch a BLANK / essentially-empty frame, the thing that
    //    silently passed before. Deliberately lenient: a real but sparse UI must
    //    still pass (the strict default floor is for golden-image work, not the
    //    "did anything paint" guard). A native-overlay blank is already refused in
    //    step 1, so here we only reject "nothing but the background fill".
    const ScreenshotContentStats stats = analyze_screenshot_content(r.png);
    if (!stats.passes_content_floor(/*min_unique_colors=*/3,
                                    /*min_non_background_coverage=*/0.001,
                                    /*min_opaque_coverage=*/0.0)) {
        r.reason =
            "captured frame is essentially blank (only the background fill — no widgets "
            "painted); the UI almost certainly did not render";
        return r;  // ok=false, png retained for debugging
    }

    r.ok = true;
    return r;
}

}  // namespace pulp::view
