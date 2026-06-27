// GPU regression for the value-driven silhouette fill on the LIVE Graphite path.
//
// ImageView::set_fill_value() overlays a color masked to the image's own alpha
// via SkiaCanvas::save_layer_with_mask("url(<file>)") → parse_url_image_mask().
// That helper decodes the PNG into a raster-backed SkImage and builds a mask
// shader from it. Skia Graphite (the live Dawn/Metal GPU backend, exercised by
// ScreenshotBackend::gpu) cannot draw a raster-backed image as a shader: it logs
// "Couldn't convert SkImage to a Graphite-backed representation" and silently
// DROPS the masked draw every frame — so the shape never visibly fills on the
// GPU, while the CPU raster screenshot path (recorder_=nullptr) composites fine.
// This was the ELYSIUM "shapes don't fill as you turn the knob" bug.
//
// The fix uploads the image to a GPU texture (SkImages::TextureFromImage) before
// makeShader when a recorder is present. This test pins it: it first proves on
// the raster backend that the fill IS meaningful in this build, then asserts the
// GPU backend composites the same fill (filled != unfilled). Pre-fix the GPU
// filled render equalled the unfilled one; post-fix they differ.
//
// Soft-skips when no raster backend (partial Skia) or no offscreen GPU adapter
// (headless CI lane without a GPU) is available, so those lanes stay green.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/widgets.hpp>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::view;

namespace {

std::string tmp_path(const char* name) {
    const char* dir = std::getenv("TMPDIR");
    std::string base = dir && *dir ? dir : "/tmp/";
    if (base.back() != '/') base.push_back('/');
    return base + name;
}

// Opaque box PNG → a full-box silhouette for the alpha mask (alpha-shaped
// clipping is the Skia mask's job; a full box is enough to prove compositing).
std::string make_source_image() {
    auto box = std::make_unique<View>();
    box->set_background_color(Color::rgba8(210, 90, 90));
    std::string path = tmp_path("pulp-fill-gpu-source.png");
    render_to_file(*box, 48, 48, path, 1.0f, ScreenshotBackend::skia);
    return path;
}

std::unique_ptr<ImageView> make_fill_view(const std::string& img, float value) {
    auto view = std::make_unique<ImageView>();
    view->set_image_path(img);
    if (value >= 0.0f) {
        view->set_fill_value(value);
        view->set_fill_color(Color::rgba(0.2f, 0.45f, 1.0f, 0.85f));
    }
    return view;
}

// Raster (CPU Skia) render — the fill always composites here (recorder=nullptr,
// makeShader on a raster image is fine). Proves the fill is meaningful.
std::vector<uint8_t> render_fill_raster(const std::string& img, float value) {
    auto view = make_fill_view(img, value);
    return render_to_png(*view, 48, 48, 1.0f, ScreenshotBackend::skia);
}

// GPU (offscreen Dawn+Skia Graphite) render via capture_view. NOTE: plain
// render_to_png(..., ScreenshotBackend::gpu) falls through to CoreGraphics on
// mac — only capture_view on a requires_gpu_host tree routes to the real
// Graphite HeadlessSurface (the path that used to drop the masked fill).
std::vector<uint8_t> render_fill_gpu(const std::string& img, float value,
                                     const char* dump = nullptr) {
    auto view = make_fill_view(img, value);
    view->set_requires_gpu_host(true);  // forces capture_view → GPU (Graphite)
    const CaptureResult r = capture_view(*view, 48, 48, 1.0f);
    if (dump && std::getenv("PULP_FILL_GPU_DUMP") && !r.png.empty()) {
        std::ofstream f(tmp_path(dump), std::ios::binary);
        f.write(reinterpret_cast<const char*>(r.png.data()),
                static_cast<std::streamsize>(r.png.size()));
    }
    return r.png;  // empty when no GPU adapter / pulp-render not compiled in
}

}  // namespace

TEST_CASE("ImageView silhouette fill composites on the GPU (Graphite) backend",
          "[view][image][fill][gpu]") {
    const std::string img = make_source_image();

    // 1) Raster baseline — prove the fill is a real, compositable effect in THIS
    // build before holding the GPU path to it. On a partial-Skia runner where
    // the url() mask can't composite, raster filled == raster unfilled; that is
    // an environment limit, not a GPU regression, so skip.
    auto base_raster = render_fill_raster(img, -1.0f);
    if (base_raster.empty()) SKIP("Skia raster screenshot backend unavailable");
    auto filled_raster = render_fill_raster(img, 1.0f);
    REQUIRE_FALSE(filled_raster.empty());
    const auto raster_cmp = compare_screenshots(base_raster, filled_raster);
    REQUIRE(raster_cmp.valid);
    if (raster_cmp.similarity >= 0.999f)
        SKIP("Skia url() image-mask compositing unavailable in this build");

    // 2) GPU (offscreen Dawn+Skia Graphite) via capture_view — the path that
    // used to drop the masked fill. Empty png when no GPU adapter is present
    // (headless CI lane); skip rather than fail there.
    auto base_gpu = render_fill_gpu(img, -1.0f, "fill-gpu-base.png");
    if (base_gpu.empty()) SKIP("offscreen GPU (Graphite) backend unavailable");
    auto filled_gpu = render_fill_gpu(img, 1.0f, "fill-gpu-filled.png");
    REQUIRE_FALSE(filled_gpu.empty());

    const auto gpu_cmp = compare_screenshots(base_gpu, filled_gpu);
    REQUIRE(gpu_cmp.valid);
    // The masked fill uploads to a GPU texture and composites, so the filled GPU
    // render must visibly differ from the unfilled one — exactly as it does on
    // raster. Pre-fix this similarity was ~1.0 (draw dropped).
    CHECK(gpu_cmp.similarity < 0.99f);
}
