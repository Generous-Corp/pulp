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
#include <pulp/runtime/trace.hpp>

#ifdef PULP_VIEW_HAS_GPU_CAPTURE
#include <pulp/render/headless_surface.hpp>
#include <pulp/canvas/canvas.hpp>
#endif

namespace pulp::view {

namespace {

// Single depth-first walk that collects everything capture_view needs to route a
// tree: the first view owning a native overlay (non-const, so we can call its
// capture_native_overlay_png hook) and whether any node requires a GPU host. One
// pass instead of two so a large tree is walked once, not once per predicate.
struct TreeScan {
    View* overlay = nullptr;   ///< First native-overlay-owning view, or null.
    bool needs_gpu = false;    ///< Any requires_gpu_host() in the subtree.
};

void scan_tree(View& v, TreeScan& s) {
    if (!s.overlay && v.contains_native_overlay()) s.overlay = &v;
    if (!s.needs_gpu && v.requires_gpu_host()) s.needs_gpu = true;
    if (s.overlay && s.needs_gpu) return;  // both answered — stop early.
    for (std::size_t i = 0; i < v.child_count(); ++i) {
        if (View* c = v.child_at(i)) {
            scan_tree(*c, s);
            if (s.overlay && s.needs_gpu) return;
        }
    }
}

// The raster backend the smart path falls back to. When this build has a real
// Skia/HeadlessSurface (PULP_VIEW_HAS_GPU_CAPTURE, gated on PULP_HAS_SKIA in
// CMake), the `skia` CPU backend renders correctly — including file images, which
// the macOS CoreGraphics default cannot. Without Skia we must NOT force `skia`
// (it would return empty bytes); defer to the platform default so a host-
// registered ScreenshotProvider can handle the capture.
constexpr ScreenshotBackend raster_fallback() {
#ifdef PULP_VIEW_HAS_GPU_CAPTURE
    return ScreenshotBackend::skia;
#else
    return ScreenshotBackend::default_backend;
#endif
}

// The lenient "did anything paint?" floor used by capture_view. This is NOT the
// strict golden-image floor — a real but sparse UI (few widgets, partly
// transparent / non-opaque background) must still pass; we only reject a frame
// that is nothing but the background fill. NOTE: passes_content_floor's positional
// signature is (min_unique_colors, min_luminance_stddev, min_non_background_coverage,
// min_opaque_coverage) — all four MUST be passed explicitly, otherwise the strict
// defaults (5% non-background, 95% opaque) silently apply and reject sparse UIs.
bool passes_capture_floor(const ScreenshotContentStats& st) {
    return st.passes_content_floor(/*min_unique_colors=*/3,
                                   /*min_luminance_stddev=*/0.0,
                                   /*min_non_background_coverage=*/0.001,
                                   /*min_opaque_coverage=*/0.0);
}

#ifdef PULP_VIEW_HAS_GPU_CAPTURE
// The standard headless paint sequence (mirrors render_to_png_skia): opaque
// background, then layout + paint the tree + overlays.
void paint_root(canvas::Canvas& c, View& root, uint32_t w, uint32_t h) {
    c.set_fill_color(canvas::Color::rgba8(30, 30, 46));
    c.fill_rect(0, 0, static_cast<float>(w), static_cast<float>(h));
    root.set_bounds({0, 0, static_cast<float>(w), static_cast<float>(h)});
    root.layout_children();  // emits the "layout" span (View::layout_children)
    {
        PULP_TRACE_SCOPE_NAMED("canvas", "paint");
        root.paint_all(c);
        View::paint_overlays(c, &root);
    }
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

    // Walk the tree once: find any native overlay AND whether GPU is required.
    TreeScan scan;
    scan_tree(root, scan);

    // 1. Native overlay (WebView / native NSView). It isn't painted into the Pulp
    //    canvas, so ask the owning view for an in-process snapshot (e.g. WKWebView
    //    takeSnapshot). If one comes back non-blank we're done; only if there's no
    //    in-process snapshot do we refuse (rather than return a silent blank).
    if (View* overlay = scan.overlay) {
        std::vector<uint8_t> png = overlay->capture_native_overlay_png(width, height);
        if (!png.empty()) {
            const ScreenshotContentStats st = analyze_screenshot_content(png);
            r.png = std::move(png);
            r.used = ScreenshotBackend::default_backend;  // native-overlay snapshot
            if (passes_capture_floor(st)) {
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
        chosen = scan.needs_gpu ? ScreenshotBackend::gpu : raster_fallback();
    }
    if (chosen == ScreenshotBackend::gpu && !has_gpu_capture()) {
        runtime::log_warn(
            "capture_view: GPU backend needed but not compiled in; falling back to "
            "raster (a requires_gpu_host view may render incompletely)");
        chosen = raster_fallback();
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
    if (!passes_capture_floor(stats)) {
        r.reason =
            "captured frame is essentially blank (only the background fill — no widgets "
            "painted); the UI almost certainly did not render";
        return r;  // ok=false, png retained for debugging
    }

    r.ok = true;
    return r;
}

}  // namespace pulp::view
